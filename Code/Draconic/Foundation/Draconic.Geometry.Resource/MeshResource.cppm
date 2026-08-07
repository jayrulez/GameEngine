/// Draconic::MeshResource - the `draconic.geometry.resource` module.
///
/// Meshes as resources: a StaticMeshSource / SkinnedMeshSource (cooked content - the
/// raw vertex stream, indices, submeshes, and for skinned the parallel skinning
/// stream + skeleton ref) is built by a factory into the runtime StaticMesh /
/// SkinnedMesh. Two source/factory pairs, mirroring the mesh hierarchy: SkinnedMeshSource
/// IS-A StaticMeshSource, so the static serialization + fill logic is shared (the skinned
/// side only adds the skinning stream). Bounds are recomputed on build (derivable from
/// the vertices), so they aren't stored.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.geometry.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;
import draconic.geometry;

using namespace draconic::foundation;
using namespace draconic::resource;

export namespace draconic::geometry
{

    // Cooked static mesh: raw static-stream bytes + 32-bit indices + submeshes (parallel
    // arrays, so they serialize via the primitive Array<T> path).
    class StaticMeshSource : public ISerializable
    {
        DRACONIC_OBJECT(StaticMeshSource, ISerializable)
    public:
        String name;
        Array<u8> vertexBlob; // raw StaticMeshVertex bytes (48B each)
        Array<u32> indexData; // 32-bit indices
        Array<i32> subStart;
        Array<i32> subCount;
        Array<i32> subMaterial;
        Array<u8> subPrim; // PrimitiveType

        void Serialize(ISerializer& ar) override { SerializeStatic(ar); }

        // Captures a StaticMesh into this source (for cooking).
        static void FromMesh(const StaticMesh& mesh, StaticMeshSource& out)
        {
            out.name = String(mesh.name.AsView());
            out.vertexBlob.Clear();
            out.vertexBlob.Resize(mesh.VertexDataSize());
            if (mesh.VertexDataSize() > 0)
            {
                MemCopy(out.vertexBlob.Data(), mesh.VertexData(), mesh.VertexDataSize());
            }
            out.indexData.Clear();
            out.indexData.Reserve(mesh.IndexCount());
            for (u32 i = 0; i < mesh.IndexCount(); ++i)
            {
                out.indexData.PushBack(mesh.indices.Get(i));
            }
            out.subStart.Clear();
            out.subCount.Clear();
            out.subMaterial.Clear();
            out.subPrim.Clear();
            for (const SubMesh& sm : mesh.subMeshes)
            {
                out.subStart.PushBack(sm.startIndex);
                out.subCount.PushBack(sm.indexCount);
                out.subMaterial.PushBack(sm.materialIndex);
                out.subPrim.PushBack(static_cast<u8>(sm.primitiveType));
            }
        }

        // Populates a StaticMesh from this source (recomputes bounds).
        void FillStatic(StaticMesh& mesh) const
        {
            mesh.name = String(name.AsView());
            const u32 vcount = static_cast<u32>(vertexBlob.Size() / sizeof(StaticMeshVertex));
            mesh.vertices.Clear();
            mesh.vertices.Resize(vcount);
            if (vcount > 0)
            {
                MemCopy(mesh.vertices.Data(), vertexBlob.Data(), vcount * sizeof(StaticMeshVertex));
            }
            mesh.indices.Resize(static_cast<u32>(indexData.Size()));
            for (usize i = 0; i < indexData.Size(); ++i)
            {
                mesh.indices.Set(static_cast<u32>(i), indexData[i]);
            }
            mesh.subMeshes.Clear();
            for (usize i = 0; i < subStart.Size(); ++i)
            {
                mesh.subMeshes.PushBack(
                    SubMesh{subStart[i], subCount[i], (i < subMaterial.Size() ? subMaterial[i] : 0),
                            static_cast<PrimitiveType>(i < subPrim.Size() ? subPrim[i] : 0)});
            }
            mesh.CalculateBounds();
        }

    protected:
        // Shared by the skinned subclass so it can append after the static fields.
        void SerializeStatic(ISerializer& ar)
        {
            draconic::foundation::Serialize(ar, "name", name);
            draconic::foundation::Serialize(ar, "vertexBlob", vertexBlob);
            // v1 blobs predate the Float4 tangent (48-byte stride, Float3 tangent at offset 36):
            // expand each vertex in place with handedness +1 - identical look, no re-authoring.
            if (ar.Mode() == SerializeMode::Read && ar.Version() < 2)
            {
                constexpr usize kOldStride = 48;
                if (vertexBlob.Size() % kOldStride == 0 && !vertexBlob.IsEmpty())
                {
                    const usize count = vertexBlob.Size() / kOldStride;
                    Array<u8> wide;
                    wide.Resize(count * sizeof(StaticMeshVertex));
                    for (usize i = 0; i < count; ++i)
                    {
                        const u8* src = vertexBlob.Data() + i * kOldStride;
                        auto* dst = reinterpret_cast<StaticMeshVertex*>(
                            wide.Data() + i * sizeof(StaticMeshVertex));
                        *dst = StaticMeshVertex{};
                        MemCopy(dst, src, 36); // pos/normal/uv/color
                        Float3 t3{1, 0, 0};
                        MemCopy(&t3, src + 36, sizeof(t3)); // old Float3 tangent
                        dst->tangent = Float4{t3.x, t3.y, t3.z, 1.0f};
                    }
                    vertexBlob = Move(wide);
                }
            }
            draconic::foundation::Serialize(ar, "indexData", indexData);
            draconic::foundation::Serialize(ar, "subStart", subStart);
            draconic::foundation::Serialize(ar, "subCount", subCount);
            draconic::foundation::Serialize(ar, "subMaterial", subMaterial);
            draconic::foundation::Serialize(ar, "subPrim", subPrim);
        }
    };

    // Cooked skinned mesh: the static cooked data + the parallel skinning stream + skeleton.
    class SkinnedMeshSource final : public StaticMeshSource
    {
        DRACONIC_OBJECT(SkinnedMeshSource, StaticMeshSource)
    public:
        Array<u8> skinningBlob; // raw VertexSkinning bytes (24B each)
        i32 skeletonIndex = -1;

        void Serialize(ISerializer& ar) override
        {
            SerializeStatic(ar);
            draconic::foundation::Serialize(ar, "skinningBlob", skinningBlob);
            draconic::foundation::Serialize(ar, "skeletonIndex", skeletonIndex);
        }

        static void FromMesh(const SkinnedMesh& mesh, SkinnedMeshSource& out)
        {
            StaticMeshSource::FromMesh(mesh, out); // static fields
            out.skinningBlob.Clear();
            out.skinningBlob.Resize(mesh.SkinningDataSize());
            if (mesh.SkinningDataSize() > 0)
            {
                MemCopy(out.skinningBlob.Data(), mesh.SkinningData(), mesh.SkinningDataSize());
            }
            out.skeletonIndex = mesh.skeletonIndex;
        }

        void FillSkinned(SkinnedMesh& mesh) const
        {
            FillStatic(mesh); // base static stream + indices + submeshes + bounds
            const u32 scount = static_cast<u32>(skinningBlob.Size() / sizeof(VertexSkinning));
            mesh.skinning.Clear();
            mesh.skinning.Resize(scount);
            if (scount > 0)
            {
                MemCopy(mesh.skinning.Data(), skinningBlob.Data(), scount * sizeof(VertexSkinning));
            }
            mesh.skeletonIndex = skeletonIndex;
        }
    };

    // Builds a StaticMeshSource into a runtime StaticMesh.
    //
    // The runtime StaticMesh/SkinnedMesh is a PURE-CPU product (vertex/index arrays + submeshes;
    // the renderer uploads to GPU buffers separately), so the async path (task #123) does the
    // whole build - including the heavy vertex/index blob copy - on a JobSystem worker and finalize
    // is a no-op. Safe: content-DB reads open independent streams and the mesh source + product
    // types are registered on the main thread at startup (RegisterModelResource / AddFactory).
    class StaticMeshFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &StaticMesh::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            return BuildMesh(instance);
        }
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            return BuildMesh(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] static RefPtr<Object> BuildMesh(draconic::content::Instance& instance)
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            // A SkinnedMeshSource IS-A StaticMeshSource, so a Ref<StaticMesh> can legitimately bind
            // a skinned product (the picker offers both). Build the REAL SkinnedMesh then - FillStatic
            // would silently drop the skin stream and the mesh could never animate.
            if (SkinnedMeshSource* skinned = Cast<SkinnedMeshSource>(object.Get()))
            {
                RefPtr<SkinnedMesh> mesh = MakeRef<SkinnedMesh>(DefaultAllocator());
                skinned->FillSkinned(*mesh);
                return mesh;
            }
            StaticMeshSource* src = Cast<StaticMeshSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            src->FillStatic(*mesh);
            return mesh;
        }
    };

    // Builds a SkinnedMeshSource into a runtime SkinnedMesh. Pure-CPU (see StaticMeshFactory).
    class SkinnedMeshFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SkinnedMesh::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            return BuildMesh(instance);
        }
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            return BuildMesh(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] static RefPtr<Object> BuildMesh(draconic::content::Instance& instance)
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            SkinnedMeshSource* src = Cast<SkinnedMeshSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<SkinnedMesh> mesh = MakeRef<SkinnedMesh>(DefaultAllocator());
            src->FillSkinned(*mesh);
            return mesh;
        }
    };

    DRACONIC_DEFINE_OBJECT_VERSIONED(StaticMeshSource, "draconic::geometry", 2)
    DRACONIC_DEFINE_OBJECT_VERSIONED(SkinnedMeshSource, "draconic::geometry", 2)

} // namespace draconic::geometry
