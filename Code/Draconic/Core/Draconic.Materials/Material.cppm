/// Draconic::Materials - the `:material` partition.
///
/// Material: the shared, immutable template - a shader name + variant flags, a list
/// of declared properties, a PipelineConfig, and default values. It is *data*: the
/// MaterialSystem reads the property list to infer the GPU bind-group layout, so a
/// custom material/shader needs no renderer changes. Per-use overrides live in a
/// MaterialInstance. Material is an Object so it can be a resource-system product.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.materials:material;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import :types;
import :pipeline;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::materials
{

    // Shared material template. Properties are declared once; defaults seed every
    // instance. Name strings are owned in a stable backing so property views stay valid.
    class Material final : public Object
    {
        DRACONIC_OBJECT(Material, Object)
    public:
        // Unique per-OBJECT id: renderer caches key by THIS, never by pointer (a reloaded
        // material can reallocate at the freed address - the bind-group versioning rule).
        const u64 uid = NextUid();

        // Sampler address modes for the material's texture slots (the importer wires them from
        // the source asset's sampler; glTF's default is Repeat). The MaterialSystem resolves the
        // actual rhi::Sampler from these when building the instance's bind group.
        rhi::AddressMode samplerU = rhi::AddressMode::Repeat;
        rhi::AddressMode samplerV = rhi::AddressMode::Repeat;

        String name;
        String shaderName;
        shaders::ShaderFlags shaderFlags = shaders::ShaderFlags::None;
        PipelineConfig pipeline;

        [[nodiscard]] bool IsValid() const noexcept { return shaderName.Size() > 0; }
        [[nodiscard]] usize PropertyCount() const noexcept { return m_properties.Size(); }
        [[nodiscard]] u32 UniformDataSize() const noexcept { return m_uniformDataSize; }

        [[nodiscard]] const MaterialPropertyDef& GetProperty(usize index) const noexcept
        {
            return m_properties[index];
        }

        // Index of the named property, or -1.
        [[nodiscard]] isize GetPropertyIndex(StringView n) const noexcept
        {
            for (usize i = 0; i < m_properties.Size(); ++i)
            {
                if (m_properties[i].name == n)
                {
                    return static_cast<isize>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] const MaterialPropertyDef* FindProperty(StringView n) const noexcept
        {
            const isize i = GetPropertyIndex(n);
            return (i >= 0) ? &m_properties[static_cast<usize>(i)] : nullptr;
        }

        [[nodiscard]] Span<const MaterialPropertyDef> Properties() const noexcept
        {
            return {m_properties.Data(), m_properties.Size()};
        }

        // Declares a property. The name is cloned into stable backing so the caller's
        // string need not outlive the material; uniform size grows to fit uniforms.
        void AddProperty(const MaterialPropertyDef& prop)
        {
            UniquePtr<String> owned = MakeUnique<String>(DefaultAllocator(), prop.name);
            MaterialPropertyDef d = prop;
            d.name = owned->AsView();
            m_propertyNames.PushBack(Move(owned));
            m_properties.PushBack(d);

            if (d.IsUniform())
            {
                const u32 end = d.offset + d.size;
                if (end > m_uniformDataSize)
                {
                    // Round to cbuffer alignment: the shader-side struct is 16-byte
                    // padded on every API, and WebGPU VALIDATES the bound range
                    // against the struct size (a 60-byte range under a 64-byte
                    // cbuffer fails). Buffer and binding both use this size.
                    m_uniformDataSize = (end + 15u) & ~15u;
                }
            }
        }

        // (Re)allocates the default uniform buffer, preserving existing defaults.
        void AllocateDefaultUniformData()
        {
            if (m_uniformDataSize == 0)
            {
                return;
            }
            if (m_defaultUniformData.Size() < m_uniformDataSize)
            {
                Array<u8> grown;
                grown.Resize(m_uniformDataSize);
                for (usize i = 0; i < m_defaultUniformData.Size(); ++i)
                {
                    grown[i] = m_defaultUniformData[i];
                }
                m_defaultUniformData = Move(grown);
            }
        }

        void SetDefaultFloat(StringView n, f32 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat2(StringView n, Float2 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat3(StringView n, Float3 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat4(StringView n, Float4 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultColor(StringView n, Float4 c) { SetDefaultFloat4(n, c); }

        void SetDefaultTexture(StringView n, rhi::TextureView* tex)
        {
            const isize i = GetPropertyIndex(n);
            if (i >= 0 && m_properties[static_cast<usize>(i)].IsTexture())
            {
                m_defaultTextures.InsertOrAssign(static_cast<usize>(i), tex);
            }
        }
        void SetDefaultSampler(StringView n, rhi::Sampler* s)
        {
            const isize i = GetPropertyIndex(n);
            if (i >= 0 && m_properties[static_cast<usize>(i)].IsSampler())
            {
                m_defaultSamplers.InsertOrAssign(static_cast<usize>(i), s);
            }
        }

        [[nodiscard]] Span<const u8> DefaultUniformData() const noexcept
        {
            return {m_defaultUniformData.Data(), m_defaultUniformData.Size()};
        }

        // Overlays a raw default-uniform blob (the cooked/serialized defaults) over the
        // uniform buffer. Used by the resource factory to restore authored defaults.
        void SetRawDefaultUniformData(Span<const u8> data)
        {
            AllocateDefaultUniformData();
            const usize n = data.Size() < m_defaultUniformData.Size() ? data.Size()
                                                                      : m_defaultUniformData.Size();
            if (n > 0)
            {
                MemCopy(m_defaultUniformData.Data(), data.Data(), n);
            }
        }
        [[nodiscard]] rhi::TextureView* GetDefaultTexture(usize propIndex) const noexcept
        {
            rhi::TextureView* const* p = m_defaultTextures.Find(propIndex);
            return (p != nullptr) ? *p : nullptr;
        }
        [[nodiscard]] rhi::Sampler* GetDefaultSampler(usize propIndex) const noexcept
        {
            rhi::Sampler* const* p = m_defaultSamplers.Find(propIndex);
            return (p != nullptr) ? *p : nullptr;
        }

    private:
        void WriteUniform(StringView n, const void* src, usize bytes)
        {
            const isize i = GetPropertyIndex(n);
            if (i < 0)
            {
                return;
            }
            const MaterialPropertyDef& d = m_properties[static_cast<usize>(i)];
            if (!d.IsUniform() || d.offset + bytes > m_defaultUniformData.Size())
            {
                return;
            }
            MemCopy(m_defaultUniformData.Data() + d.offset, src, bytes);
        }

        Array<MaterialPropertyDef> m_properties;
        Array<UniquePtr<String>> m_propertyNames; // stable backing for property name views
        Array<u8> m_defaultUniformData;
        u32 m_uniformDataSize = 0;
        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        HashMap<usize, rhi::TextureView*> m_defaultTextures;
        HashMap<usize, rhi::Sampler*> m_defaultSamplers;
    };

    DRACONIC_DEFINE_OBJECT(Material, "draconic::materials")

} // namespace draconic::materials
