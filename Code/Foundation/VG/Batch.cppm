// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :batch partition.
//
// The batched output an external renderer consumes: VGCommand (a run of indices
// sharing state), VGBatch (vertices, indices, commands and textures), and the
// ClipPathManager (stencil-based clip stack that emits into a batch).

module;
#include "Core/Prelude.h"

export module foundation.vg:batch;

import foundation.core;
import foundation.image;
import :enums;
import :vertex;
import :path;
import :tessellation;

using namespace foundation::core;

export namespace foundation::vg
{
    namespace image = foundation::image;

    /// A single draw command: a run of geometry sharing render state.
    struct VGCommand
    {
        i32 startIndex = 0;    ///< Starting index in the index buffer.
        i32 indexCount = 0;    ///< Number of indices to draw.
        i32 textureIndex = -1; ///< Index into the texture list (-1 for none).
        Rectangle clipRect;    ///< Clip rectangle in screen coordinates.
        VGClipMode clipMode = VGClipMode::None;
        /// Stencil-clipped and confined to clipRect (a rectangle clip inside a path clip); empty
        /// clipRect hides the draw. Scissor-mode commands are always confined to clipRect.
        bool scissored = false;
        VGBlendMode blendMode = VGBlendMode::Normal;
        i32 stencilRef = 0; ///< Stencil reference value (for stencil clipping).
        VGDrawMode drawMode = VGDrawMode::Default; ///< Straight sampling or MSDF decode.
        VGFillPhase fillPhase = VGFillPhase::Direct; ///< Stencil-then-cover phase (see enum).
        FillRule fillRule = FillRule::NonZero; ///< Winding rule for StencilWrite/Cover phases.
        VGGradientSpread gradientSpread =
            VGGradientSpread::Pad; ///< LUT sampler address mode for this draw.
        /// The layer whose offscreen target this command draws into (VGBatch::layers), -1 for the
        /// target itself.
        i32 layer = -1;
        /// A Layer-mode composite: the layer whose target the quad samples.
        i32 compositeLayer = -1;
        /// A Layer-mode composite that samples the layer's blurred copy (VGLayer::backdropBlur): a
        /// backdrop blur's shape (VGContext::DrawBackdropBlur).
        bool backdrop = false;
    };

    /// One offscreen layer (VGContext::PushLayer): drawn into a target of its own and composited
    /// into its parent once, so a group fades as one.
    struct VGLayer
    {
        i32 parent = -1;     ///< The enclosing layer, -1 for the target itself.
        Rectangle bounds{};  ///< Its content's device bounds, on whole pixels (empty: nothing drawn).
        f32 opacity = 1.0f;  ///< Applied once, at the composite.
        VGBlendMode blendMode = VGBlendMode::Normal; ///< How the composite blends into the parent.
        f32 blur = 0.0f; ///< A Gaussian blur's sigma in device pixels, 0 for none (bounds carry 3 sigma).
        /// A blurred copy to draw for backdrop composites, its sigma (the layer itself stays sharp).
        f32 backdropBlur = 0.0f;
        /// Colour filters applied at the composite: 0 is grey and 1 unchanged (CSS's saturate(),
        /// grayscale() its complement), and a factor on the colour (brightness()).
        f32 saturation = 1.0f;
        f32 brightness = 1.0f;
    };

    /// Batched vector-graphics geometry and draw commands. The output of
    /// VGContext that an external renderer consumes. Fields are public for the
    /// tessellation and clip code that writes into them.
    class VGBatch
    {
    public:
        Array<VGVertex> vertices;  ///< Vertex data for all geometry.
        Array<u32> indices;        ///< Index data for all geometry.
        Array<VGCommand> commands; ///< Draw commands (batched by state).
        /// Analytic boxes' vertices, six to a box, for Box-mode commands (no indices).
        Array<VGBoxVertex> boxVertices;
        /// Offscreen layers, in the order they were pushed (a child after its parent).
        Array<VGLayer> layers;
        // Textures referenced by commands (by textureIndex). Not owned by the
        // batch; VGContext manages lifetime. By convention index 0 is a 1x1
        // white texture for solid-color draws.
        Array<const image::ImageData*> textures;
        // Sources whose renderer-side GPU cache entries must be dropped before this
        // batch's textures are considered: the invalidation half of the renderer's
        // identity-keyed texture cache (e.g. evicted gradient LUTs). Identity keys
        // only: the renderer compares pointers, never dereferences them.
        Array<const image::ImageData*> evictedTextures;
        // Distance-field parameters for DistanceField-mode commands: the MSDF spread (texels) and
        // the atlas dimensions the DF fragment shader needs for its screen-space AA.
        f32 distanceFieldPixelRange = 4.0f;
        f32 distanceFieldAtlasWidth = 512.0f;
        f32 distanceFieldAtlasHeight = 512.0f;

        /// Vertex data as a span for GPU upload.
        [[nodiscard]] Span<VGVertex> GetVertexData()
        {
            return Span<VGVertex>(vertices.Data(), vertices.Size());
        }
        /// Index data as a span for GPU upload.
        [[nodiscard]] Span<u32> GetIndexData() { return Span<u32>(indices.Data(), indices.Size()); }

        [[nodiscard]] usize CommandCount() const { return commands.Size(); }
        [[nodiscard]] VGCommand GetCommand(usize index) const { return commands[index]; }

        /// The texture for a command (null if the command has no valid texture).
        [[nodiscard]] const image::ImageData* GetTextureForCommand(usize index) const
        {
            const VGCommand cmd = commands[index];
            if (cmd.textureIndex >= 0 && static_cast<usize>(cmd.textureIndex) < textures.Size())
                return textures[static_cast<usize>(cmd.textureIndex)];
            return nullptr;
        }

        [[nodiscard]] usize VertexCount() const { return vertices.Size(); }
        [[nodiscard]] usize IndexCount() const { return indices.Size(); }

        /// Clear all data for reuse. Also clears the texture list; the caller
        /// re-adds required textures (e.g. the white fallback at index 0).
        void Clear()
        {
            vertices.Clear();
            indices.Clear();
            commands.Clear();
            boxVertices.Clear();
            layers.Clear();
            textures.Clear();
            evictedTextures.Clear();
        }

        /// Reserve capacity for expected geometry.
        void Reserve(usize vertexCount, usize indexCount, usize commandCount)
        {
            if (vertexCount > vertices.Capacity())
                vertices.Reserve(vertexCount);
            if (indexCount > indices.Capacity())
                indices.Reserve(indexCount);
            if (commandCount > commands.Capacity())
                commands.Reserve(commandCount);
        }

        [[nodiscard]] bool IsEmpty() const
        {
            return commands.IsEmpty() || ((vertices.IsEmpty() || indices.IsEmpty()) && boxVertices.IsEmpty());
        }
    };

    /// Manages a stencil-based clip path stack, emitting stencil-write geometry.
    class ClipPathManager
    {
    public:
        /// Current stencil reference value.
        [[nodiscard]] i32 CurrentStencilRef() const { return m_currentStencilRef; }

        /// Push a clip path: emits stencil-write geometry into the batch and
        /// increments the stencil reference value.
        void PushClipPath(const Path& path, FillRule fillRule, VGBatch& batch,
                          f32 tolerance = 0.25f)
        {
            m_stencilRefStack.PushBack(m_currentStencilRef);
            ++m_currentStencilRef;

            // Tessellate the clip path; the renderer draws this to write the stencil buffer.
            const i32 startIndex = static_cast<i32>(batch.indices.Size());
            FillTessellator::Tessellate(path, fillRule, Color::White, false, batch.vertices,
                                        batch.indices, tolerance);
            const i32 indexCount = static_cast<i32>(batch.indices.Size()) - startIndex;

            if (indexCount > 0)
            {
                VGCommand cmd;
                cmd.startIndex = startIndex;
                cmd.indexCount = indexCount;
                cmd.clipMode = VGClipMode::Stencil;
                cmd.stencilRef = m_currentStencilRef;
                batch.commands.PushBack(cmd);
            }
        }

        /// Pop the current clip path, decrementing the stencil reference.
        void PopClip()
        {
            if (!m_stencilRefStack.IsEmpty())
            {
                m_currentStencilRef = m_stencilRefStack.Back();
                m_stencilRefStack.PopBack();
            }
            else
            {
                m_currentStencilRef = 0;
            }
        }

        /// Reset the clip path stack.
        void Clear()
        {
            m_stencilRefStack.Clear();
            m_currentStencilRef = 0;
        }

        /// Current clip stack depth.
        [[nodiscard]] usize Depth() const { return m_stencilRefStack.Size(); }

    private:
        Array<i32> m_stencilRefStack;
        i32 m_currentStencilRef = 0;
    };
}
