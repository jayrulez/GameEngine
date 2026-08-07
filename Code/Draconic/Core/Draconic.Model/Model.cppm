/// A complete 3D model with meshes, materials, skeleton, and animations.
/// Ported from Sedulous.Models/Model.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include <limits>
#include <string>
#include <vector>

export module draconic.model:model;

import draconic.foundation;
import :vertex_format;
import :mesh_part;
import :model_texture;
import :model_material;
import :model_bone;
import :model_mesh;
import :model_animation;
import :model_skin;

using namespace draconic::foundation;

export namespace draconic::model
{

    /// Result of a model load operation.
    enum class ModelLoadResult : u32
    {
        Ok,
        FileNotFound,
        ParseError,
        UnsupportedFormat,
        OutOfMemory,
        InvalidData,
    };

    /// The up axis of a coordinate system, as reported by the source file.
    enum class CoordinateAxis : u32
    {
        PositiveX,
        NegativeX,
        PositiveY,
        NegativeY,
        PositiveZ,
        NegativeZ,
    };

    /// A complete 3D model with all owned containers.
    class Model
    {
    public:
        Model() = default;

        ~Model()
        {
            for (auto* m : m_meshes)
                delete m;
            for (auto* m : m_materials)
                delete m;
            for (auto* b : m_bones)
                delete b;
            for (auto* s : m_skins)
                delete s;
            for (auto* a : m_animations)
                delete a;
            for (auto* t : m_textures)
                delete t;
        }

        // Non-copyable, movable.
        Model(const Model&) = delete;
        Model& operator=(const Model&) = delete;
        Model(Model&& other) noexcept
            : rootBoneIndex(other.rootBoneIndex), originalUpAxis(other.originalUpAxis),
              m_name(static_cast<String&&>(other.m_name)),
              m_meshes(static_cast<Array<ModelMesh*>&&>(other.m_meshes)),
              m_materials(static_cast<Array<ModelMaterial*>&&>(other.m_materials)),
              m_bones(static_cast<Array<ModelBone*>&&>(other.m_bones)),
              m_skins(static_cast<Array<ModelSkin*>&&>(other.m_skins)),
              m_animations(static_cast<Array<ModelAnimation*>&&>(other.m_animations)),
              m_textures(static_cast<Array<ModelTexture*>&&>(other.m_textures)),
              m_samplers(static_cast<Array<TextureSampler>&&>(other.m_samplers)),
              m_bounds(other.m_bounds)
        {
            other.m_meshes.Clear();
            other.m_materials.Clear();
            other.m_bones.Clear();
            other.m_skins.Clear();
            other.m_animations.Clear();
            other.m_textures.Clear();
        }
        Model& operator=(Model&& other) noexcept
        {
            if (this != &other)
            {
                for (auto* m : m_meshes)
                    delete m;
                for (auto* m : m_materials)
                    delete m;
                for (auto* b : m_bones)
                    delete b;
                for (auto* s : m_skins)
                    delete s;
                for (auto* a : m_animations)
                    delete a;
                for (auto* t : m_textures)
                    delete t;

                m_name = static_cast<String&&>(other.m_name);
                m_meshes = static_cast<Array<ModelMesh*>&&>(other.m_meshes);
                m_materials = static_cast<Array<ModelMaterial*>&&>(other.m_materials);
                m_bones = static_cast<Array<ModelBone*>&&>(other.m_bones);
                m_skins = static_cast<Array<ModelSkin*>&&>(other.m_skins);
                m_animations = static_cast<Array<ModelAnimation*>&&>(other.m_animations);
                m_textures = static_cast<Array<ModelTexture*>&&>(other.m_textures);
                m_samplers = static_cast<Array<TextureSampler>&&>(other.m_samplers);
                m_bounds = other.m_bounds;
                rootBoneIndex = other.rootBoneIndex;
                originalUpAxis = other.originalUpAxis;

                other.m_meshes.Clear();
                other.m_materials.Clear();
                other.m_bones.Clear();
                other.m_skins.Clear();
                other.m_animations.Clear();
                other.m_textures.Clear();
            }
            return *this;
        }

        // -- Name --

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        // -- Meshes --

        [[nodiscard]] Span<ModelMesh* const> meshes() const
        {
            return Span<ModelMesh* const>(m_meshes.Data(), m_meshes.Size());
        }
        [[nodiscard]] Span<ModelMesh*> meshes()
        {
            return Span<ModelMesh*>(m_meshes.Data(), m_meshes.Size());
        }

        /// Add a mesh (takes ownership). Returns index.
        i32 addMesh(ModelMesh* mesh)
        {
            i32 idx = static_cast<i32>(m_meshes.Size());
            m_meshes.PushBack(mesh);
            return idx;
        }

        // -- Materials --

        [[nodiscard]] Span<ModelMaterial* const> materials() const
        {
            return Span<ModelMaterial* const>(m_materials.Data(), m_materials.Size());
        }
        [[nodiscard]] Span<ModelMaterial*> materials()
        {
            return Span<ModelMaterial*>(m_materials.Data(), m_materials.Size());
        }

        /// Add a material (takes ownership). Returns index.
        i32 addMaterial(ModelMaterial* mat)
        {
            i32 idx = static_cast<i32>(m_materials.Size());
            m_materials.PushBack(mat);
            return idx;
        }

        // -- Bones --

        [[nodiscard]] Span<ModelBone* const> bones() const
        {
            return Span<ModelBone* const>(m_bones.Data(), m_bones.Size());
        }
        [[nodiscard]] Span<ModelBone*> bones()
        {
            return Span<ModelBone*>(m_bones.Data(), m_bones.Size());
        }

        /// Add a bone (takes ownership). Sets bone.index. Returns index.
        i32 addBone(ModelBone* bone)
        {
            i32 idx = static_cast<i32>(m_bones.Size());
            bone->index = idx;
            m_bones.PushBack(bone);
            return idx;
        }

        // -- Skins --

        [[nodiscard]] Span<ModelSkin* const> skins() const
        {
            return Span<ModelSkin* const>(m_skins.Data(), m_skins.Size());
        }
        [[nodiscard]] Span<ModelSkin*> skins()
        {
            return Span<ModelSkin*>(m_skins.Data(), m_skins.Size());
        }

        /// Add a skin (takes ownership). Returns index.
        i32 addSkin(ModelSkin* skin)
        {
            i32 idx = static_cast<i32>(m_skins.Size());
            m_skins.PushBack(skin);
            return idx;
        }

        // -- Animations --

        [[nodiscard]] Span<ModelAnimation* const> animations() const
        {
            return Span<ModelAnimation* const>(m_animations.Data(), m_animations.Size());
        }
        [[nodiscard]] Span<ModelAnimation*> animations()
        {
            return Span<ModelAnimation*>(m_animations.Data(), m_animations.Size());
        }

        /// Add an animation (takes ownership). Returns index.
        i32 addAnimation(ModelAnimation* anim)
        {
            i32 idx = static_cast<i32>(m_animations.Size());
            m_animations.PushBack(anim);
            return idx;
        }

        // -- Textures --

        [[nodiscard]] Span<ModelTexture* const> textures() const
        {
            return Span<ModelTexture* const>(m_textures.Data(), m_textures.Size());
        }
        [[nodiscard]] Span<ModelTexture*> textures()
        {
            return Span<ModelTexture*>(m_textures.Data(), m_textures.Size());
        }

        /// Add a texture (takes ownership). Returns index.
        i32 addTexture(ModelTexture* tex)
        {
            i32 idx = static_cast<i32>(m_textures.Size());
            m_textures.PushBack(tex);
            return idx;
        }

        // -- Samplers --

        [[nodiscard]] Span<const TextureSampler> samplers() const
        {
            return Span<const TextureSampler>(m_samplers.Data(), m_samplers.Size());
        }
        [[nodiscard]] Span<TextureSampler> samplers()
        {
            return Span<TextureSampler>(m_samplers.Data(), m_samplers.Size());
        }

        /// Add a sampler. Returns index.
        i32 addSampler(TextureSampler sampler)
        {
            i32 idx = static_cast<i32>(m_samplers.Size());
            m_samplers.PushBack(sampler);
            return idx;
        }

        // -- Bounds --

        [[nodiscard]] AABB bounds() const { return m_bounds; }

        /// Calculate bounds from all meshes.
        void calculateBounds()
        {
            if (m_meshes.IsEmpty())
            {
                m_bounds = AABB{};
                return;
            }

            Float3 bmin(std::numeric_limits<f32>::max());
            Float3 bmax(std::numeric_limits<f32>::lowest());

            for (auto* mesh : m_meshes)
            {
                AABB mb = mesh->bounds();
                bmin = Min(bmin, mb.min);
                bmax = Max(bmax, mb.max);
            }

            m_bounds = AABB{bmin, bmax};
        }

        // -- Hierarchy --

        /// Build bone hierarchy from parent indices.
        void buildBoneHierarchy()
        {
            // Clear existing children.
            for (auto* bone : m_bones)
                bone->clearChildren();

            // Build hierarchy.
            for (usize i = 0; i < m_bones.Size(); ++i)
            {
                auto* bone = m_bones[i];
                if (bone->parentIndex >= 0 &&
                    static_cast<usize>(bone->parentIndex) < m_bones.Size())
                {
                    m_bones[bone->parentIndex]->addChild(bone);
                }
                else if (bone->parentIndex < 0)
                {
                    rootBoneIndex = bone->index;
                }
            }
        }

        // -- Lookup by name --

        /// Get mesh by name (nullptr if not found).
        [[nodiscard]] ModelMesh* getMesh(StringView n) const
        {
            for (auto* m : m_meshes)
                if (m->name() == n)
                    return m;
            return nullptr;
        }

        /// Get material by name (nullptr if not found).
        [[nodiscard]] ModelMaterial* getMaterial(StringView n) const
        {
            for (auto* m : m_materials)
                if (m->name() == n)
                    return m;
            return nullptr;
        }

        /// Get bone by name (nullptr if not found).
        [[nodiscard]] ModelBone* getBone(StringView n) const
        {
            for (auto* b : m_bones)
                if (b->name() == n)
                    return b;
            return nullptr;
        }

        /// Get animation by name (nullptr if not found).
        [[nodiscard]] ModelAnimation* getAnimation(StringView n) const
        {
            for (auto* a : m_animations)
                if (a->name() == n)
                    return a;
            return nullptr;
        }

        // -- Public fields --

        /// Index of the root bone (-1 if no hierarchy).
        i32 rootBoneIndex = -1;

        /// The original up axis of the source file's coordinate system.
        CoordinateAxis originalUpAxis = CoordinateAxis::PositiveY;

    private:
        String m_name;
        Array<ModelMesh*> m_meshes;
        Array<ModelMaterial*> m_materials;
        Array<ModelBone*> m_bones;
        Array<ModelSkin*> m_skins;
        Array<ModelAnimation*> m_animations;
        Array<ModelTexture*> m_textures;
        Array<TextureSampler> m_samplers;
        AABB m_bounds;
    };

} // namespace draconic::model
