/// Standard and skinned vertex structs for model data.
/// Ported from Sedulous.Models/ModelVertex.bf.

export module draconic.model:model_vertex;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::model
{

    /// Standard vertex format for static models (52 bytes). tangent.w = TBN handedness (+-1).
    struct ModelVertex
    {
        Float3 position{};          // 12 bytes
        Float3 normal{0, 1, 0};     // 12 bytes
        Float2 texCoord{};          // 8 bytes
        u32 color = 0xFFFFFFFF;     // 4 bytes (packed RGBA)
        Float4 tangent{1, 0, 0, 1}; // 16 bytes (xyz tangent, w handedness)
        // Total: 52 bytes

        constexpr ModelVertex() = default;
        constexpr ModelVertex(Float3 pos, Float3 nrm, Float2 uv, u32 col = 0xFFFFFFFF,
                              Float4 tan = {1, 0, 0, 1})
            : position(pos), normal(nrm), texCoord(uv), color(col), tangent(tan)
        {
        }
    };

    static_assert(sizeof(ModelVertex) == 52, "ModelVertex must be 52 bytes");

    /// Skinned vertex format for animated models (76 bytes). tangent.w = TBN handedness (+-1).
    struct SkinnedModelVertex
    {
        Float3 position{};            // 12 bytes
        Float3 normal{0, 1, 0};       // 12 bytes
        Float2 texCoord{};            // 8 bytes
        u32 color = 0xFFFFFFFF;       // 4 bytes (packed RGBA)
        Float4 tangent{1, 0, 0, 1};   // 16 bytes (xyz tangent, w handedness)
        u16 joints[4] = {0, 0, 0, 0}; // 8 bytes (up to 4 bone indices)
        Float4 weights{1, 0, 0, 0};   // 16 bytes (bone weights)
        // Total: 76 bytes

        constexpr SkinnedModelVertex() = default;
        constexpr SkinnedModelVertex(Float3 pos, Float3 nrm, Float2 uv, u32 col, Float4 tan, u16 j0,
                                     u16 j1, u16 j2, u16 j3, Float4 wt)
            : position(pos), normal(nrm), texCoord(uv), color(col), tangent(tan),
              joints{j0, j1, j2, j3}, weights(wt)
        {
        }
    };

    static_assert(sizeof(SkinnedModelVertex) == 76, "SkinnedModelVertex must be 76 bytes");

} // namespace draconic::model
