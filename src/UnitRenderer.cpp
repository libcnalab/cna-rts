// SPDX-License-Identifier: MIT
#include "UnitRenderer.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "CNA/GraphicsCapability.hpp"
#include "CNA/Internal/Renderers/Common/IGraphicsRenderer.hpp"
#include "Microsoft/Xna/Framework/Graphics/BlendState.hpp"
#include "Microsoft/Xna/Framework/Graphics/BufferUsage.hpp"
#include "Microsoft/Xna/Framework/Graphics/DepthStencilState.hpp"
#include "Microsoft/Xna/Framework/Graphics/IndexElementSize.hpp"
#include "Microsoft/Xna/Framework/Graphics/PrimitiveType.hpp"
#include "Microsoft/Xna/Framework/Graphics/RasterizerState.hpp"
#include "Microsoft/Xna/Framework/Graphics/SetDataOptions.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexBufferBinding.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexDeclaration.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexElement.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexElementFormat.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexElementUsage.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaRts
{
    namespace
    {
        /** @brief One corner of the unit quad. A single Vector2 element, so the per-instance
         *  stream starts at attribute location 1. */
        struct QuadVertex
        {
            float x, y;
        };
        static_assert(sizeof(QuadVertex) == 8);

        const VertexDeclaration& QuadDeclaration()
        {
            static const VertexDeclaration declaration{
                VertexElement(0, VertexElementFormat::Vector2, VertexElementUsage::Position, 0)};
            return declaration;
        }

        /**
         * @brief The per-instance layout.
         *
         * EasyGL gives a `ShaderEffect` its attribute locations by concatenating the bound
         * streams in binding order -- the mesh stream's one element takes location 0, so these
         * two take 1 and 2. (The *stock* shaders instead put an instance matrix at the fixed
         * locations 12..15; a custom effect owns its own convention, which is what lets this
         * stream be eight bytes instead of sixty-four.)
         */
        const VertexDeclaration& InstanceDeclaration()
        {
            static const VertexDeclaration declaration{
                VertexElement(0, VertexElementFormat::NormalizedShort2,
                              VertexElementUsage::TextureCoordinate, 1),
                VertexElement(4, VertexElementFormat::Byte4,
                              VertexElementUsage::TextureCoordinate, 2)};
            return declaration;
        }

        /** @brief 32 palette entries: team-major, one row of eight per team. */
        constexpr int kPaletteEntries = 32;
        constexpr int kSelectedBit = 4;   ///< palette index bit that means "this unit is selected"

        /**
         * @brief Per-archetype quad edge, in world units.
         *
         * Sized against the map's own density rule -- about one unit per one and a half tiles --
         * so a packed army covers a useful fraction of the ground it occupies. Much smaller and a
         * million units read as scattered dust; much larger and neighbouring soldiers merge into
         * one blob and the crowd stops looking like a crowd.
         */
        constexpr float kUnitSizeBase = 0.80f;
        constexpr float kUnitSizeStep = 0.25f;

        constexpr const char* kEsHeader =
            "#version 300 es\nprecision highp float;\nprecision highp int;\n";
        constexpr const char* kDesktopHeader = "#version 330 core\n";

        /**
         * @brief The `#version` lines to try, best guess first.
         *
         * `GraphicsDevice::GetShaderDialectEXT()` is the right question to ask, and most renderers
         * -- EasyGL included -- have not answered it yet: the base class returns `Unknown` and only
         * WebGPU overrides it. So the dialect is used when it is known, the renderer's own name is
         * the fallback hint, and if both come up empty the two GLSL headers are simply tried in
         * turn. Compiling a shader twice at start-up is a cheaper way to find out than refusing to
         * draw on a renderer that would have worked.
         */
        std::vector<const char*> ShaderHeaders(GraphicsDevice& device)
        {
            using CNA::Internal::Renderers::ShaderDialectEXT;
            switch (device.GetShaderDialectEXT())
            {
                case ShaderDialectEXT::GlslDesktop: return {kDesktopHeader};
                case ShaderDialectEXT::GlslEs:      return {kEsHeader};
                case ShaderDialectEXT::GlslVulkan:
                case ShaderDialectEXT::Hlsl:
                case ShaderDialectEXT::Msl:
                case ShaderDialectEXT::Wgsl:
                    // A different language, not a different header. Say so rather than compiling
                    // GLSL into a renderer that cannot read it.
                    return {};
                default: break;
            }

            const std::string_view name = device.GetGraphicsRendererName();
            const bool looksEmbedded = name.find("ES") != std::string_view::npos ||
                                       name.find("WEBGL") != std::string_view::npos ||
                                       name.find("GLES") != std::string_view::npos;
            if (looksEmbedded) return {kEsHeader, kDesktopHeader};
            return {kDesktopHeader, kEsHeader};
        }

        const char* const kVertexBody = R"GLSL(
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec2 aInstancePos;
layout(location = 2) in vec4 aInstanceAttr;

uniform mat4 World;
uniform mat4 View;
uniform mat4 Projection;

uniform vec3 uPalette[32];
uniform vec4 uMapExtent;   // xy: half extent in world units, zw: the same, for the fog lookup
uniform vec4 uSizing;      // x: base size, y: per-archetype step, z: minimum world size, w: roundness
uniform vec4 uFogControl;  // x: fog enabled, y: hide-in-fog enabled
uniform sampler2D uFog;

out vec4 vColor;
out vec2 vCorner;

void main()
{
    vec2 world = (aInstancePos + 1.0) * uMapExtent.xy;

    // Fog of war reaches the vertex stage, not a post pass: an enemy standing in the dark is
    // not dimmed, it is not submitted. Collapsing the quad to a point outside clip space costs
    // one comparison and removes the primitive before rasterisation.
    float hidden = 0.0;
    if (uFogControl.x > 0.5)
    {
        vec2 fogUv = aInstancePos * 0.5 + 0.5;
        float darkness = texture(uFog, fogUv).a;
        hidden = (uFogControl.y > 0.5 && aInstanceAttr.w > 0.5 && darkness > 0.35) ? 1.0 : 0.0;
    }

    float size = max(uSizing.x + uSizing.y * aInstanceAttr.z, uSizing.z);
    vec2 offset = aCorner * size;
    vec4 position = Projection * View * World * vec4(world + offset, 0.0, 1.0);
    gl_Position = mix(position, vec4(2.0, 2.0, 2.0, 1.0), hidden);

    int paletteIndex = int(aInstanceAttr.x + 0.5);
    vec3 tint = uPalette[paletteIndex];
    // Health darkens a unit rather than shrinking it: at two pixels across, a size change is
    // invisible and a brightness change is not.
    float vigour = 0.45 + 0.55 * (aInstanceAttr.y * (1.0 / 255.0));
    // The alpha channel carries the roundness switch rather than an opacity: units are opaque,
    // and this saves a second varying on a path that runs a million times a frame.
    vColor = vec4(tint * vigour, uSizing.w);
    vCorner = aCorner;
}
)GLSL";

        const char* const kFragmentBody = R"GLSL(
in vec4 vColor;
in vec2 vCorner;
out vec4 fragColor;

void main()
{
    // A cheap round body: the quad's corners are cut so a crowd seen from close up reads as many
    // soldiers rather than as a mosaic of squares.
    //
    // The cut is switched OFF once a unit is smaller than a few pixels, and that is not a
    // micro-optimisation -- it is a correctness fix. At one pixel across, whether the pixel
    // centre lands inside the circle depends on the quad's sub-pixel position, so a regular
    // field of units keeps and drops bodies in a repeating pattern and the army renders as a
    // lattice of holes that is not in the simulation at all.
    if (vColor.a > 0.5 && dot(vCorner, vCorner) > 0.25) discard;
    fragColor = vec4(vColor.rgb, 1.0);
}
)GLSL";

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    UnitRenderer::UnitRenderer() = default;
    UnitRenderer::~UnitRenderer() = default;

    bool UnitRenderer::Load(GraphicsDevice& device, std::int32_t maxInstances)
    {
        Unload();
        capacity_ = std::max<std::int32_t>(1, maxInstances);
        diagnostic_.clear();

        if (!device.SupportsCapability(CNA::GraphicsCapability::Instancing) ||
            !device.SupportsCapability(CNA::GraphicsCapability::MultiStreamVertexInput))
        {
            diagnostic_ = "this renderer does not support instanced multi-stream drawing";
            return false;
        }

        const std::vector<const char*> headers = ShaderHeaders(device);
        if (headers.empty())
        {
            diagnostic_ = std::string("renderer '") + std::string(device.GetGraphicsRendererName()) +
                          "' wants a shading language other than GLSL";
            return false;
        }
        for (const char* header : headers)
        {
            auto candidate = std::make_unique<ShaderEffect>(device, std::string(header) + kVertexBody,
                                                           std::string(header) + kFragmentBody);
            if (candidate->IsEffectValid()) { effect_ = std::move(candidate); break; }
            diagnostic_ = "unit shader did not compile: " + candidate->GetCompileErrorEXT();
        }
        if (!effect_)
        {
            return false;
        }
        diagnostic_.clear();

        const QuadVertex corners[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        quad_ = std::make_unique<VertexBuffer>(device, QuadDeclaration(), 4, BufferUsage::WriteOnly);
        quad_->SetDataRaw(corners, 4, static_cast<int>(sizeof(QuadVertex)));

        const std::uint16_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
        indices_ = std::make_unique<IndexBuffer>(device, IndexElementSize::SixteenBits, 6,
                                                 BufferUsage::WriteOnly);
        indices_->SetData(quadIndices, 6);

        instances_ = std::make_unique<DynamicVertexBuffer>(device, InstanceDeclaration(),
                                                           capacity_, BufferUsage::WriteOnly);
        staging_.resize(static_cast<std::size_t>(capacity_));

        ready_ = true;
        return true;
    }

    void UnitRenderer::Unload()
    {
        ready_ = false;
        instances_.reset();
        indices_.reset();
        quad_.reset();
        effect_.reset();
        staging_.clear();
        staging_.shrink_to_fit();
    }

    void UnitRenderer::Build(JobSystem& jobs, const Simulation& simulation, int viewerTeam,
                             int selectedGroup)
    {
        System::Diagnostics::Stopwatch watch = System::Diagnostics::Stopwatch::StartNew();

        const std::int32_t count = std::min(simulation.UnitCount(), capacity_);
        const auto extent = static_cast<float>(simulation.GetTerrain().Size());
        // Q15 over the map: one raw step is extent / 32767 world units, a thirtieth of a tile on
        // the largest map here.
        const float toNormalised = 32767.0f / (extent * 0.5f);

        const Fixed* posX = simulation.PosX();
        const Fixed* posY = simulation.PosY();
        const std::uint8_t* team = simulation.TeamOf();
        const std::uint8_t* kind = simulation.KindOf();
        const std::uint8_t* health = simulation.HealthOf();
        const std::uint8_t* groupOf = simulation.GroupOf();
        UnitInstance* out = staging_.data();

        jobs.ParallelChunks(count, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t i = begin; i < end; ++i)
            {
                const auto ui = static_cast<std::size_t>(i);
                const float nx = (posX[i].ToFloat() - extent * 0.5f) * toNormalised;
                const float ny = (posY[i].ToFloat() - extent * 0.5f) * toNormalised;
                UnitInstance& instance = out[ui];
                instance.x = static_cast<std::int16_t>(std::clamp(nx, -32767.0f, 32767.0f));
                instance.y = static_cast<std::int16_t>(std::clamp(ny, -32767.0f, 32767.0f));

                const int unitTeam = team[ui] & (kMaxTeams - 1);
                int palette = unitTeam * 8 + (kind[ui] % 3);
                if (selectedGroup >= 0 && groupOf[ui] == selectedGroup) palette |= kSelectedBit;
                instance.palette = static_cast<std::uint8_t>(palette);
                instance.health = health[ui];
                instance.size = kind[ui] % 3;
                instance.flags = (viewerTeam >= 0 && unitTeam != viewerTeam) ? 1 : 0;
            }
        });

        stats_.instances = count;
        watch.Stop();
        stats_.buildMs = ElapsedMs(watch);
    }

    void UnitRenderer::Draw(GraphicsDevice& device, const Camera& camera,
                            const Simulation& simulation, const Texture2D* fog, bool fogEnabled)
    {
        stats_.drawCalls = 0;
        stats_.uploadMs = 0.0;
        stats_.drawMs = 0.0;
        if (!ready_ || stats_.instances <= 0) return;

        System::Diagnostics::Stopwatch watch = System::Diagnostics::Stopwatch::StartNew();
        instances_->SetData(staging_.data(), 0, stats_.instances, SetDataOptions::Discard);
        watch.Stop();
        stats_.uploadMs = ElapsedMs(watch);

        watch.Restart();

        // One hue per team, brightened by archetype and again when the group is selected -- so a
        // team still reads as one army however its units differ.
        static const std::array<float, 3> kTeamHue[kMaxTeams] = {
            {0.35f, 0.62f, 1.00f},   // blue
            {1.00f, 0.42f, 0.30f},   // red
            {0.45f, 0.90f, 0.45f},   // green
            {0.42f, 0.42f, 0.46f},   // the fallen
        };

        std::array<float, kPaletteEntries * 3> palette{};
        for (int team = 0; team < kMaxTeams; ++team)
        {
            for (int slot = 0; slot < 8; ++slot)
            {
                const int entry = team * 8 + slot;
                const bool selected = (slot & kSelectedBit) != 0;
                const int archetype = slot & 3;
                // Archetypes differ in brightness, not hue, so a team still reads as one army.
                const float shade = 0.78f + 0.16f * static_cast<float>(archetype) +
                                    (selected ? 0.55f : 0.0f);
                for (int channel = 0; channel < 3; ++channel)
                {
                    palette[static_cast<std::size_t>(entry) * 3 + channel] =
                        std::min(1.0f, kTeamHue[team][static_cast<std::size_t>(channel)] * shade);
                }
            }
        }

        const auto extent = static_cast<float>(simulation.GetTerrain().Size());
        // Never smaller than one and a half screen pixels: at the fully zoomed-out view a unit is
        // well under a pixel across, and a quad that lands between sample points simply
        // disappears -- so the army thins out exactly where it is densest.
        const float minWorldSize = 1.5f / camera.pixelsPerUnit;
        const float largestUnit =
            std::max(kUnitSizeBase + kUnitSizeStep * 2.0f, minWorldSize) * camera.pixelsPerUnit;
        const float roundness = largestUnit >= 5.0f ? 1.0f : 0.0f;

        effect_->setWorldProperty(Matrix::getIdentityProperty());
        effect_->setViewProperty(Matrix::getIdentityProperty());
        effect_->setProjectionProperty(camera.Projection());
        effect_->Apply();

        effect_->SetUniformVec3Array("uPalette", palette.data(), kPaletteEntries);
        effect_->SetUniformVec4("uMapExtent", extent * 0.5f, extent * 0.5f, 0.0f, 0.0f);
        effect_->SetUniformVec4("uSizing", kUnitSizeBase, kUnitSizeStep, minWorldSize, roundness);
        const bool useFog = fogEnabled && fog != nullptr;
        effect_->SetUniformVec4("uFogControl", useFog ? 1.0f : 0.0f, useFog ? 1.0f : 0.0f,
                                0.0f, 0.0f);
        effect_->SetUniformInt("uFog", 0);
        if (useFog) effect_->SetTexture(0, *const_cast<Texture2D*>(fog));

        device.setBlendStateProperty(BlendState::Opaque);
        device.setDepthStencilStateProperty(DepthStencilState::None);
        device.setRasterizerStateProperty(RasterizerState::CullNone);

        std::vector<VertexBufferBinding> bindings;
        bindings.emplace_back(quad_.get(), 0, 0);
        bindings.emplace_back(instances_.get(), 0, 1);
        device.SetVertexBuffers(bindings);
        device.setIndicesProperty(indices_.get());

        device.DrawInstancedPrimitives(PrimitiveType::TriangleList, 0, 0, 4, 0, 2,
                                       stats_.instances);
        stats_.drawCalls = 1;

        watch.Stop();
        stats_.drawMs = ElapsedMs(watch);
    }
}
