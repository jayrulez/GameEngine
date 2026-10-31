// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials - the `:system` partition.
///
/// MaterialSystem: owns material instances' GPU resources and infers the bind-group layout
/// from a material's declared property list. The uniforms share one uniform buffer at binding
/// 0, each effect group gets its own uniform buffer from binding 1, and each texture or sampler
/// gets its own entry. A new material or custom shader therefore needs no renderer changes.
/// Layouts are cached by content hash; instance uniform buffers and bind groups are (re)built
/// lazily, and instances notify the system when dirty so PrepareDirtyInstances is O(dirty).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.materials:system;

import foundation.core;
import foundation.rhi;
import :types;
import :material;
import :instance;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::materials
{

    class MaterialSystem final : public IMaterialInstanceSink
    {
    public:
        MaterialSystem() = default;
        ~MaterialSystem() override { Shutdown(); }

        MaterialSystem(const MaterialSystem&) = delete;
        MaterialSystem& operator=(const MaterialSystem&) = delete;

        // Binds the system to a device and creates default GPU resources: a sampler, and white
        // and flat-normal 1x1 fallback textures used when a texture slot is unbound.
        Status Initialize(rhi::Device& device)
        {
            m_device = &device;
            m_queue = device.GetQueue(rhi::QueueType::Graphics);
            if (m_queue == nullptr)
            {
                return Status{ErrorCode::Unknown};
            }
            return CreateDefaultResources();
        }

        [[nodiscard]] rhi::Device* Device() const noexcept { return m_device; }
        [[nodiscard]] rhi::Sampler* DefaultSampler() const noexcept { return m_defaultSampler; }
        [[nodiscard]] rhi::TextureView* WhiteTexture() const noexcept { return m_whiteView; }
        [[nodiscard]] rhi::TextureView* NormalTexture() const noexcept { return m_normalView; }
        [[nodiscard]] rhi::TextureView* BlackTexture() const noexcept { return m_blackView; }

        // Ensures the instance's uniform buffer is up to date and returns it (null if the material
        // declares no uniforms). Lets a renderer assemble its own fixed set-2 layout (UBO and textures)
        // while still sourcing the packed uniform data from the material system.
        [[nodiscard]] rhi::Buffer* EnsureUniformBuffer(MaterialInstance& instance)
        {
            Material* material = instance.GetMaterial();
            if (material == nullptr || material->UniformDataSize() == 0)
            {
                return nullptr;
            }
            instance.SetSink(this);
            if (instance.IsUniformDirty())
            {
                if (!UpdateUniformBuffer(instance))
                {
                    return nullptr;
                }
                instance.ClearUniformDirty();
            }
            rhi::Buffer** buf = m_uniformBuffers.Find(&instance);
            return (buf != nullptr) ? *buf : nullptr;
        }

        // Bind-group layout inferred from the material's property definitions, cached.
        [[nodiscard]] rhi::BindGroupLayout* GetOrCreateLayout(Material& material)
        {
            const u64 hash = ComputeLayoutHash(material);
            if (rhi::BindGroupLayout** cached = m_layoutCache.Find(hash))
            {
                return *cached;
            }

            Array<rhi::BindGroupLayoutEntry> entries;

            bool hasUniforms = false;
            for (const MaterialPropertyDef& p : material.Properties())
            {
                if (p.IsUniform())
                {
                    hasUniforms = true;
                    break;
                }
            }
            if (hasUniforms && material.UniformDataSize() > 0)
            {
                // Vertex too: a surface shader's Vertex() reads its material's properties.
                entries.PushBack(rhi::BindGroupLayoutEntry::UniformBuffer(
                    0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment));
            }
            // Each effect group's buffer, right after the material's (the bind group pushes them at
            // the same positions: WebGPU matches entries by position). An effect block binds at
            // b1 of the material's set (kShaderEffectRegister). The engine declares one effect per
            // flag and its shaders declare one (Wind), so a second group would take b2, which no
            // shader declares; effects would need registers of their own first. The stage is
            // Vertex because the effects are vertex effects (the sway).
            u32 effectBinding = kFirstEffectBinding;
            for (const MaterialEffectGroup& group : material.EffectGroups())
            {
                if (group.uniformSize > 0)
                {
                    entries.PushBack(rhi::BindGroupLayoutEntry::UniformBuffer(
                        effectBinding++, rhi::ShaderStage::Vertex));
                }
            }

            u32 texBinding = 0, sampBinding = 0;
            for (const MaterialPropertyDef& p : material.Properties())
            {
                switch (p.type)
                {
                case MaterialPropertyType::Texture2D:
                    entries.PushBack(rhi::BindGroupLayoutEntry::SampledTexture(
                        texBinding++, rhi::ShaderStage::Fragment));
                    break;
                case MaterialPropertyType::TextureCube:
                    entries.PushBack(rhi::BindGroupLayoutEntry::SampledTexture(
                        texBinding++, rhi::ShaderStage::Fragment,
                        rhi::TextureViewDimension::TextureCube));
                    break;
                case MaterialPropertyType::Sampler:
                    entries.PushBack(rhi::BindGroupLayoutEntry::Sampler(
                        sampBinding++, rhi::ShaderStage::Fragment));
                    break;
                default:
                    break; // scalars live in the uniform buffer
                }
            }
            if (entries.IsEmpty())
            {
                return nullptr;
            }

            rhi::BindGroupLayoutDesc desc{};
            desc.entries = Span<const rhi::BindGroupLayoutEntry>{entries.Data(), entries.Size()};
            rhi::BindGroupLayout* layout = nullptr;
            if (!m_device->CreateBindGroupLayout(desc, layout).IsOk())
            {
                return nullptr;
            }
            m_layoutCache.InsertOrAssign(hash, layout);
            return layout;
        }

        // (Re)builds an instance's uniform buffer and bind group if dirty. Returns its
        // bind group, ready to bind for rendering.
        [[nodiscard]] rhi::BindGroup* PrepareInstance(MaterialInstance& instance,
                                                      rhi::BindGroupLayout* layout = nullptr)
        {
            Material* material = instance.GetMaterial();
            if (material == nullptr)
            {
                return nullptr;
            }
            instance.SetSink(this);

            rhi::BindGroupLayout* bgLayout = layout;
            if (bgLayout == nullptr)
            {
                bgLayout = GetOrCreateLayout(*material);
            }
            if (bgLayout == nullptr)
            {
                return nullptr;
            }

            if (instance.IsUniformDirty() && HasUniformBuffers(*material))
            {
                if (!UpdateUniformBuffer(instance))
                {
                    return nullptr;
                }
                instance.ClearUniformDirty();
            }
            instance.SetBindGroupLayout(bgLayout);

            if (instance.IsBindGroupDirty())
            {
                if (!UpdateBindGroup(instance, bgLayout))
                {
                    return nullptr;
                }
                instance.ClearBindGroupDirty();
            }
            rhi::BindGroup** bg = m_bindGroups.Find(&instance);
            return (bg != nullptr) ? *bg : nullptr;
        }

        [[nodiscard]] rhi::BindGroup* GetBindGroup(MaterialInstance& instance)
        {
            rhi::BindGroup** bg = m_bindGroups.Find(&instance);
            return (bg != nullptr) ? *bg : nullptr;
        }

        // --- IMaterialInstanceSink: dirty notification and cleanup ---
        void MarkInstanceDirty(MaterialInstance* instance) override
        {
            if (instance == nullptr || instance->IsInDirtyList())
            {
                return;
            }
            instance->SetInDirtyList(true);
            m_dirty.PushBack(instance);
        }

        /// Per-frame: frees retired bind groups once the frame ring has cycled past them.
        /// The renderer calls this once per frame alongside its own retirement ticks.
        void TickRetired()
        {
            usize w = 0;
            for (usize i = 0; i < m_retiredBindGroups.Size(); ++i)
            {
                RetiredBindGroup r = m_retiredBindGroups[i];
                if (r.framesLeft <= 1)
                {
                    m_device->DestroyBindGroup(r.bg);
                }
                else
                {
                    r.framesLeft -= 1;
                    m_retiredBindGroups[w++] = r;
                }
            }
            m_retiredBindGroups.Resize(w);
        }

        // Re-prepares every instance dirtied since the last drain (O(dirty)).
        void PrepareDirtyInstances()
        {
            for (usize i = 0; i < m_dirty.Size(); ++i)
            {
                MaterialInstance* inst = m_dirty[i];
                if (inst == nullptr)
                {
                    continue;
                }
                inst->SetInDirtyList(false);
                if (inst->IsUniformDirty() || inst->IsBindGroupDirty())
                {
                    (void)PrepareInstance(*inst);
                }
            }
            m_dirty.Clear();
        }

        // Hands the instance's uniform buffer to the caller (removed from the system) without
        // destroying it, like DetachBindGroup. ReleaseInstance frees immediately, so an instance
        // that in-flight frames may still reference must have both its bind group and its
        // uniform buffer detached for deferred retirement before it dies; detaching only the
        // bind group frees memory the GPU is still using.
        [[nodiscard]] rhi::Buffer* DetachUniformBuffer(MaterialInstance* instance)
        {
            if (instance == nullptr)
            {
                return nullptr;
            }
            if (rhi::Buffer** buf = m_uniformBuffers.Find(instance))
            {
                rhi::Buffer* b = *buf;
                m_uniformBuffers.Remove(instance);
                return b;
            }
            return nullptr;
        }

        // Hands the instance's effect buffers to the caller (removed from the system) without
        // destroying them, appended to `out`: DetachUniformBuffer for the effect groups'
        // buffers, retired the same way.
        void DetachEffectBuffers(MaterialInstance* instance, Array<rhi::Buffer*>& out)
        {
            if (instance == nullptr)
            {
                return;
            }
            if (EffectBuffers* buffers = m_effectBuffers.Find(instance))
            {
                for (rhi::Buffer* b : buffers->buffers)
                {
                    if (b != nullptr)
                    {
                        out.PushBack(b);
                    }
                }
                m_effectBuffers.Remove(instance);
            }
        }

        // Hands the instance's bind group to the caller (removed from the system) without
        // destroying it, for deferred retirement while in-flight frames may still bind it.
        [[nodiscard]] rhi::BindGroup* DetachBindGroup(MaterialInstance* instance)
        {
            if (instance == nullptr)
            {
                return nullptr;
            }
            if (rhi::BindGroup** bg = m_bindGroups.Find(instance))
            {
                rhi::BindGroup* g = *bg;
                m_bindGroups.Remove(instance);
                return g;
            }
            return nullptr;
        }

        void ReleaseInstance(MaterialInstance* instance) override
        {
            if (instance == nullptr)
            {
                return;
            }
            if (instance->IsInDirtyList())
            {
                RemoveFromDirty(instance);
                instance->SetInDirtyList(false);
            }
            if (rhi::BindGroup** bg = m_bindGroups.Find(instance))
            {
                rhi::BindGroup* g = *bg;
                m_device->DestroyBindGroup(g);
                m_bindGroups.Remove(instance);
            }
            if (rhi::Buffer** buf = m_uniformBuffers.Find(instance))
            {
                rhi::Buffer* b = *buf;
                m_device->DestroyBuffer(b);
                m_uniformBuffers.Remove(instance);
            }
            if (EffectBuffers* buffers = m_effectBuffers.Find(instance))
            {
                for (rhi::Buffer* b : buffers->buffers)
                {
                    if (b != nullptr)
                    {
                        m_device->DestroyBuffer(b);
                    }
                }
                m_effectBuffers.Remove(instance);
            }
        }

        // Sampler cache keyed by address/filter combination.
        [[nodiscard]] rhi::Sampler*
        GetOrCreateSampler(rhi::AddressMode u, rhi::AddressMode v,
                           rhi::FilterMode minF = rhi::FilterMode::Linear,
                           rhi::FilterMode magF = rhi::FilterMode::Linear,
                           rhi::MipmapFilterMode mip = rhi::MipmapFilterMode::Linear)
        {
            const u64 key = static_cast<u64>(u) | (static_cast<u64>(v) << 4) |
                            (static_cast<u64>(minF) << 8) | (static_cast<u64>(magF) << 12) |
                            (static_cast<u64>(mip) << 16);
            if (rhi::Sampler** cached = m_samplerCache.Find(key))
            {
                return *cached;
            }

            rhi::SamplerDesc desc{};
            desc.addressU = u;
            desc.addressV = v;
            desc.addressW = rhi::AddressMode::Repeat;
            desc.minFilter = minF;
            desc.magFilter = magF;
            desc.mipmapFilter = mip;
            rhi::Sampler* sampler = nullptr;
            if (!m_device->CreateSampler(desc, sampler).IsOk())
            {
                return m_defaultSampler;
            }
            m_samplerCache.InsertOrAssign(key, sampler);
            return sampler;
        }

    private:
        Status CreateDefaultResources()
        {
            // The default material sampler repeats, the glTF default. Assets rely on it: UVs may
            // lie outside [0,1] (DamagedHelmet's V coordinates are in [1,2]), and clamping smears
            // them into the edge texels. Materials that want clamp get it per sampler through
            // GetOrCreateSampler.
            rhi::SamplerDesc sd{};
            sd.addressU = rhi::AddressMode::Repeat;
            sd.addressV = rhi::AddressMode::Repeat;
            sd.addressW = rhi::AddressMode::Repeat;
            if (!m_device->CreateSampler(sd, m_defaultSampler).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            if (!CreateTexture1x1(Color32{255, 255, 255, 255}, m_whiteTex, m_whiteView))
            {
                return Status{ErrorCode::Unknown};
            }
            if (!CreateTexture1x1(Color32{128, 128, 255, 255}, m_normalTex, m_normalView))
            {
                return Status{ErrorCode::Unknown};
            }
            if (!CreateTexture1x1(Color32{0, 0, 0, 255}, m_blackTex, m_blackView))
            {
                return Status{ErrorCode::Unknown};
            }
            return Status{};
        }

        bool CreateTexture1x1(Color32 color, rhi::Texture*& outTex, rhi::TextureView*& outView)
        {
            rhi::TextureDesc td{};
            td.dimension = rhi::TextureDimension::Texture2D;
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = 1;
            td.height = 1;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"1x1";
            if (!m_device->CreateTexture(td, outTex).IsOk())
            {
                return false;
            }

            const u8 pixel[4] = {color.r, color.g, color.b, color.a};
            rhi::TransferBatch* tb = nullptr;
            if (m_queue->CreateTransferBatch(tb).IsOk() && tb != nullptr)
            {
                rhi::TextureDataLayout layout{};
                layout.bytesPerRow = 4;
                layout.rowsPerImage = 1;
                tb->WriteTexture(outTex, Span<const u8>{pixel, 4}, layout, rhi::Extent3D{1, 1, 1});
                (void)tb->Submit();
                m_queue->DestroyTransferBatch(tb);
            }

            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            return m_device->CreateTextureView(outTex, vd, outView).IsOk();
        }

        // Whether the material has a uniform buffer of any kind: its own or an effect group's.
        [[nodiscard]] static bool HasUniformBuffers(const Material& material) noexcept
        {
            if (material.UniformDataSize() > 0)
            {
                return true;
            }
            for (const MaterialEffectGroup& group : material.EffectGroups())
            {
                if (group.uniformSize > 0)
                {
                    return true;
                }
            }
            return false;
        }

        // The instance's effect group buffers, created on first use and written from its values.
        bool UpdateEffectBuffers(MaterialInstance& instance)
        {
            Material* material = instance.GetMaterial();
            if (material->EffectGroupCount() == 0)
            {
                return true;
            }
            EffectBuffers* buffers = m_effectBuffers.Find(&instance);
            if (buffers == nullptr)
            {
                buffers = &m_effectBuffers.InsertOrAssign(&instance, EffectBuffers{});
            }
            while (buffers->buffers.Size() < material->EffectGroupCount())
            {
                buffers->buffers.PushBack(nullptr);
            }
            for (usize g = 0; g < material->EffectGroupCount(); ++g)
            {
                const MaterialEffectGroup& group = material->GetEffectGroup(g);
                if (group.uniformSize == 0)
                {
                    continue;
                }
                rhi::Buffer*& buffer = buffers->buffers[g];
                if (buffer == nullptr)
                {
                    rhi::BufferDesc bd{};
                    bd.size = group.uniformSize; // a whole row already (Material::SetEffectGroup)
                    bd.usage = rhi::BufferUsage::Uniform;
                    bd.memory = rhi::MemoryLocation::CpuToGpu;
                    if (!m_device->CreateBuffer(bd, buffer).IsOk())
                    {
                        buffer = nullptr;
                        return false;
                    }
                }
                const Span<const u8> data = instance.EffectData(g);
                if (data.Size() > 0 && data.Size() <= group.uniformSize)
                {
                    if (void* ptr = buffer->Map())
                    {
                        EncodeUniformsForGpu(Span<const MaterialPropertyDef>{group.properties.Data(),
                                                                             group.properties.Size()},
                                             data, static_cast<u8*>(ptr));
                        buffer->Unmap();
                    }
                }
            }
            return true;
        }

        bool UpdateUniformBuffer(MaterialInstance& instance)
        {
            Material* material = instance.GetMaterial();
            if (!UpdateEffectBuffers(instance))
            {
                return false;
            }
            if (material->UniformDataSize() == 0)
            {
                return true;
            }

            rhi::Buffer* buffer = nullptr;
            if (rhi::Buffer** existing = m_uniformBuffers.Find(&instance))
            {
                buffer = *existing;
            }
            else
            {
                rhi::BufferDesc bd{};
                // Round up to 16: an HLSL cbuffer is padded to a 16-byte multiple, so the bound
                // range must cover that even when the declared properties sum to less (a
                // {float4, float, float} block is 24 bytes of data but a 32-byte cbuffer).
                bd.size = (material->UniformDataSize() + 15u) & ~15u;
                bd.usage = rhi::BufferUsage::Uniform;
                bd.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(bd, buffer).IsOk())
                {
                    return false;
                }
                m_uniformBuffers.InsertOrAssign(&instance, buffer);
            }

            const Span<const u8> data = instance.UniformData();
            if (data.Size() > 0)
            {
                if (void* ptr = buffer->Map())
                {
                    EncodeUniformsForGpu(*material, data, static_cast<u8*>(ptr));
                    buffer->Unmap();
                }
            }
            return true;
        }

        bool UpdateBindGroup(MaterialInstance& instance, rhi::BindGroupLayout* layout)
        {
            Material* material = instance.GetMaterial();
            Array<rhi::BindGroupEntry> entries;

            if (rhi::Buffer** buf = m_uniformBuffers.Find(&instance))
            {
                entries.PushBack(
                    rhi::BindGroupEntry::BufferEntry(*buf, 0, material->UniformDataSize()));
            }
            // The effect groups' buffers, in the layout's order (GetOrCreateLayout).
            if (material->EffectGroupCount() > 0)
            {
                const EffectBuffers* buffers = m_effectBuffers.Find(&instance);
                for (usize g = 0; g < material->EffectGroupCount(); ++g)
                {
                    const MaterialEffectGroup& group = material->GetEffectGroup(g);
                    if (group.uniformSize == 0)
                    {
                        continue;
                    }
                    if (buffers == nullptr || g >= buffers->buffers.Size() || buffers->buffers[g] == nullptr)
                    {
                        return false; // never written: the layout's entry would go unfilled
                    }
                    entries.PushBack(rhi::BindGroupEntry::BufferEntry(buffers->buffers[g], 0, group.uniformSize));
                }
            }

            usize propIndex = 0;
            for (const MaterialPropertyDef& p : material->Properties())
            {
                if (p.IsTexture())
                {
                    rhi::TextureView* view = instance.GetTexture(propIndex);
                    if (view == nullptr)
                    {
                        // Neutral textures for unbound slots: a normal map decodes to (0,0,1), the
                        // geometric normal; every other map (albedo, metallic-roughness, occlusion,
                        // emissive) multiplies its factor, so white is the identity. For emission
                        // glTF defines factor x texture with the texture white when absent, so a glow
                        // authored as a colour alone shows, and a material that does not glow has a
                        // black EmissiveColor.
                        view = Contains(p.name, u8"ormal") ? m_normalView : m_whiteView;
                    }
                    if (view != nullptr)
                    {
                        entries.PushBack(rhi::BindGroupEntry::TextureEntry(view));
                    }
                }
                else if (p.IsSampler())
                {
                    rhi::Sampler* s = instance.GetSampler(propIndex);
                    if (s == nullptr)
                    {
                        s = GetOrCreateSampler(material->samplerU, material->samplerV);
                    }
                    if (s == nullptr)
                    {
                        s = m_defaultSampler;
                    }
                    if (s != nullptr)
                    {
                        entries.PushBack(rhi::BindGroupEntry::SamplerEntry(s));
                    }
                }
                ++propIndex;
            }
            if (entries.IsEmpty())
            {
                return false;
            }

            if (rhi::BindGroup** old = m_bindGroups.Find(&instance))
            {
                // Deferred free: the replaced group may still be bound by an in-flight frame,
                // and destroying it now would leave that frame drawing with invalid descriptors.
                m_retiredBindGroups.PushBack(RetiredBindGroup{*old, kRetireFrames});
                m_bindGroups.Remove(&instance);
            }

            rhi::BindGroupDesc desc{};
            desc.layout = layout;
            desc.entries = Span<const rhi::BindGroupEntry>{entries.Data(), entries.Size()};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(desc, bg).IsOk())
            {
                return false;
            }
            m_bindGroups.InsertOrAssign(&instance, bg);
            return true;
        }

        static u64 ComputeLayoutHash(Material& material)
        {
            u64 h = 17;
            h = h * 31u + material.UniformDataSize();
            for (const MaterialPropertyDef& p : material.Properties())
            {
                h = h * 31u + static_cast<u64>(p.type);
                h = h * 31u + p.binding;
            }
            // Each effect group adds a binding: a material with one is another layout.
            u64 effectBindings = 0;
            for (const MaterialEffectGroup& group : material.EffectGroups())
            {
                effectBindings += group.uniformSize > 0 ? 1u : 0u;
            }
            h = h * 31u + 0x9E37u + effectBindings;
            return h;
        }

        // Plain substring check; callers search for "ormal" to match "normal" and "Normal".
        static bool Contains(StringView haystack, StringView needle)
        {
            if (needle.Size() == 0 || haystack.Size() < needle.Size())
            {
                return false;
            }
            const usize last = haystack.Size() - needle.Size();
            for (usize i = 0; i <= last; ++i)
            {
                bool match = true;
                for (usize j = 0; j < needle.Size(); ++j)
                {
                    if (haystack[i + j] != needle[j])
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    return true;
                }
            }
            return false;
        }

        void RemoveFromDirty(MaterialInstance* instance)
        {
            for (usize i = 0; i < m_dirty.Size(); ++i)
            {
                if (m_dirty[i] == instance)
                {
                    m_dirty[i] = nullptr;
                    return;
                }
            }
        }

        void Shutdown()
        {
            if (m_device == nullptr)
            {
                return;
            }
            for (auto& e : m_bindGroups)
            {
                m_device->DestroyBindGroup(e.value);
            }
            for (RetiredBindGroup& r : m_retiredBindGroups)
            {
                m_device->DestroyBindGroup(r.bg);
            }
            m_retiredBindGroups.Clear();
            for (auto& e : m_uniformBuffers)
            {
                m_device->DestroyBuffer(e.value);
            }
            for (auto& e : m_effectBuffers)
            {
                for (rhi::Buffer* b : e.value.buffers)
                {
                    if (b != nullptr)
                    {
                        m_device->DestroyBuffer(b);
                    }
                }
            }
            m_effectBuffers.Clear();
            for (auto& e : m_layoutCache)
            {
                m_device->DestroyBindGroupLayout(e.value);
            }
            for (auto& e : m_samplerCache)
            {
                m_device->DestroySampler(e.value);
            }
            m_bindGroups.Clear();
            m_uniformBuffers.Clear();
            m_layoutCache.Clear();
            m_samplerCache.Clear();

            if (m_whiteView)
            {
                m_device->DestroyTextureView(m_whiteView);
            }
            if (m_normalView)
            {
                m_device->DestroyTextureView(m_normalView);
            }
            if (m_blackView)
            {
                m_device->DestroyTextureView(m_blackView);
            }
            if (m_whiteTex)
            {
                m_device->DestroyTexture(m_whiteTex);
            }
            if (m_normalTex)
            {
                m_device->DestroyTexture(m_normalTex);
            }
            if (m_blackTex)
            {
                m_device->DestroyTexture(m_blackTex);
            }
            if (m_defaultSampler)
            {
                m_device->DestroySampler(m_defaultSampler);
            }
            m_device = nullptr;
        }

        // The binding the first effect group's buffer takes in the material's set (b1:
        // foundation.shaders' kShaderEffectRegister).
        static constexpr u32 kFirstEffectBinding = 1;

        rhi::Device* m_device = nullptr;
        rhi::Queue* m_queue = nullptr;
        rhi::Sampler* m_defaultSampler = nullptr;
        static constexpr u32 kRetireFrames = 3;
        struct RetiredBindGroup
        {
            rhi::BindGroup* bg;
            u32 framesLeft;
        };
        Array<RetiredBindGroup> m_retiredBindGroups;

        rhi::Texture* m_whiteTex = nullptr;
        rhi::TextureView* m_whiteView = nullptr;
        rhi::Texture* m_normalTex = nullptr;
        rhi::TextureView* m_normalView = nullptr;
        rhi::Texture* m_blackTex = nullptr;
        rhi::TextureView* m_blackView = nullptr;

        HashMap<u64, rhi::BindGroupLayout*> m_layoutCache;
        HashMap<u64, rhi::Sampler*> m_samplerCache;
        HashMap<MaterialInstance*, rhi::Buffer*> m_uniformBuffers;
        // Each instance's effect group buffers, by group index (null for a group without uniforms).
        struct EffectBuffers
        {
            Array<rhi::Buffer*> buffers;
        };
        HashMap<MaterialInstance*, EffectBuffers> m_effectBuffers;
        HashMap<MaterialInstance*, rhi::BindGroup*> m_bindGroups;
        Array<MaterialInstance*> m_dirty;
    };

} // namespace foundation::materials
