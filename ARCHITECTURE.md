# How cna-rts works

This is the design document: what each subsystem does, and — more usefully — *why it is shaped
the way it is*, since almost every choice here was forced by the scale rather than chosen for
elegance. At ten thousand units nearly anything works. At a million, most of the obvious answers
are arithmetically impossible, and the ones that survive are not the ones a small-scale RTS would
have picked.

The numbers quoted below were measured on the machine described in the README's table caption.
They are there to show the shape of the cost, not as a spec.

---

## The two clocks

The simulation advances in whole ticks at a fixed 30 Hz. The renderer draws whenever it can. A
frame therefore contains zero, one, or several ticks, and at a million units the honest answer is
usually "one tick every other frame".

They are separate on purpose. A variable time step is not reproducible — the same seed on a faster
machine takes different-sized steps and diverges — and reproducibility is the point of the whole
`--verify` / `--record` / `--replay` apparatus. A fixed step that also paced the renderer would
report the target frame rate instead of the achieved one, which is the one thing a benchmark may
never do.

`--bench` mode pins the ratio: **exactly one simulation tick per rendered frame**. That is what
makes the table's FPS column mean "frames per second with the simulation running", rather than
"frames per second of a paused world".

---

## Determinism

`Fixed` (`src/Fixed.hpp`) is a Q16.16 signed fixed-point scalar, and it is the simulation's only
numeric type. Positions, velocities, steering, formation offsets — all integers.

Floating point would be *reproducible* on one binary and one machine. It stops being reproducible
the moment a compiler contracts a multiply-add, vectorises a reduction into a different summation
order, or a different libm rounds `sqrt` one ulp the other way. In an RTS a one-ulp divergence at
tick 40 is a different battle by tick 4000. `a * b >> 16` has no such freedom.

Three more rules hold the line:

- **Positions are double-buffered.** A unit reads its neighbours' positions to steer away from
  them, so writing a new position into the array another thread is still reading would make the
  result depend on thread timing. Everything else a unit writes is its own and is written in
  place.
- **The work partition is fixed, not stolen.** `JobSystem::ParallelChunks` always cuts a range
  into `WorkerCount()` contiguous blocks at the same boundaries. *Which* thread runs a block is
  free (chunks are claimed from a shared counter, so a fast worker helps a slow one); *what is in*
  a block is not.
- **Every reduction is order-free.** The state hash sums per-unit hashes rather than chaining
  per-chunk ones, because a chained hash depends on how many chunks there were and would then
  differ between an eight-core and a sixteen-core machine — reporting nondeterminism that is not
  there and hiding the kind that is. Each term carries its own unit index, so the sum is a
  function of the state alone.

`--verify` runs the same seed twice *with deliberately different worker counts* and compares. Two
runs at the same worker count would prove almost nothing.

---

## Entities

Parallel arrays, not objects. The movement pass touches position and velocity and nothing else; a
struct-of-entities layout would drag health, team, kind and padding through the cache with them.

A unit's group is contiguous: group `g` owns units `[firstUnit, firstUnit + unitCount)`. That
makes the formation slot derivable from the unit index with no lookup, and keeps the group's data
hot while a chunk walks through it.

---

## Spatial partitioning — `SpatialGrid`

A uniform grid over the map, **rebuilt from scratch every tick by counting sort**.

Rebuilding beats updating here. An incremental structure — a quadtree, or a grid with per-cell
insert and remove — pays a pointer chase and a branch per moved entity, and fragments as the swarm
flows across the map. A counting sort is a few linear passes over flat arrays and leaves every
cell's members *contiguous*, which is what makes the neighbour queries cache-friendly at a million
entities.

Five passes, all parallel except the prefix sum, which is a serial dependency by definition:

1. bucket every unit, counting into that chunk's own row of the count table;
2. sum the per-chunk rows into per-cell totals;
3. exclusive prefix over cells, then turn each chunk's count into that chunk's write cursor —
   chunk *c*'s block inside a cell begins after every lower-numbered chunk's, which is what makes
   the sorted order depend on the unit index alone;
4. scatter the indices **and the positions alongside them**;
5. tally each cell's per-team population.

Pass 4 writing a second copy of the positions is the single largest measured win in the program.
With only the index list, the movement pass followed each neighbour into the position arrays at a
scattered offset — several million cache misses a tick, about **60 ms** at a million units. Writing
the positions out during a scatter that is already touching those cache lines made every neighbour
read sequential within its cell.

Pass 5's tally is what fog of war and the minimap read instead of walking a million units of their
own.

---

## Movement — `Simulation::MoveUnits`

The hot loop. Per unit, per tick:

**Where does it want to be?** Inside the formation envelope, at its own formation slot. Outside it,
the flow-field direction for its sector; failing that, the sector's guidance tile.

**Crowd pressure.** Two contributions. A density gradient read straight off the grid — four
neighbouring cell populations, normalised by the local crowd through a reciprocal *lookup table*
so the normalisation costs no division. And a handful of real neighbours.

**No square roots, no divisions in the inner loop.** The neighbour loop runs
`unitCount × neighbourSamples` times a tick — six million times at the top scale. The *direction*
of a separation push is quantised to eight compass points by sign tests; its *magnitude* comes out
of a 64-entry table indexed by the squared distance shifted down. At a scale where a unit is two
pixels across, the visual difference from exact normalisation is precisely none.

**Neighbours are read as one contiguous run.** The sample is the unit's own neighbourhood *inside
its cell's slice of the sorted array*, centred on where the sort put it. Striding across the whole
cell would sample more evenly and would cost a modulo per unit plus a cache miss per neighbour.
What this gives up is that partners are cell-mates with nearby unit indices rather than an even
spread of the cell — at forty units per cell and six samples, a distinction without a visible
difference.

**The crowd push is capped below the goal direction.** Without the cap an army does not march.
Separation is summed over several neighbours, and a packed cell holds dozens of them, so the sum
reaches three times the length of the vector it is added to — and a unit whose steering is three
parts "get away from these people" and one part "walk east" churns in place. Capped, crowd
behaviour becomes a correction rather than a destination.

**Terrain is a bitset.** Passability is asked two or three times per unit at scattered tile
coordinates. At one byte per tile the million-unit map is 1.6 MB and most reads miss; at one bit it
is 200 KB and most reads are served by L2. Sector size is forced to a power of two for the same
kind of reason: `SectorIndex` becomes a shift instead of two integer divisions, a million times a
tick.

Collision is axis-separated, so a unit that walks into a cliff slides along it rather than stopping
dead — the difference between an army flowing around an obstacle and an army piling against it.

---

## Hierarchical pathfinding — `SectorGraph`, `FindCorridor`, `GroupNavigation`

Per-unit A* at this scale is not slow, it is impossible: a million units repathing once a second at
even ten microseconds each is ten seconds of work per second. So a path query answers for a
**group**, and it answers in three layers.

**Layer 1 — the abstract graph, built once per map.** The map is cut into sectors. Along each
sector border, every maximal run of mutually passable tiles becomes a *portal*. Within each sector,
one integration field per portal gives the true within-sector distance to every other portal, and
those distances are the abstract graph's edges. Every sector is independent, so the whole build is
one parallel pass — the most parallel phase in the program, and the reason map load is not a
visible pause.

A portal is a **span, not a point**, and both facts matter. The abstract A* needs one
representative tile so the graph has finite nodes. The flow field that actually moves the units
seeds from the *whole span*, because seeding from the representative alone aims every unit in the
sector at a single tile and turns a doorway sixteen tiles wide into a one-tile queue.

**Layer 2 — A\* over portals.** Two tile-level integrations per query, both confined to one sector
(start tile to the start sector's portals; the goal sector's portals to the goal tile), then a
heap search over a few dozen portal nodes with an octile heuristic. This is the hierarchical
bargain made concrete: a query looks at a few dozen nodes instead of four million tiles.

**Layer 3 — flow fields along the corridor.** For each sector the answer crosses, one integration
field seeded from the exit portal's span (or from the goal tile in the last sector), converted to
one direction byte per tile. Units then spend a single array lookup per tick to know which way to
walk.

**Units that fall off the corridor are not abandoned.** A breadth-first sweep over the sector
adjacency graph gives *every* sector on the map a guidance tile — the doorway that leads back
toward the plan — so a unit shoved out of the planned route by the crowd walks back into it instead
of pressing against a cliff forever.

Integration itself uses **Dial's algorithm**, not a binary heap: every edge costs 10 or 14, so a
bucket ring of sixteen is a constant-time priority queue where a heap would pay a log. The region
is small, it is integrated thousands of times while a map loads, and that constant is the
difference between a snappy start-up and a visible pause.

Path queries are budgeted (`--paths-per-tick`, default 1) so a burst of orders spreads over
several ticks instead of landing on one.

---

## Formations

A slot offset is a pure function of `(kind, slot, total, spacing)` — no assignment pass, no state,
no allocation. A unit derives its slot from its own index every tick.

Two things scale, and they scale differently. The slot count is **capped at 2048** however large
the group is; the *spacing between slots* grows instead, sized so the formation's footprint matches
the ground the group actually needs at the map's density — about one and a half tiles per unit.

The cap comes first. Without the cap, shape and scale
collide: a hundred and twenty-five thousand units in a single rank is a line four hundred tiles
wide, wider than the map, and a group whose formation envelope is that big is *always* inside it,
so it never marches — it only spreads. That was a real bug here, and the symptom was armies that
crawled at forty percent of their own speed. With the cap, units past 2048 share a slot, which at
these densities is exactly right: sixty units on one slot pile up around it and separation spreads
them, so the formation reads as the shape of the whole army rather than as one soldier per
position.

The spacing has to grow, though, or the cap creates a second bug in the shape of the first. Two
thousand slots at two thirds of a tile describe a patch eight tiles deep; a hundred and twenty-five
thousand units crammed into it is a hundred and thirty units per tile, and an army that should look
like a horde across a province renders as one bright line. `FormationSpacingFor` sizes the
formation from the group's own footprint instead, so a 125k group in a box occupies about
430 × 430 tiles.

One consequence worth naming: because the map is sized to hold exactly this density, eight groups
of 125k each occupy roughly the whole map at the top scale, so at 1M nearly every unit is inside
its formation's envelope at all times. Pathfinding does most of its visible work at the smaller
scales, where there is room to march.

And the slot lookups have to be **closed-form**. Both the wedge and the circle originally searched
for their rank by walking outward one ring at a time — `O(sqrt(slot))`, up to forty-five
iterations — which was invisible until the spacing change put every unit inside its envelope and
the search started running a million times a tick. It measured ten milliseconds. Rank *r* of a
wedge starts at slot *r²*, so the rank is an integer square root; ring *r* of a circle starts at
`1 + 3r(r-1)`, so the ring is the root of a quadratic. Neither needs a loop.

---

## Fog of war

**Vision is not stamped per unit.** A million circular stamps would be a hundred million writes a
tick and would spend nearly all of them re-lighting a tile a thousand neighbours had already lit.

What is stamped is *occupancy*: the spatial grid's per-cell team tally already exists, so fog of
war starts from a grid of ones and zeroes whose size is the **cell count** — independent of the
unit count. Vision radius is then a chamfer distance transform over that grid: two raster sweeps
with a 5/7 mask that approximates Euclidean distance to within about 2%, and a cell is visible when
its distance to the nearest occupied cell is inside the radius. `O(cells)` with a constant of about
eight, whatever the radius and however many units produced the occupancy.

Teams are independent, so the teams are the parallel axis; the sweeps inside a team are
raster-ordered and stay serial by nature.

The result is one small texture. The renderer draws it over the ground as darkness — and the unit
shader **samples it in the vertex stage**, so an enemy standing in the dark is not dimmed, it is
not submitted: the quad collapses outside clip space and the primitive never reaches
rasterisation.

---

## GPU instancing — `UnitRenderer`

One quad, four vertices, six indices, bound once. One per-instance stream with up to a million
entries. **One `DrawInstancedPrimitives` call**, two million triangles, no per-unit CPU work in the
draw at all.

The only per-frame cost that scales with the army is filling and uploading the stream, so the
stream's width *is* the frame's memory bandwidth. It is **eight bytes**:

| bytes | field | why |
| --- | --- | --- |
| 4 | position, two normalised 16-bit integers | over a 2048-tile map that is a thirtieth of a tile — far below one screen pixel at any zoom this benchmark uses |
| 1 | palette index | the colour itself lives in a 32-entry uniform array the shader reads, which is what buys four bytes back |
| 1 | health | darkens the unit; at two pixels across a size change would be invisible and a brightness change is not |
| 1 | archetype size | |
| 1 | flags | bit 0: hide where the viewing team has no vision |

Twelve bytes would be 12 MB a frame at a million units; a 64-byte world matrix — what a
general-purpose instanced renderer hands the GPU, and what CNA's own `InstancedRendererEXT`
uploads — would be 64 MB a frame, more than an integrated part's bandwidth budget at 60 Hz. Eight
bytes is 8 MB, which fits.

**Attribute locations.** EasyGL gives a `ShaderEffect` its attribute locations by concatenating the
bound streams in binding order, so the mesh stream's one element takes location 0 and the two
instance elements take 1 and 2. (The *stock* CNA shaders instead put an instance matrix at the
fixed locations 12–15; a custom effect owns its own convention, which is exactly what lets this
stream be eight bytes instead of sixty-four.)

**The round cutout is switched off below a few pixels.** That is a correctness fix, not an
optimisation. At one pixel across, whether the pixel centre lands inside the circle depends on the
quad's sub-pixel position, so a regular field of units keeps and drops bodies in a repeating
pattern and the army renders as a lattice of holes that is not in the simulation at all. The
minimum quad size is likewise clamped to one and a half screen pixels, so the army does not thin
out exactly where it is densest.

**The shader is GLSL, chosen by trying.** `GraphicsDevice::GetShaderDialectEXT()` is the right
question and most renderers — EasyGL included — have not answered it yet: the base class returns
`Unknown` and only WebGPU overrides it. So the dialect is used when it is known, the renderer's own
name is the fallback hint, and if both come up empty the ES 3.00 and desktop 3.30 headers are
simply tried in turn. Compiling a shader twice at start-up is a cheaper way to find out than
refusing to draw on a renderer that would have worked.

---

## Ground, fog and minimap — `TerrainRenderer`

All three are one textured quad each, drawn through `SpriteBatch`. The instanced path exists for
the million things, not for the three.

The terrain texture is built once and uploaded a band at a time through one reusable buffer. A
single upload would need a four-million-element `Color` array, and CNA's `Color` is a polymorphic
C++ object rather than the blittable value type XNA has — so that array would be tens of megabytes
of vtable pointers on the way to sixteen megabytes of pixels.

Fog and minimap textures are rebuilt on the simulation's fog interval, not every frame, because
neither changes faster than that. The minimap's static terrain layer is averaged per grid cell
once at load; per refresh it only composites the armies and the fog, and it reads the armies from
the grid's tally rather than from the units — the same trick fog of war uses, for the same reason.

---

## Text — `TextRenderer`

`SpriteFont` is CNA's real text type and the right one for a game, but it wants a glyph texture and
a metrics table from the content pipeline, and a benchmark that should be cloneable and runnable
must not need a content build to print its own frame rate. The font is ninety-five 5×7 glyphs
written as ASCII art in the `.cpp` and baked into one small texture at start-up — spelled out
rather than packed into hex because a font is the one table where a typo is invisible in the source
and obvious on the screen, and in this shape it is obvious in both.

---

## Threading — `JobSystem`

A persistent pool with a fixed partition, for two reasons.

**No thread is created inside a tick.** `System::Threading::Tasks::Parallel::For` —
sharp-runtime's own, faithfully ported from .NET — launches one `std::async` per iteration,
batched to the degree of parallelism. That is right for the coarse work the API is written for, and
it is roughly thirty microseconds of thread creation per chunk per phase here: five phases at 30 Hz
across sixteen chunks is milliseconds of pure overhead inside a 33-millisecond budget. This pool
pays that cost once, at startup.

**The claim and the function pointer are read together, under the lock.** The obvious lock-free
version — read the body pointer once, then claim chunks with an atomic counter — is wrong in a way
that only shows up under load, and it showed up here: a worker that reads the pointer, is
descheduled, and wakes after the caller has moved on to the *next* `ParallelChunks` call will claim
a chunk of the new batch and run it through the old batch's function object, which by then is a
destroyed temporary in a frame that has returned. Checking the generation at claim time rules that
out — a successful claim implies the caller is still blocked inside the call that lent us the
function.

---

## Where the time goes

At a million units on the reference machine, a simulation tick is roughly:

| phase | ms | note |
| --- | --- | --- |
| movement | 17.6 | the hot loop; about a third of it is the six neighbour samples |
| grid rebuild | 4.5 | five passes; the count table is `workers × cells` |
| combat + centroids | 0.9 | one cell lookup per unit |
| state hash | 0.8 | two hashes per unit |
| path queries | 0.1 | amortised: at most one group replans per tick |
| fog of war | 0.1 | amortised: it runs every fourth tick |
| **total** | **24.0** | 41.7 ticks per second, against a 30 Hz target |

And a frame adds the instance build, the upload, and one draw call. The numbers in the README's
tables are the measured versions of this.
