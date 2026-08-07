/// Defines a portion of a mesh that uses a specific material.
/// Ported from Sedulous.Models/ModelMeshPart.bf.

export module draconic.model:mesh_part;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::model
{

    /// A sub-range of a mesh's index buffer bound to one material.
    struct ModelMeshPart
    {
        i32 indexStart = 0;     // Starting index in the index buffer.
        i32 indexCount = 0;     // Number of indices in this part.
        i32 materialIndex = -1; // Material index (-1 for no material).

        constexpr ModelMeshPart() = default;
        constexpr ModelMeshPart(i32 start, i32 count, i32 matIdx = -1)
            : indexStart(start), indexCount(count), materialIndex(matIdx)
        {
        }
    };

} // namespace draconic::model
