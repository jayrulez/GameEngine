/// Draconic::Materials - the `:instance` partition.
///
/// MaterialInstance: a per-use copy of a Material's properties with overridable
/// values and dirty tracking. Setters write into an override uniform buffer / texture
/// maps and flip dirty flags; on a false->true transition the instance NOTIFIES its
/// sink (the MaterialSystem) so re-prep is O(dirty), not O(all instances). The sink
/// is an interface declared here so :system can depend on :instance (not vice-versa),
/// breaking the partition cycle.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.materials:instance;

import draconic.foundation;
import draconic.rhi;
import :types;
import :material;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::materials
{

    class MaterialInstance;

    // What a MaterialInstance calls back into (implemented by MaterialSystem). Keeps the
    // instance->system edge an interface so the partitions don't import each other.
    class IMaterialInstanceSink
    {
    public:
        virtual ~IMaterialInstanceSink() = default;
        virtual void MarkInstanceDirty(MaterialInstance* instance) = 0;
        virtual void ReleaseInstance(MaterialInstance* instance) = 0;
    };

    // Up-to-128-property override bitset.
    struct PropertyOverrideMask
    {
        u64 lo = 0, hi = 0;
        void Set(usize i) noexcept
        {
            if (i < 64)
                lo |= (1ull << i);
            else if (i < 128)
                hi |= (1ull << (i - 64));
        }
        void Clear(usize i) noexcept
        {
            if (i < 64)
                lo &= ~(1ull << i);
            else if (i < 128)
                hi &= ~(1ull << (i - 64));
        }
        [[nodiscard]] bool IsSet(usize i) const noexcept
        {
            if (i < 64)
                return (lo & (1ull << i)) != 0;
            if (i < 128)
                return (hi & (1ull << (i - 64))) != 0;
            return false;
        }
        void Reset() noexcept
        {
            lo = 0;
            hi = 0;
        }
        [[nodiscard]] bool HasAny() const noexcept { return lo != 0 || hi != 0; }
    };

    // Per-use material with overridable properties + dirty tracking.
    class MaterialInstance
    {
    public:
        explicit MaterialInstance(Material* material) : m_material(RefPtr<Material>(material))
        {
            if (material != nullptr && material->UniformDataSize() > 0)
            {
                m_uniformData.Resize(material->UniformDataSize());
                const Span<const u8> defaults = material->DefaultUniformData();
                for (usize i = 0; i < defaults.Size() && i < m_uniformData.Size(); ++i)
                {
                    m_uniformData[i] = defaults[i];
                }
            }
        }

        ~MaterialInstance()
        {
            if (m_sink != nullptr)
            {
                m_sink->ReleaseInstance(this);
            }
        }

        MaterialInstance(const MaterialInstance&) = delete;
        MaterialInstance& operator=(const MaterialInstance&) = delete;

        [[nodiscard]] Material* GetMaterial() const noexcept { return m_material.Get(); }

        // --- property setters (write override + mark dirty) ---
        void SetFloat(StringView n, f32 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetFloat2(StringView n, Float2 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetFloat3(StringView n, Float3 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetFloat4(StringView n, Float4 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetColor(StringView n, Float4 c) { SetFloat4(n, c); }

        void SetTexture(StringView n, rhi::TextureView* tex)
        {
            const isize i = m_material->GetPropertyIndex(n);
            if (i < 0 || !m_material->GetProperty(static_cast<usize>(i)).IsTexture())
            {
                return;
            }
            m_textures.InsertOrAssign(static_cast<usize>(i), tex);
            m_overrides.Set(static_cast<usize>(i));
            SetBindGroupDirty();
        }
        void SetSampler(StringView n, rhi::Sampler* s)
        {
            const isize i = m_material->GetPropertyIndex(n);
            if (i < 0 || !m_material->GetProperty(static_cast<usize>(i)).IsSampler())
            {
                return;
            }
            m_samplers.InsertOrAssign(static_cast<usize>(i), s);
            m_overrides.Set(static_cast<usize>(i));
            SetBindGroupDirty();
        }

        // --- effective values (override else material default) ---
        [[nodiscard]] rhi::TextureView* GetTexture(usize propIndex) const
        {
            if (m_overrides.IsSet(propIndex))
            {
                rhi::TextureView* const* p = m_textures.Find(propIndex);
                if (p != nullptr)
                {
                    return *p;
                }
            }
            return m_material->GetDefaultTexture(propIndex);
        }
        [[nodiscard]] rhi::Sampler* GetSampler(usize propIndex) const
        {
            if (m_overrides.IsSet(propIndex))
            {
                rhi::Sampler* const* p = m_samplers.Find(propIndex);
                if (p != nullptr)
                {
                    return *p;
                }
            }
            return m_material->GetDefaultSampler(propIndex);
        }
        [[nodiscard]] Span<const u8> UniformData() const noexcept
        {
            return m_uniformData.Size() > 0
                       ? Span<const u8>{m_uniformData.Data(), m_uniformData.Size()}
                       : m_material->DefaultUniformData();
        }

        void ResetProperty(StringView n)
        {
            const isize idx = m_material->GetPropertyIndex(n);
            if (idx < 0)
            {
                return;
            }
            const usize i = static_cast<usize>(idx);
            const MaterialPropertyDef& d = m_material->GetProperty(i);
            if (d.IsUniform() && m_uniformData.Size() >= d.offset + d.size)
            {
                const Span<const u8> defaults = m_material->DefaultUniformData();
                if (defaults.Size() >= d.offset + d.size)
                {
                    MemCopy(m_uniformData.Data() + d.offset, defaults.Data() + d.offset, d.size);
                }
                SetUniformDirty();
            }
            else if (d.IsTexture())
            {
                m_textures.Remove(i);
                SetBindGroupDirty();
            }
            else if (d.IsSampler())
            {
                m_samplers.Remove(i);
                SetBindGroupDirty();
            }
            m_overrides.Clear(i);
        }

        // --- dirty-state (driven by MaterialSystem) ---
        [[nodiscard]] bool IsUniformDirty() const noexcept { return m_uniformDirty; }
        [[nodiscard]] bool IsBindGroupDirty() const noexcept { return m_bindGroupDirty; }
        void ClearUniformDirty() noexcept { m_uniformDirty = false; }
        void ClearBindGroupDirty() noexcept { m_bindGroupDirty = false; }
        void MarkUniformDirty() { SetUniformDirty(); }
        void MarkBindGroupDirty() { SetBindGroupDirty(); }

        // --- wiring used only by MaterialSystem ---
        void SetSink(IMaterialInstanceSink* sink) noexcept { m_sink = sink; }
        [[nodiscard]] bool IsInDirtyList() const noexcept { return m_inDirtyList; }
        void SetInDirtyList(bool v) noexcept { m_inDirtyList = v; }
        [[nodiscard]] rhi::BindGroupLayout* BindGroupLayout() const noexcept
        {
            return m_bindGroupLayout;
        }
        void SetBindGroupLayout(rhi::BindGroupLayout* l) noexcept { m_bindGroupLayout = l; }

    private:
        void WriteUniform(StringView n, const void* src, usize bytes)
        {
            const isize idx = m_material->GetPropertyIndex(n);
            if (idx < 0)
            {
                return;
            }
            const usize i = static_cast<usize>(idx);
            const MaterialPropertyDef& d = m_material->GetProperty(i);
            if (!d.IsUniform() || m_uniformData.Size() < d.offset + bytes)
            {
                return;
            }
            MemCopy(m_uniformData.Data() + d.offset, src, bytes);
            m_overrides.Set(i);
            SetUniformDirty();
        }
        void SetUniformDirty()
        {
            if (m_uniformDirty)
            {
                return;
            }
            m_uniformDirty = true;
            if (!m_inDirtyList && m_sink != nullptr)
            {
                m_sink->MarkInstanceDirty(this);
            }
        }
        void SetBindGroupDirty()
        {
            if (m_bindGroupDirty)
            {
                return;
            }
            m_bindGroupDirty = true;
            if (!m_inDirtyList && m_sink != nullptr)
            {
                m_sink->MarkInstanceDirty(this);
            }
        }

        RefPtr<Material> m_material; // owned (keeps a reloaded-away material alive while cached)
        IMaterialInstanceSink* m_sink = nullptr;      // borrowed (the MaterialSystem)
        Array<u8> m_uniformData;                      // override uniform buffer
        HashMap<usize, rhi::TextureView*> m_textures; // override textures by property index
        HashMap<usize, rhi::Sampler*> m_samplers;     // override samplers by property index
        PropertyOverrideMask m_overrides;
        rhi::BindGroupLayout* m_bindGroupLayout = nullptr; // set by MaterialSystem for PSO creation
        bool m_uniformDirty = true;
        bool m_bindGroupDirty = true;
        bool m_inDirtyList = false;
    };

} // namespace draconic::materials
