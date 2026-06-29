// Raptor::VG — :batch partition.
//
// The batched output an external renderer consumes: VGCommand (a run of indices
// sharing state), VGBatch (vertices/indices/commands/textures), and the
// ClipPathManager (stencil-based clip stack that emits into a batch). Ported
// from Sedulous.VG (VGCommand/VGBatch/ClipPathManager).

module;
#include "Core/Prelude.h"

export module raptor.vg:batch;

import raptor.core;
import raptor.image;
import :enums;
import :vertex;
import :path;
import :tessellation;

using namespace raptor::core;

export namespace raptor::vg
{
    namespace img = raptor::image;

    /// A single draw command: a run of geometry sharing render state.
    struct VGCommand
    {
        i32 startIndex = 0;        ///< Starting index in the index buffer.
        i32 indexCount = 0;        ///< Number of indices to draw.
        i32 textureIndex = -1;     ///< Index into the texture list (-1 for none).
        Rect clipRect;             ///< Clip rectangle in screen coordinates.
        VGClipMode clipMode = VGClipMode::None;
        VGBlendMode blendMode = VGBlendMode::Normal;
        i32 stencilRef = 0;        ///< Stencil reference value (for stencil clipping).
        VGDrawMode drawMode = VGDrawMode::Default; ///< Pipeline selection.
    };

    /// Batched vector-graphics geometry and draw commands. The output of
    /// VGContext that an external renderer consumes. Fields are public for the
    /// tessellation/clip code that writes into them (matching Sedulous).
    class VGBatch
    {
    public:
        Array<VGVertex> vertices;  ///< Vertex data for all geometry.
        Array<u32> indices;        ///< Index data for all geometry.
        Array<VGCommand> commands; ///< Draw commands (batched by state).
        // Textures referenced by commands (by textureIndex). NOT owned by the
        // batch — VGContext manages lifetime. By convention index 0 is a 1x1
        // white texture for solid-color draws.
        Array<const img::ImageData*> textures;

        // Distance-field rendering metadata (set by VGContext when DF text is drawn).
        f32 dfPxRange  = 4.0f;  ///< Signed-distance pixel range.
        f32 dfAtlasW   = 512.0f;
        f32 dfAtlasH   = 512.0f;

        /// Vertex data as a span for GPU upload.
        [[nodiscard]] Span<VGVertex> GetVertexData() { return Span<VGVertex>(vertices.Data(), vertices.Size()); }
        /// Index data as a span for GPU upload.
        [[nodiscard]] Span<u32> GetIndexData() { return Span<u32>(indices.Data(), indices.Size()); }

        [[nodiscard]] usize CommandCount() const { return commands.Size(); }
        [[nodiscard]] VGCommand GetCommand(usize index) const { return commands[index]; }

        /// The texture for a command (null if the command has no valid texture).
        [[nodiscard]] const img::ImageData* GetTextureForCommand(usize index) const
        {
            const VGCommand cmd = commands[index];
            if (cmd.textureIndex >= 0 && static_cast<usize>(cmd.textureIndex) < textures.Size())
                return textures[static_cast<usize>(cmd.textureIndex)];
            return nullptr;
        }

        [[nodiscard]] usize VertexCount() const { return vertices.Size(); }
        [[nodiscard]] usize IndexCount() const { return indices.Size(); }

        /// Clear all data for reuse. Also clears the texture list — the caller
        /// re-adds required textures (e.g. the white fallback at index 0).
        void Clear()
        {
            vertices.Clear();
            indices.Clear();
            commands.Clear();
            textures.Clear();
        }

        /// Reserve capacity for expected geometry.
        void Reserve(usize vertexCount, usize indexCount, usize commandCount)
        {
            if (vertexCount > vertices.Capacity()) vertices.Reserve(vertexCount);
            if (indexCount > indices.Capacity()) indices.Reserve(indexCount);
            if (commandCount > commands.Capacity()) commands.Reserve(commandCount);
        }

        [[nodiscard]] bool IsEmpty() const { return vertices.IsEmpty() || indices.IsEmpty() || commands.IsEmpty(); }
    };

    /// Manages a stencil-based clip path stack, emitting stencil-write geometry.
    class ClipPathManager
    {
    public:
        /// Current stencil reference value.
        [[nodiscard]] i32 CurrentStencilRef() const { return m_currentStencilRef; }

        /// Push a clip path: emits stencil-write geometry into the batch and
        /// increments the stencil reference value.
        void PushClipPath(const Path& path, FillRule fillRule, VGBatch& batch, f32 tolerance = 0.25f)
        {
            m_stencilRefStack.PushBack(m_currentStencilRef);
            ++m_currentStencilRef;

            // Tessellate the clip path; the renderer draws this to write the stencil buffer.
            const i32 startIndex = static_cast<i32>(batch.indices.Size());
            FillTessellator::Tessellate(path, fillRule, Color::White, false, batch.vertices, batch.indices, tolerance);
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
