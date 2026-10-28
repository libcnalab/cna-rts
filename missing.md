# CNA defects and gaps found while building cna-rts

**Current disposition (2026-10-28):** This is a historical inventory from the September
`cnanext` checkout. The findings were rechecked against current CNA on 2026-10-27:

- Render-target PNG/JPEG saving, EasyGL shader-dialect reporting and direct RGBA byte uploads are
  implemented. CNA commit `3b43bd868` also made the EasyGL C1 render-target effect-source leg use
  a real custom 3D shader, compare ordinary and rendered textures, apply a non-identity uniform
  and declare the `uRtFlipV` row correction. Its focused result was 4/4; the complete registered
  fixture, the resolved-target save tests and `EasyGL_ShaderEffect_GLSL` passed.
- The later 100k/1M failure was a `cna-rts` profile choice, not a CNA defect. Microsoft XNA 4.0
  limits one Reach draw to 65,535 instances and HiDef to 1,048,575. Commit `5b5dc24` selects HiDef
  before device creation and saves the capture RenderTarget2D directly.
- The consumer validation built against that CNA checkout with OPENGLES3. The 10k headless run and
  10k/100k/1M worker-count determinism checks passed. The private GPU run wrote a 640x480 PNG and
  completed 10 measured frames at 10k, 100k and 1M entities with one draw call at every scale.
- `TerrainRenderer.cpp` still fills a reusable 64-row `std::vector<Color>` before each upload.
  Current CNA accepts the same SurfaceFormat::Color payload as exact RGBA bytes, so this may be
  simplified later without changing behavior; it is not a correctness or performance blocker.

The custom 3D result above is an EasyGL result. SDL_GPU's custom-effect slot remains SpriteBatch
only, and no claim about another renderer family follows without its own measured shader contract.
The one observed window-size mismatch below still has no reproduction and therefore no ticket.

Everything below turned up while writing this benchmark against CNA, not from looking for it.
There was no fuzzing and no test-suite run — that is what `cna-killer` is for. These are the
things that made cna-rts refuse to draw, refuse to save a file, or take a detour, plus one gap in
CNA's own test coverage that this project happens to disprove.

**Observed against** `cnanext` at `8cab5f32a` (2026-09-02), renderer `OPENGLES3` (EasyGL),
platform SDL3, Linux/Mesa 25.0.7. Three files were uncommitted in that tree at the time
(`IGraphicsRenderer.hpp`, `TextureCube.hpp`, `EasyGLRenderer.hpp` — another session's
work in progress); none of them touch anything below.

Each item states what was *verified* and what was only *observed*, because the difference matters
to whoever picks these up.

---

## 1. `Texture2D::SaveAsPng` cannot save a `RenderTarget2D` — but `GetData` can

**Severity:** real defect, XNA-compatibility. **Status:** verified in code and at runtime.

`modules/graphics/src/Xna/Texture2D.cpp:2472`

```cpp
void Texture2D::SaveAsPng(const std::string& filename) const
{
    if (!cpuPixels_ || cpuPixels_->empty())
        throw std::runtime_error("Texture2D::SaveAsPng: no CPU-side pixel data available");

    ImageLoader::SavePng(cpuPixels_->data(), width, height, filename);
}
```

It consults only the CPU shadow. A `RenderTarget2D` that has been *rendered into* — and never
written with `SetData` — has no shadow, so this throws for the one texture a game most wants to
save.

What makes it a defect rather than a limitation is that **the readback already exists on the same
class**. `Texture2D::GetData` has an explicit renderer path for exactly this case
(`Texture2D.cpp:1390`):

> `// RenderTarget2D delegates level zero to the renderer even when a CPU shadow exists: a` …

So two readback routes on one object disagree about whether a rendered target's pixels are
reachable. In XNA, `renderTarget.SaveAsPng(stream, w, h)` is the canonical screenshot idiom and
works.

**Reproduction**

```cpp
RenderTarget2D target(device, 800, 600);
device.SetRenderTarget(&target);
device.Clear(Color::CornflowerBlue);
device.SetRenderTarget(nullptr);
target.SaveAsPng("shot.png");   // throws: "no CPU-side pixel data available"
```

**Workaround in use here** (`cna-rts/src/RtsGame.cpp`, `CaptureScreenshot`): read the target back
with `GetData`, push the pixels into a throwaway `Texture2D` with `SetData` so *it* has a shadow,
and encode that. Costs a full round trip and a second full-size texture per capture.

**Suggested fix:** route `SaveAsPng` / `SaveAsJpeg` through the same acquisition `GetData` uses,
so the renderer fallback applies. The throw should survive only for the case `GetData` also
cannot serve.

---

## 2. `GetShaderDialectEXT()` is unimplemented on 35 of 36 renderer families

**Severity:** capability reporting that defeats its own API's purpose. **Status:** verified.

The base class returns `Unknown`
(`modules/graphics/include/CNA/Internal/Renderers/Common/IGraphicsRenderer.hpp:2048`), and exactly
one family overrides it — WebGPU
(`modules/renderers/webgpu/include/CNA/Internal/Renderers/WebGPU/WebGPURenderer.hpp:1343`). Every
other family, **including EasyGL, which backs the default `OPENGLES3` renderer**, answers
`Unknown`.

`ShaderEffect.hpp:71` tells callers to rely on it:

> *"Ask `@ref GraphicsDevice::GetShaderDialectEXT` which dialect the active renderer wants; an
> application that needs both generally supplies two shader sources…"*

A `ShaderEffect` takes GLSL source, and GLSL ES 3.00 and desktop GLSL 3.30 need different
`#version` lines. An application that does what the documentation says gets `Unknown` on the
flagship renderer and has no supported way to pick. cna-rts's first build refused to draw with
*"the active renderer wants a shading language other than GLSL"* — on a renderer that takes GLSL
perfectly well.

**Workaround in use here** (`cna-rts/src/UnitRenderer.cpp`, `ShaderHeaders`): use the dialect when
it is known, fall back to a substring hint from `GetGraphicsRendererName()`, and if both are
useless compile the ES header first and the desktop header second, keeping whichever links.

**Suggested fix:** override it in the GL family — `GlslEs` for `OPENGLES2/3` and `WEBGL1/2`,
`GlslDesktop` for `OPENGL33/4`, and correspondingly in the D3D, Metal and Vulkan families. It is
a one-line override per renderer and it is the difference between a documented capability query
and a coin toss.

---

## 3. `kCustomEffectIn3D` decides nothing — and decides it wrongly

**Severity:** dead conditional in CNA's own test; silent under-coverage. **Status:** verified in
code, and disproved empirically by this project.

`modules/graphics/examples/rendertarget_effect_source_test.cpp:204`

```cpp
constexpr bool kCustomEffectIn3D =
#if defined(CNA_RENDERER_SDL_GPU)
    false;
#else
    false;   // conservatively declared everywhere; C1's SpriteBatch custom-effect leg still runs
#endif
```

Both arms are `false`, so the `#if` selects nothing and the constant is a constant.

Two reasons this is worth a ticket rather than a shrug:

**It is the same defect that was already fixed two constants below.** At line 219, immediately
after this one, `kSecondSampleableFormat` carries the fix note:

> *"plans/plan_runtimerenderer.md RTR-P9-10: this guard used to answer `false` in BOTH arms, so it
> decided nothing…"*

Same shape, same file, ten lines apart, one fixed and one not.

**The value is also factually wrong for EasyGL.** cna-rts binds a custom `ShaderEffect` to an
*instanced* 3D draw — `SetVertexBuffers` with a per-instance stream, then
`DrawInstancedPrimitives` — and it renders correctly at up to a million instances. EasyGL supports
it deliberately: `EasyGLRenderer.cpp:11223` gives a custom effect its own attribute-location convention
(`instanceBaseLocation = params.customEffectRenderer != nullptr ? PerVertexLocationCount(params)
: kStockInstanceBaseLocation`) precisely so a custom effect can define its own instance layout.
The test declares that path unsupported and skips a leg that would pass.

**Suggested fix:** make the guard actually branch — `false` for `SDL_GPU` (where the boundary is
real, since its custom-effect slot lives on the sprite renderer) and `true` for the families that
support it, EasyGL first. Then leg C1 exercises the 3D route it was written for.

---

## 4. No POD upload path for `Color`-format textures

**Severity:** ergonomics and performance, not correctness. **Status:** verified.

`Texture2D::SetData` for a `SurfaceFormat::Color` texture takes `const Color*`
(`Texture2D.cpp:380`), and CNA's `Color` is not XNA's blittable value type — it is a polymorphic
C++ object (`Color.hpp:22`, deriving `IPackedVectorT<UInt32>` and `IEquatable<Color>`), so it
carries a vtable pointer and alignment padding.

Uploading a 2048×2048 terrain texture therefore means materialising **four million polymorphic
objects** to move sixteen megabytes of pixels. `VertexBuffer` has `SetDataRaw`
(`VertexBuffer.hpp:387`) for exactly this reason; `Texture2D` has raw entry points only for
`ByteEXT`, `UShortEXT` and the DXT block formats, none of which is a four-channel colour texture.

**Workaround in use here** (`cna-rts/src/TerrainRenderer.cpp`): upload the texture sixty-four rows
at a time through one reusable `std::vector<Color>`, which bounds the transient allocation but
still constructs every pixel as an object.

**Suggested fix:** a `Texture2D::SetDataRawEXT(int level, const Rectangle*, const void*, int
count, int bytesPerPixel)` mirroring `VertexBuffer::SetDataRaw`. The internal path already works
in bytes — `SetData` copies out of `getRProperty()`/`getGProperty()`/… into a `std::uint8_t`
buffer — so this is exposing a shape that exists rather than adding one.

---

## Observed, not confirmed

- **Window size ignored once.** One run of `cna-rts --width 1600 --height 900` produced a
  1771×900 back buffer (the size was read from `GraphicsDevice::getViewportProperty()`). It did
  not reproduce on later runs with the same flags, and it may well be the window manager rather
  than CNA. Recorded only so it is not lost; not worth a ticket without a repro.

## Deliberately not listed

- **`Color`'s packed constructor being private.** XNA 4.0 has no public `Color(uint)` either —
  `PackedValue` is the setter. CNA is right and my first attempt was wrong.
- **`Rectangle` exposing public `X`/`Y`/`Width`/`Height` instead of properties.** That is XNA's
  own shape.
- **The transient link failure** from `EasyGLTextureCubeRenderer::SetCompressedDataEXT` and
  `EasyGLRenderer::IsCompressedCubeTransferFormatEXT` being declared without definitions. That was
  another session's uncommitted work in flight during this one, not a defect in CNA.
- **`System::Threading::Tasks::Parallel::For` launching one `std::async` per iteration.** It is
  sharp-runtime, not CNA, and its own source documents the choice at length. It is a real trap for
  a 30 Hz inner loop — cna-rts has its own thread pool because of it — but it is a documented
  characteristic rather than a bug.
