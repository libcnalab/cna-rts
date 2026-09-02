// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Microsoft/Xna/Framework/Graphics/DynamicVertexBuffer.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/IndexBuffer.hpp"
#include "Microsoft/Xna/Framework/Graphics/ShaderEffect.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexBuffer.hpp"

#include "Camera.hpp"
#include "JobSystem.hpp"
#include "Simulation.hpp"

namespace CnaRts
{
    /**
     * @brief One unit on the GPU: eight bytes, and every one of them earned.
     *
     * At a million units the instance stream is uploaded whole every frame, so its width *is* the
     * frame's memory bandwidth: twelve bytes would be 12 MB a frame, and a 64-byte world matrix
     * -- which is what a general-purpose instanced renderer hands the GPU -- would be 64 MB a
     * frame, more than the discrete bandwidth budget of an integrated part at 60 Hz. Eight bytes
     * is 8 MB, which fits.
     *
     * Position is a pair of normalised 16-bit integers rather than floats: over a 2048-tile map
     * that is a thirtieth of a tile, far below one screen pixel at any zoom this benchmark uses.
     * Colour is not carried at all -- `palette` indexes a 32-entry uniform array the shader reads,
     * which is what buys the other four bytes back.
     */
    struct UnitInstance
    {
        std::int16_t x = 0;         ///< normalised to [-32767, 32767] across the map
        std::int16_t y = 0;
        std::uint8_t palette = 0;   ///< team * 8 + kind, plus the selected bit
        std::uint8_t health = 255;
        std::uint8_t size = 0;      ///< archetype size index
        std::uint8_t flags = 0;     ///< bit 0: hide where the local player has no vision
    };
    static_assert(sizeof(UnitInstance) == 8, "the instance stream must stay eight bytes wide");

    /** @brief What one frame of unit rendering cost and how much of it reached the GPU. */
    struct UnitRenderStats
    {
        double buildMs = 0.0;
        double uploadMs = 0.0;
        double drawMs = 0.0;
        std::int32_t instances = 0;
        std::int32_t drawCalls = 0;
    };

    /**
     * @brief Draws every unit in one instanced draw call.
     *
     * The whole point of the project on the GPU side. A quad of four vertices and six indices is
     * bound once; the per-instance stream carries a million entries; and `DrawInstancedPrimitives`
     * turns that into two million triangles in a single call. There is no per-unit CPU work in the
     * draw at all, and the only per-frame cost that scales with the army is filling and uploading
     * the stream -- which is why the stream is eight bytes wide and filled in parallel.
     */
    class UnitRenderer
    {
    public:
        UnitRenderer();
        ~UnitRenderer();

        UnitRenderer(const UnitRenderer&) = delete;
        UnitRenderer& operator=(const UnitRenderer&) = delete;

        /**
         * @brief Creates the quad, the instance stream and the shader.
         * @return false when the renderer cannot instance or the shader did not compile; the
         *         reason is in `Diagnostic()` and the caller should fall back or report it.
         */
        bool Load(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                  std::int32_t maxInstances);
        void Unload();

        [[nodiscard]] bool Ready() const { return ready_; }
        [[nodiscard]] const std::string& Diagnostic() const { return diagnostic_; }

        /**
         * @brief Fills the instance stream from the simulation, in parallel.
         *
         * @param viewerTeam  Whose fog of war decides what is drawn; negative reveals everything.
         * @param selectedGroup Group to highlight, or -1.
         */
        void Build(JobSystem& jobs, const Simulation& simulation, int viewerTeam,
                   int selectedGroup);

        /** @brief Uploads and issues the draw. `Build` must have run this frame. */
        void Draw(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                  const Camera& camera, const Simulation& simulation,
                  const Microsoft::Xna::Framework::Graphics::Texture2D* fog,
                  bool fogEnabled);

        [[nodiscard]] const UnitRenderStats& Stats() const { return stats_; }

    private:
        bool ready_ = false;
        std::string diagnostic_;
        std::int32_t capacity_ = 0;

        std::unique_ptr<Microsoft::Xna::Framework::Graphics::VertexBuffer> quad_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::IndexBuffer> indices_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::DynamicVertexBuffer> instances_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::ShaderEffect> effect_;

        std::vector<UnitInstance> staging_;
        UnitRenderStats stats_;
    };
}
