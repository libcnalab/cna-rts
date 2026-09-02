# Changelog

All notable changes to cna-rts are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-02

First release: a complete RTS scaling benchmark on CNA, from map generation to the markdown table.

### Added

- **Deterministic simulation.** Q16.16 fixed-point arithmetic throughout, a fixed 30 Hz time step,
  double-buffered positions, a work partition that does not depend on the worker count, and an
  order-free state hash. `--verify` proves the result is identical across different worker counts;
  `--record` / `--replay` reproduce a whole run from its tick-stamped order log.
- **GPU instancing.** One quad and one `DrawInstancedPrimitives` call for the entire army, driven
  by an eight-byte-per-unit instance stream and a custom GLSL `ShaderEffect`. The shader header is
  chosen from `GetShaderDialectEXT()` where a renderer declares one, and by trying ES 3.00 then
  desktop 3.30 where none does.
- **Spatial partitioning.** A uniform grid rebuilt every tick by parallel counting sort, scattering
  positions alongside indices so neighbour reads stay sequential inside a cell, and tallying each
  cell's per-team population for fog of war, combat and the minimap.
- **Hierarchical pathfinding.** Sector portals with precomputed within-sector distance matrices
  (built in parallel at map load), A\* over the portal graph, per-sector flow fields along the
  answer, and a sector-level breadth-first sweep that gives every off-corridor sector a route back
  to the plan.
- **Formations.** Line, column, wedge, box and circle as a pure function of a unit's index, with
  the slot count capped so a hundred-thousand-unit group has a shape rather than a map-wide
  envelope.
- **Fog of war.** Per-team visibility rasterised from grid cell occupancy by a chamfer distance
  transform, so its cost is independent of the unit count. Sampled in the vertex shader, so hidden
  enemies are never submitted.
- **Combat and reinforcement.** Attrition from the enemy population of a unit's own cell, with
  respawns that hold the entity count constant so a long benchmark measures throughput rather than
  its own attrition rate.
- **Instrumented HUD.** Per-phase simulation timings, instance build and upload cost, draw calls,
  stream size, the live state hash, a frame-time histogram, and a minimap with the view rectangle.
- **Benchmark harness.** `--bench` sweeps unit counts on screen with exactly one simulation tick
  per frame and prints a markdown table; `--sim-only` measures the simulation alone with no window;
  `--screenshot` writes a PNG of a chosen frame.

### Fixed during development

These were real defects found by running the thing, and each is worth recording because the
symptom did not obviously point at the cause.

- **Job system: a worker could run the wrong batch's body.** Reading the body pointer once and then
  claiming chunks from an atomic counter let a descheduled worker wake into the *next*
  `ParallelChunks` call, claim one of its chunks, and run it through the previous call's function
  object — a destroyed temporary in a frame that had already returned. AddressSanitizer caught it
  as a stack-use-after-scope. The claim and the body pointer are now read together under the lock,
  and the generation is checked at claim time.
- **State hash depended on the worker count.** Chaining the per-chunk hashes made the result a
  function of how the work was divided, so the determinism check failed between an eight-core and a
  sixteen-core run for a reason that had nothing to do with the simulation. Per-unit hashes are now
  summed, which is associative and commutative, and each term carries its own unit index.
- **Terrain rendered pure white.** The colour dither multiplied a `std::size_t` tile index by a
  32-bit constant; at 64 bits the product does not wrap, so the shift meant to yield 0–7 returned
  thousands and every tile past the first saturated to white. The index is now narrowed before the
  multiply.
- **Armies crawled at 40% of their own speed.** A group's formation envelope was sized from its
  full membership, so a five-thousand-unit line was a hundred tiles wide and the group was always
  *inside* its own envelope — steering at formation slots instead of marching. Formation slots are
  now capped at 2048 and shared beyond that.
- **The crowd would not march.** Separation summed over six neighbours reached three times the
  length of the goal direction it was added to, so a dense army churned in place. The total crowd
  push is now capped below the goal direction.
- **Units rendered as a lattice of holes.** The round-body cutout discards fragments outside the
  quad's inscribed circle; at one pixel across, whether the pixel centre falls inside depends on
  sub-pixel position, so a regular field of units kept and dropped bodies in a repeating pattern.
  The cutout is switched off below a few pixels and the minimum quad size is clamped to one and a
  half pixels.
- **Unattended runs picked up the desktop's input.** Captures came back with the help overlay open
  and the zoom changed. Input is now read only while the game window has focus.
- **Armies were drawn up in opposite corners and never met.** The map grows with the army to hold
  density constant, so at a million units a corner-to-corner march takes ten minutes of wall clock:
  nothing ever fought, and the case the benchmark exists to measure was never reached. The two
  sides now start in facing bands with a narrow no-man's-land between them.
- **A formation's footprint did not scale with its group.** Capping the slot count fixed the
  envelope but left the spacing at two thirds of a tile, so two thousand slots described a patch
  eight tiles deep into which a hundred and twenty-five thousand units crammed at a hundred and
  thirty per tile — an army that should cover a province rendered as one bright line. Spacing is
  now derived from the group's own footprint at the map's density.
- **The formation slot lookups were O(sqrt(n)) searches.** Invisible until the spacing fix put
  every unit inside its envelope, at which point the wedge's and circle's ring walks ran a million
  times a tick and cost ten milliseconds. Both are closed-form now: a wedge's rank is an integer
  square root, a circle's ring is the root of a quadratic.
- **The circle formation was an octagon.** Interpolating between eight cardinal unit vectors is
  7.6% short at the midpoint. Replaced with an exact 33-entry quarter-turn cosine table, giving
  128 directions that are each a genuine unit vector — and, unlike calling `std::cos` at start-up,
  identical on every platform.
- **Unattended captures picked up the desktop's mouse wheel.** Input is now ignored entirely while
  a `--screenshot` capture is pending, so a capture's framing is whatever `--zoom` and `--follow`
  say and nothing else.
- **The movement pass was memory-bound on neighbour reads.** Following each neighbour index into
  the position arrays cost several million scattered reads a tick — about 60 ms at a million units.
  The spatial sort now writes an interleaved copy of the positions alongside the indices, and the
  neighbour sample is a contiguous run centred on the unit's own slot; passability moved to a
  bitset and sector size to a power of two at the same time. Movement fell from 92 ms to about
  14 ms per tick at a million units.
