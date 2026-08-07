// UI Sandbox - the first on-screen test of draconic.ui (the Sedulous.UI port). Builds a small View tree
// (a themed FlexLayout panel of controls), styles it with the ported DarkTheme StyleSheet, lays it out
// with UIContext, and renders it through the same VG -> VGRenderer -> RHI path as VGSandbox/GUISandbox.
// Input is driven by a fullscreen InputSurface (gated by an InputRouter) pumped into the UIContext's
// InputManager via UiInputBridge; keyboard/text go through the bridge's event path, and the text field's
// IME is driven from focus (WantsTextInput). A faithful-but-minimal analogue of Sedulous's UISandbox
// (sans the Toolkit), to prove the port end-to-end.

#include <new>
#include <cstdio>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.shell;
import draconic.image;
import draconic.fonts;
import draconic.fonts.ttf;
import draconic.vg;
import draconic.vg.renderer;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.shell;
import draconic.ui.vfs;
import draconic.vfs;
import draconic.runtime;
import draconic.runtime.client;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.ui.runtime;
import draconic.ui.application;
import draconic.ui.viewport;

#include "../../Common/FlyCamera.h" // shared free-fly camera, driven from the viewport's gated devices

using namespace draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace ui = draconic::ui;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;
namespace shell = draconic::shell;
namespace image = draconic::image;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;
namespace vfs = draconic::vfs;
namespace samples = draconic::samples;

namespace
{
    // VG shader source now lives in draconic.vg.renderer (VertexShaderSource/FragmentShaderSource),
    // shared by every VG consumer instead of being copied into each sample.

    // A minimal, self-contained spinning cube rendered via raw RHI into a ViewportView's offscreen
    // RGBA16Float color + Depth32Float depth targets - the payload for the Viewport tab. It only
    // exercises the OnRender(encoder) contract; the real renderer would slot in here instead.
    struct SpinningCube
    {
        static constexpr const char8_t kShader[] = u8R"(
#pragma pack_matrix(row_major)
cbuffer Uniforms : register(b0) { float4x4 MVP; };
struct VSIn { float3 Position : TEXCOORD0; float3 Color : TEXCOORD1; };
struct PSIn { float4 Position : SV_POSITION; float3 Color : COLOR0; };
PSIn VSMain(VSIn i) { PSIn o; o.Position = mul(float4(i.Position, 1.0), MVP); o.Color = i.Color; return o; }
float4 PSMain(PSIn i) : SV_TARGET { return float4(i.Color, 1.0); }
)";
        // 8 corners, each a distinct color; stride = 6 floats (pos3 + color3).
        static constexpr float kVerts[] = {
            -1, -1, -1, 0, 0, 0, 1, -1, -1, 1, 0, 0, 1, 1, -1, 1, 1, 0, -1, 1, -1, 0, 1, 0,
            -1, -1, 1,  0, 0, 1, 1, -1, 1,  1, 0, 1, 1, 1, 1,  1, 1, 1, -1, 1, 1,  0, 1, 1,
        };
        static constexpr u16 kIdx[] = {
            0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
            3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
        };
        struct Uniforms
        {
            Float4x4 mvp;
        };

        void Init(rhi::Device* device, shaders::Compiler* compiler, i32 frameCount,
                  rhi::TextureFormat colorFmt, rhi::TextureFormat depthFmt)
        {
            m_device = device;
            rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics, 0);

            CompileOne(compiler, shaders::ShaderStage::Vertex, u8"VSMain", m_vs);
            CompileOne(compiler, shaders::ShaderStage::Fragment, u8"PSMain", m_ps);

            rhi::BufferDesc vbd{};
            vbd.size = sizeof(kVerts);
            vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
            vbd.memory = rhi::MemoryLocation::GpuOnly;
            device->CreateBuffer(vbd, m_vb);
            rhi::BufferDesc ibd{};
            ibd.size = sizeof(kIdx);
            ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            ibd.memory = rhi::MemoryLocation::GpuOnly;
            device->CreateBuffer(ibd, m_ib);
            rhi::TransferBatch* tb = nullptr;
            queue->CreateTransferBatch(tb);
            tb->WriteBuffer(m_vb, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
            tb->WriteBuffer(m_ib, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
            tb->Submit();
            queue->DestroyTransferBatch(tb);

            rhi::BindGroupLayoutEntry e =
                rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
            rhi::BindGroupLayoutDesc bgld{};
            bgld.entries = Span<const rhi::BindGroupLayoutEntry>(&e, 1);
            device->CreateBindGroupLayout(bgld, m_bgl);
            rhi::BindGroupLayout* layouts[1] = {m_bgl};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(layouts, 1);
            device->CreatePipelineLayout(pld, m_pl);

            // Per-frame uniform ring (frame N+1's CPU write mustn't clobber frame N's in-flight read).
            m_uniforms.Resize(static_cast<usize>(frameCount));
            m_bindGroups.Resize(static_cast<usize>(frameCount));
            for (i32 i = 0; i < frameCount; ++i)
            {
                rhi::BufferDesc ud{};
                ud.size = sizeof(Uniforms);
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                device->CreateBuffer(ud, m_uniforms[static_cast<usize>(i)]);
                rhi::BindGroupEntry bge = rhi::BindGroupEntry::BufferEntry(
                    m_uniforms[static_cast<usize>(i)], 0, sizeof(Uniforms));
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_bgl;
                bgd.entries = Span<const rhi::BindGroupEntry>(&bge, 1);
                device->CreateBindGroup(bgd, m_bindGroups[static_cast<usize>(i)]);
            }

            rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                             {rhi::VertexFormat::Float32x3, 12, 1}};
            rhi::VertexBufferLayout vbl{};
            vbl.stride = 24;
            vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
            rhi::ColorTargetState ct{};
            ct.format = colorFmt; // matches the viewport's owned color format
            rhi::RenderPipelineDesc rpd{};
            rpd.layout = m_pl;
            rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
            rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
            rpd.fragment = rhi::FragmentState{};
            rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
            rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
            rpd.depthStencil = rhi::DepthStencilState{};
            rpd.depthStencil->format = depthFmt;
            rpd.depthStencil->depthWriteEnabled = true;
            rpd.depthStencil->depthCompare = rhi::CompareFunction::Less;
            rpd.primitive.cullMode = rhi::CullMode::None;
            device->CreateRenderPipeline(rpd, m_pipeline);
        }

        void Render(rhi::CommandEncoder& enc, rhi::TextureView* colorView,
                    rhi::TextureView* depthView, u32 w, u32 h, rhi::ClearColor clear,
                    const Float4x4& mvp, i32 frameIndex)
        {
            if (m_pipeline == nullptr)
            {
                return;
            }
            const usize fi = static_cast<usize>(frameIndex);
            Uniforms u{mvp};
            if (u8* p = static_cast<u8*>(m_uniforms[fi]->Map()))
            {
                MemCopy(p, &u, sizeof(u));
                m_uniforms[fi]->Unmap();
            }

            rhi::ColorAttachment ca{};
            ca.view = colorView;
            ca.loadOp = rhi::LoadOp::Clear;
            ca.storeOp = rhi::StoreOp::Store;
            ca.clearValue = clear;
            rhi::DepthStencilAttachment dsa{};
            dsa.view = depthView;
            dsa.depthLoadOp = rhi::LoadOp::Clear;
            dsa.depthStoreOp = rhi::StoreOp::Store;
            dsa.depthClearValue = 1.0f;
            rhi::RenderPassDesc rp{};
            rp.colorAttachments.Add(ca);
            rp.depthStencilAttachment = dsa;
            rhi::RenderPassEncoder* pass = enc.BeginRenderPass(rp);
            if (pass == nullptr)
            {
                return;
            }
            pass->SetViewport(0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f);
            pass->SetScissor(0, 0, w, h);
            pass->SetPipeline(m_pipeline);
            pass->SetBindGroup(0, m_bindGroups[fi]);
            pass->SetVertexBuffer(0, m_vb, 0);
            pass->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
            pass->DrawIndexed(36);
            pass->End();
        }

        void Shutdown()
        {
            if (m_device == nullptr)
            {
                return;
            }
            if (m_pipeline)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            for (usize i = 0; i < m_bindGroups.Size(); ++i)
            {
                if (m_bindGroups[i])
                {
                    m_device->DestroyBindGroup(m_bindGroups[i]);
                }
            }
            for (usize i = 0; i < m_uniforms.Size(); ++i)
            {
                if (m_uniforms[i])
                {
                    m_device->DestroyBuffer(m_uniforms[i]);
                }
            }
            if (m_pl)
            {
                m_device->DestroyPipelineLayout(m_pl);
            }
            if (m_bgl)
            {
                m_device->DestroyBindGroupLayout(m_bgl);
            }
            if (m_ib)
            {
                m_device->DestroyBuffer(m_ib);
            }
            if (m_vb)
            {
                m_device->DestroyBuffer(m_vb);
            }
            if (m_ps)
            {
                m_device->DestroyShaderModule(m_ps);
            }
            if (m_vs)
            {
                m_device->DestroyShaderModule(m_vs);
            }
            m_device = nullptr;
        }

        void CompileOne(shaders::Compiler* compiler, shaders::ShaderStage stage, StringView entry,
                        rhi::ShaderModule*& out)
        {
            const bool isDX12 = (m_device->type == rhi::DeviceType::DX12);
            const shaders::ShaderTarget target =
                isDX12 ? shaders::ShaderTarget::DXIL : shaders::ShaderTarget::SPIRV;
            shaders::CompileOptions opts{};
            opts.shaderModel = u8"6_0";
            opts.optimizationLevel = 3;
            if (!isDX12)
            {
                opts.bindingShifts = shaders::BindingShifts::Standard();
                opts.bindingShiftSets = 4;
            }
            const StringView src(kShader);
            shaders::CompileResult cr{};
            if (compiler->compile(reinterpret_cast<const u8*>(src.Data()), src.Size(), stage, entry,
                                  target, opts, cr) == ErrorCode::Ok)
            {
                rhi::ShaderModuleDesc d{};
                d.code = Span<const u8>(cr.bytecode, cr.bytecodeSize);
                m_device->CreateShaderModule(d, out);
            }
            compiler->freeResult(cr);
        }

        rhi::Device* m_device = nullptr;
        rhi::ShaderModule* m_vs = nullptr;
        rhi::ShaderModule* m_ps = nullptr;
        rhi::Buffer* m_vb = nullptr;
        rhi::Buffer* m_ib = nullptr;
        rhi::BindGroupLayout* m_bgl = nullptr;
        rhi::PipelineLayout* m_pl = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        Array<rhi::Buffer*> m_uniforms;
        Array<rhi::BindGroup*> m_bindGroups;
    };

#ifndef DRACONIC_UI_FONT_PATH
#define DRACONIC_UI_FONT_PATH ""
#endif

#ifndef DRACONIC_UI_ASSET_DIR
#define DRACONIC_UI_ASSET_DIR ""
#endif

    // Appends "<n>" into buf (small values); caller supplies the prefix.
    inline void AppendNum(char8_t* buf, usize& pos, i32 n)
    {
        char8_t d[12];
        usize dc = 0;
        i32 v = n < 0 ? -n : n;
        if (v == 0)
            d[dc++] = u8'0';
        while (v > 0)
        {
            d[dc++] = static_cast<char8_t>(u8'0' + v % 10);
            v /= 10;
        }
        if (n < 0)
            buf[pos++] = u8'-';
        for (usize k = 0; k < dc; ++k)
            buf[pos++] = d[dc - 1 - k];
    }

    // A byte RGBA color for procedural theme images (mirrors Sedulous Color32).
    struct Px
    {
        u8 r, g, b, a;
    };

    // Byte -> float Color helper (Sedulous Color(r,g,b,a) literals).
    inline Color Rgb(f32 r, f32 g, f32 b, f32 a = 255.0f)
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Generate a rounded-rectangle RGBA image with per-corner radii (faithful port of the Sedulous
    // UISandbox MakeRoundedRectImage; the uniform-radius overload there is exactly this with all four
    // radii equal, so it delegates). Corner pixels outside the arc are transparent; the 1px ring just
    // inside the arc (and, for square corners, the outer edge) is drawn in the border color.
    inline UniquePtr<image::OwnedImageData> MakeRoundedRectImage(u32 w, u32 h, Px fill, Px border,
                                                                 i32 rTL, i32 rTR, i32 rBR, i32 rBL)
    {
        Array<u8> data;
        data.Resize(static_cast<usize>(w) * h * 4);

        for (u32 y = 0; y < h; ++y)
        {
            for (u32 x = 0; x < w; ++x)
            {
                bool inside = true;
                bool isBorder = false;

                i32 cx = -1, cy = -1;
                i32 r = 0;
                if (x < static_cast<u32>(rTL) && y < static_cast<u32>(rTL))
                {
                    cx = rTL;
                    cy = rTL;
                    r = rTL;
                }
                else if (x >= w - static_cast<u32>(rTR) && y < static_cast<u32>(rTR))
                {
                    cx = static_cast<i32>(w) - rTR - 1;
                    cy = rTR;
                    r = rTR;
                }
                else if (x < static_cast<u32>(rBL) && y >= h - static_cast<u32>(rBL))
                {
                    cx = rBL;
                    cy = static_cast<i32>(h) - rBL - 1;
                    r = rBL;
                }
                else if (x >= w - static_cast<u32>(rBR) && y >= h - static_cast<u32>(rBR))
                {
                    cx = static_cast<i32>(w) - rBR - 1;
                    cy = static_cast<i32>(h) - rBR - 1;
                    r = rBR;
                }

                if (cx >= 0)
                {
                    const i32 dx = static_cast<i32>(x) - cx;
                    const i32 dy = static_cast<i32>(y) - cy;
                    const f32 dist = Sqrt(static_cast<f32>(dx * dx + dy * dy));
                    if (dist > static_cast<f32>(r))
                    {
                        inside = false;
                    }
                    else if (dist > static_cast<f32>(r - 1))
                    {
                        isBorder = true;
                    }
                }

                if (inside && cx < 0)
                {
                    if (x == 0 || x == w - 1 || y == 0 || y == h - 1)
                    {
                        isBorder = true;
                    }
                }

                const Px c = inside ? (isBorder ? border : fill) : Px{0, 0, 0, 0};
                const usize offset = static_cast<usize>(y * w + x) * 4;
                data[offset] = c.r;
                data[offset + 1] = c.g;
                data[offset + 2] = c.b;
                data[offset + 3] = c.a;
            }
        }

        return MakeUnique<image::OwnedImageData>(DefaultAllocator(), w, h,
                                                 image::PixelFormat::RGBA8, Move(data));
    }

    // Uniform-radius overload (all four corners the same).
    inline UniquePtr<image::OwnedImageData> MakeRoundedRectImage(u32 w, u32 h, Px fill, Px border,
                                                                 i32 radius)
    {
        return MakeRoundedRectImage(w, h, fill, border, radius, radius, radius, radius);
    }

    // A tree row view: draws depth-indented text (TreeView overlays the expand arrows). No DRACONIC_
    // OBJECT (this sample TU imports modules only, not the reflection header) - the adapter recovers it
    // via static_cast since it created the view.
    class TreeItemView final : public draconic::ui::View
    {
    public:
        void Set(StringView text, i32 depth)
        {
            m_text = String(text);
            m_depth = depth;
        }
        void OnDraw(draconic::ui::UIDrawContext& ctx) override
        {
            if (m_text.Size() > 0 && ctx.FontService() != nullptr)
            {
                const f32 textX = static_cast<f32>(m_depth + 1) * m_indent;
                const Color color = ResolveStyleColor(
                    draconic::ui::StyleProperty::TextColor,
                    Color{220.0f / 255.0f, 220.0f / 255.0f, 230.0f / 255.0f, 1.0f});
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), 14.0f))
                {
                    ctx.VG().DrawText(m_text, font, Rectangle{textX, 0, Width() - textX, Height()},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      color);
                }
            }
        }

    private:
        String m_text;
        i32 m_depth = 0;
        f32 m_indent = 20.0f;
    };

    // Demo list adapter: N "Item k" labels.
    class DemoListAdapter final : public draconic::ui::ListAdapterBase
    {
    public:
        explicit DemoListAdapter(i32 count) : m_count(count) {}
        [[nodiscard]] i32 ItemCount() const override { return m_count; }
        [[nodiscard]] RefPtr<draconic::ui::View> CreateView(i32) override
        {
            return MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView{});
        }
        void BindView(draconic::ui::View* view, i32 position) override
        {
            if (auto* label = draconic::foundation::Cast<draconic::ui::Label>(view))
            {
                char8_t buf[24] = u8"Item ";
                usize p = 5;
                AppendNum(buf, p, position + 1);
                buf[p] = 0;
                label->SetText(StringView(buf));
            }
        }

    private:
        i32 m_count;
    };

    // Demo tree adapter: 5 folders x 3 files; folder 0 has a subfolder with 2 files.
    class DemoTreeAdapter final : public draconic::ui::ITreeAdapter
    {
    public:
        [[nodiscard]] i32 RootCount() const override { return 5; }
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
        {
            if (nodeId == -1)
                return 5;
            if (nodeId >= 0 && nodeId < 5)
                return (nodeId == 0) ? 4 : 3;
            if (nodeId == 50)
                return 2;
            return 0;
        }
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
        {
            if (parentId == -1)
                return childIndex;
            if (parentId >= 0 && parentId < 5)
            {
                if (parentId == 0 && childIndex == 3)
                    return 50;
                return 100 + parentId * 10 + childIndex;
            }
            if (parentId == 50)
                return 500 + childIndex;
            return -1;
        }
        [[nodiscard]] i32 GetDepth(i32 nodeId) const override
        {
            if (nodeId >= 500)
                return 2;
            if (nodeId >= 100 || nodeId == 50)
                return 1;
            return 0;
        }
        [[nodiscard]] bool HasChildren(i32 nodeId) const override
        {
            return (nodeId >= 0 && nodeId < 5) || nodeId == 50;
        }
        [[nodiscard]] RefPtr<draconic::ui::View> CreateView(i32) override
        {
            return MakeRef<TreeItemView>(DefaultAllocator());
        }
        void BindView(draconic::ui::View* view, i32 nodeId, i32 depth, bool) override
        {
            auto* item =
                static_cast<TreeItemView*>(view); // adapter created it, so the type is known
            char8_t buf[24];
            usize p = 0;
            const char8_t* pre = HasChildren(nodeId) ? u8"Folder " : u8"File ";
            for (usize k = 0; pre[k] != 0; ++k)
                buf[p++] = pre[k];
            AppendNum(buf, p, nodeId);
            buf[p] = 0;
            item->Set(StringView(buf), depth);
        }
    };

    class DragChip; // fwd

    // Custom drag payload carrying the source chip. No DRACONIC_OBJECT (sample TU) - the drop targets
    // guard on Format() == "demo/chip" and static_cast, since only chips produce that format.
    class ChipDragData final : public draconic::ui::DragData
    {
    public:
        DragChip* SourceChip;
        explicit ChipDragData(DragChip* source)
            : draconic::ui::DragData(u8"demo/chip"), SourceChip(source)
        {
        }
    };

    // A draggable coloured chip (ColorView + IDragSource).
    class DragChip final : public draconic::ui::ColorView, public draconic::ui::IDragSource
    {
    public:
        explicit DragChip(draconic::foundation::Color color)
            : draconic::ui::ColorView(color, 30.0f, 30.0f)
        {
        }
        [[nodiscard]] draconic::ui::IDragSource* AsDragSource() override { return this; }
        [[nodiscard]] RefPtr<draconic::ui::DragData> CreateDragData() override
        {
            return MakeRef<ChipDragData>(DefaultAllocator(), this);
        }
        [[nodiscard]] RefPtr<draconic::ui::View> CreateDragVisual(draconic::ui::DragData*) override
        {
            auto panel = MakeRef<draconic::ui::Panel>(DefaultAllocator());
            panel->Padding = draconic::ui::Thickness{6, 2};
            panel->SetStyle(draconic::ui::StyleProperty::Background,
                            RefPtr<draconic::ui::Drawable>(MakeRef<draconic::ui::ColorDrawable>(
                                DefaultAllocator(), Color.Value())));
            panel->AddView(
                MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView(u8"chip")).Get());
            return panel;
        }
        void OnDragStarted(draconic::ui::DragData*) override { Opacity = 0.4f; }
        void OnDragCompleted(draconic::ui::DragData*, draconic::ui::DragDropEffects, bool) override
        {
            Opacity = 1.0f;
        }
    };

    // A container that accepts chip drops and reorders by swapping colours (FlexLayout + IDropTarget).
    class ChipReorderContainer final : public draconic::ui::FlexLayout,
                                       public draconic::ui::IDropTarget
    {
    public:
        [[nodiscard]] draconic::ui::IDropTarget* AsDropTarget() override { return this; }
        [[nodiscard]] draconic::ui::DragDropEffects CanAcceptDrop(draconic::ui::DragData* data, f32,
                                                                  f32) override
        {
            return data->Format() == StringView(u8"demo/chip")
                       ? draconic::ui::DragDropEffects::Move
                       : draconic::ui::DragDropEffects::None;
        }
        void OnDragEnter(draconic::ui::DragData*, f32, f32) override {}
        void OnDragOver(draconic::ui::DragData*, f32, f32) override {}
        void OnDragLeave(draconic::ui::DragData*) override {}
        [[nodiscard]] draconic::ui::DragDropEffects OnDrop(draconic::ui::DragData* data, f32 localX,
                                                           f32) override
        {
            if (data->Format() != StringView(u8"demo/chip"))
            {
                return draconic::ui::DragDropEffects::None;
            }
            DragChip* source = static_cast<ChipDragData*>(data)->SourceChip;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                draconic::ui::View* child = GetChildAt(i);
                if (localX >= child->Bounds.x && localX < child->Bounds.x + child->Width())
                {
                    DragChip* target = static_cast<DragChip*>(child);
                    if (target != source)
                    {
                        const draconic::foundation::Color tmp = source->Color.Value();
                        source->Color.SetValue(target->Color.Value());
                        target->Color.SetValue(tmp);
                    }
                    return draconic::ui::DragDropEffects::Move;
                }
            }
            return draconic::ui::DragDropEffects::None;
        }
    };

    // A drop box that recolours to the dropped chip's colour (View + IDropTarget).
    class ColorDropBox final : public draconic::ui::View, public draconic::ui::IDropTarget
    {
    public:
        [[nodiscard]] draconic::ui::IDropTarget* AsDropTarget() override { return this; }
        void OnDraw(draconic::ui::UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            ctx.VG().FillRoundedRect(bounds, 4.0f, m_bg);
            ctx.VG().StrokeRoundedRect(
                bounds, 4.0f, Color{70.0f / 255.0f, 75.0f / 255.0f, 85.0f / 255.0f, 1.0f}, 1.0f);
            if (ctx.FontService() != nullptr)
            {
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), 12.0f))
                {
                    ctx.VG().DrawText(
                        m_text.AsView(), font, bounds, fonts::TextAlignment::Center,
                        fonts::VerticalAlignment::Middle,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                }
            }
        }
        [[nodiscard]] draconic::ui::DragDropEffects CanAcceptDrop(draconic::ui::DragData* data, f32,
                                                                  f32) override
        {
            return data->Format() == StringView(u8"demo/chip")
                       ? draconic::ui::DragDropEffects::Copy
                       : draconic::ui::DragDropEffects::None;
        }
        void OnDragEnter(draconic::ui::DragData*, f32, f32) override
        {
            m_text = String(u8"Release!");
            Invalidate();
        }
        void OnDragOver(draconic::ui::DragData*, f32, f32) override {}
        void OnDragLeave(draconic::ui::DragData*) override
        {
            m_text = String(u8"Drop here");
            Invalidate();
        }
        [[nodiscard]] draconic::ui::DragDropEffects OnDrop(draconic::ui::DragData* data, f32,
                                                           f32) override
        {
            if (data->Format() == StringView(u8"demo/chip"))
            {
                m_bg = static_cast<ChipDragData*>(data)->SourceChip->Color.Value();
                m_text = String(u8"Dropped!");
                Invalidate();
            }
            return draconic::ui::DragDropEffects::Copy;
        }

    protected:
        void OnMeasure(draconic::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(30)};
        }

    private:
        String m_text = String(u8"Drop here");
        Color m_bg{50.0f / 255.0f, 55.0f / 255.0f, 65.0f / 255.0f, 1.0f};
    };

    // A bordered area that opens a nested ContextMenu on right-click.
    class ContextMenuDemoArea final : public draconic::ui::View
    {
    public:
        void OnDraw(draconic::ui::UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const Color bg =
                ResolveStyleColor(draconic::ui::StyleProperty::BorderColor,
                                  Color{50.0f / 255.0f, 55.0f / 255.0f, 65.0f / 255.0f, 1.0f});
            ctx.VG().FillRoundedRect(bounds, 4.0f, draconic::ui::Palette::Darken(bg, 0.3f));
            ctx.VG().StrokeRoundedRect(bounds, 4.0f, bg, 1.0f);
            if (ctx.FontService() != nullptr)
            {
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), 14.0f))
                {
                    ctx.VG().DrawText(
                        u8"Right-click for context menu", font, bounds,
                        fonts::TextAlignment::Center, fonts::VerticalAlignment::Middle,
                        Color{180.0f / 255.0f, 185.0f / 255.0f, 200.0f / 255.0f, 1.0f});
                }
            }
        }
        void OnMouseDown(draconic::ui::MouseEventArgs& e) override
        {
            if (e.Button != draconic::ui::MouseButton::Right || Context == nullptr)
            {
                return;
            }
            auto menu = MakeRef<draconic::ui::ContextMenu>(DefaultAllocator());
            menu->AddItem(u8"Cut", []() {});
            menu->AddItem(u8"Copy", []() {});
            menu->AddItem(u8"Paste", []() {});
            menu->AddSeparator();
            draconic::ui::MenuItem* sub = menu->AddSubmenu(u8"More");
            auto* subMenu = draconic::foundation::Cast<draconic::ui::ContextMenu>(sub->Submenu.Get());
            subMenu->AddItem(u8"Select All", []() {});
            subMenu->AddItem(u8"Find", []() {});
            subMenu->AddSeparator();
            draconic::ui::MenuItem* nested = subMenu->AddSubmenu(u8"Even More");
            auto* nestedMenu =
                draconic::foundation::Cast<draconic::ui::ContextMenu>(nested->Submenu.Get());
            nestedMenu->AddItem(u8"Nested Item 1", []() {});
            nestedMenu->AddItem(u8"Nested Item 2", []() {});
            menu->AddSeparator();
            menu->AddItem(u8"Disabled Item", []() {}, false);

            const Float2 screenPos = LocalToScreen(Float2{e.X, e.Y});
            menu->Show(Context, screenPos.x, screenPos.y);
            e.Handled = true;
        }

    protected:
        void OnMeasure(draconic::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(80)};
        }
    };

    // A Button whose tooltip is custom (multi-line) content, via ITooltipProvider.
    class RichTooltipButton final : public draconic::ui::Button,
                                    public draconic::ui::ITooltipProvider
    {
    public:
        explicit RichTooltipButton(StringView text) : draconic::ui::Button(text)
        {
            IsTooltipInteractive = true;
        }
        [[nodiscard]] draconic::ui::ITooltipProvider* AsTooltipProvider() override { return this; }
        [[nodiscard]] RefPtr<draconic::ui::View> CreateTooltipContent() override
        {
            auto layout = MakeRef<draconic::ui::FlexLayout>(DefaultAllocator());
            layout->Direction = draconic::ui::Orientation::Vertical;
            layout->Spacing = 4.0f;
            layout->AddView(
                MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView(u8"Rich Tooltip"))
                    .Get());
            layout->AddView(MakeRef<draconic::ui::Separator>(DefaultAllocator()).Get());
            auto l1 = MakeRef<draconic::ui::Label>(
                DefaultAllocator(), StringView(u8"This tooltip has multiple lines,"));
            l1->AddClass(u8"label-dim");
            layout->AddView(l1.Get());
            auto l2 = MakeRef<draconic::ui::Label>(
                DefaultAllocator(), StringView(u8"a separator, and custom content."));
            l2->AddClass(u8"label-dim");
            layout->AddView(l2.Get());
            auto colorRow = MakeRef<draconic::ui::FlexLayout>(DefaultAllocator());
            colorRow->Direction = draconic::ui::Orientation::Horizontal;
            colorRow->Spacing = 4.0f;
            colorRow->AddView(MakeRef<draconic::ui::ColorView>(
                                  DefaultAllocator(),
                                  Color{220.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                  16.0f, 16.0f)
                                  .Get());
            colorRow->AddView(MakeRef<draconic::ui::ColorView>(
                                  DefaultAllocator(),
                                  Color{60.0f / 255.0f, 180.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                  16.0f, 16.0f)
                                  .Get());
            colorRow->AddView(MakeRef<draconic::ui::ColorView>(
                                  DefaultAllocator(),
                                  Color{60.0f / 255.0f, 60.0f / 255.0f, 220.0f / 255.0f, 1.0f},
                                  16.0f, 16.0f)
                                  .Get());
            layout->AddView(colorRow.Get());
            return layout;
        }
    };

    // Demo grid adapter: N coloured cells.
    class DemoGridAdapter final : public draconic::ui::ListAdapterBase
    {
    public:
        explicit DemoGridAdapter(i32 count) : m_count(count) {}
        [[nodiscard]] i32 ItemCount() const override { return m_count; }
        [[nodiscard]] RefPtr<draconic::ui::View> CreateView(i32) override
        {
            return MakeRef<draconic::ui::ColorView>(
                DefaultAllocator(), Color{100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f},
                0.0f, 0.0f);
        }
        void BindView(draconic::ui::View* view, i32 position) override
        {
            if (auto* cv = draconic::foundation::Cast<draconic::ui::ColorView>(view))
            {
                const f32 r = (60 + (position * 7) % 160) / 255.0f;
                const f32 g = (80 + (position * 13) % 140) / 255.0f;
                const f32 b = (100 + (position * 23) % 120) / 255.0f;
                cv->Color.SetValue(Color{r, g, b, 1.0f});
            }
        }

    private:
        i32 m_count;
    };

    // A flat drag-to-reorder list source for the Toolkit tab's DraggableTreeView (port of the Sedulous
    // UISandbox ReorderableListAdapter). Implements the toolkit IReorderableTreeAdapter (ITreeAdapter +
    // CanMove/MoveItem); the DraggableTreeView borrows it, so the app keeps it alive as a member.
    class ReorderableListAdapter final : public ui::toolkit::IReorderableTreeAdapter
    {
    public:
        explicit ReorderableListAdapter(Span<const StringView> items)
        {
            for (usize i = 0; i < items.Size(); ++i)
            {
                m_items.PushBack(String(items[i]));
            }
        }

        [[nodiscard]] i32 RootCount() const override { return static_cast<i32>(m_items.Size()); }
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
        {
            return (nodeId == -1) ? static_cast<i32>(m_items.Size()) : 0;
        }
        [[nodiscard]] i32 GetChildId(i32, i32 childIndex) const override { return childIndex; }
        [[nodiscard]] i32 GetDepth(i32) const override { return 0; }
        [[nodiscard]] bool HasChildren(i32) const override { return false; }

        [[nodiscard]] RefPtr<draconic::ui::View> CreateView(i32) override
        {
            return MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView{});
        }

        void BindView(draconic::ui::View* view, i32 nodeId, i32, bool) override
        {
            if (auto* label = draconic::foundation::Cast<draconic::ui::Label>(view))
            {
                if (nodeId >= 0 && nodeId < static_cast<i32>(m_items.Size()))
                {
                    label->SetText(m_items[static_cast<usize>(nodeId)].AsView());
                }
            }
        }

        [[nodiscard]] bool CanMove(i32 fromPosition, i32 toPosition) override
        {
            const i32 n = static_cast<i32>(m_items.Size());
            return fromPosition >= 0 && fromPosition < n && toPosition >= 0 && toPosition <= n &&
                   fromPosition != toPosition;
        }

        void MoveItem(i32 fromPosition, i32 toPosition) override
        {
            if (!CanMove(fromPosition, toPosition))
            {
                return;
            }
            String item = m_items[static_cast<usize>(fromPosition)];
            m_items.RemoveAt(static_cast<usize>(fromPosition));
            const i32 insertAt = (toPosition > fromPosition) ? toPosition - 1 : toPosition;
            m_items.Insert(static_cast<usize>(Min(insertAt, static_cast<i32>(m_items.Size()))),
                           Move(item));
        }

    private:
        Array<String> m_items;
    };
}

class UISandbox : public runtime::IApplication
{
public:
    void OnStartup(runtime::IApplicationHost& host) override;
    void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
    void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override;
    void OnShutdown(runtime::IApplicationHost& host) override;

private:
    void BuildUI();
    void LoadFontSize(StringView family, StringView path, f32 pixelHeight);
    [[nodiscard]] bool HasFonts() const
    {
        return !StringView(reinterpret_cast<const utf8char*>(DRACONIC_UI_FONT_PATH)).IsEmpty();
    }
    [[nodiscard]] static RefPtr<ui::FlexLayoutParams> LP(ui::SizeSpec w, ui::SizeSpec h)
    {
        auto p = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        p->Width = w;
        p->Height = h;
        return p;
    }
    [[nodiscard]] static RefPtr<ui::FlexLayoutParams> Grow(f32 g)
    {
        auto p = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        p->Grow = g;
        return p;
    }
    [[nodiscard]] static RefPtr<ui::FlexLayout> VFlex(f32 spacing = 0.0f)
    {
        auto f = MakeRef<ui::FlexLayout>(DefaultAllocator());
        f->Direction = ui::Orientation::Vertical;
        f->Spacing = spacing;
        return f;
    }
    [[nodiscard]] static RefPtr<ui::FlexLayout> HFlex(f32 spacing = 0.0f)
    {
        auto f = MakeRef<ui::FlexLayout>(DefaultAllocator());
        f->Direction = ui::Orientation::Horizontal;
        f->Spacing = spacing;
        return f;
    }
    [[nodiscard]] RefPtr<ui::Panel> MakeBox(Color color, StringView text);
    void BuildControlsTab(ui::TabView* tabView);
    void BuildScrollViewTab(ui::TabView* tabView);
    void BuildLayoutsTab(ui::TabView* tabView);
    void BuildTabPlacementTab(ui::TabView* tabView);
    void BuildTextInputTab(ui::TabView* tabView);
    void BuildDataControlsTab(ui::TabView* tabView);
    void BuildOverlaysTab(ui::TabView* tabView);
    void BuildDragDropTab(ui::TabView* tabView);
    void BuildAnimationsTab(ui::TabView* tabView);
    void BuildToolkitTab(
        ui::TabView* tabView); // draconic.ui.toolkit: bars + split + tree + color picker
    void
    BuildPropertyGridTab(ui::TabView* tabView); // draconic.ui.toolkit: PropertyGrid + all editors
    void
    BuildCurveEditorTab(ui::TabView* tabView); // draconic.ui.toolkit: CurveCanvas tangent editor
    void
    BuildNodeGraphTab(ui::TabView* tabView); // draconic.ui.toolkit: NodeGraphCanvas state machine
    void BuildDockingTab(
        ui::TabView* tabView); // draconic.ui.application: DockManager + OS floating windows
    void BuildViewportTab(
        ui::TabView* tabView); // draconic.ui.viewport: ViewportView hosting a spinning cube
    void BuildPauseMenuTab(
        ui::TabView* tabView); // loads a .sml screen via a VFS-backed resource provider

    // Window size (was provided by SampleApp; re-captured from the main window in OnStartup).
    u32 m_width = 820;
    u32 m_height = 720;

    UniquePtr<fonts::TrueTypeFontService> m_fontService;

    // VFS-backed resource provider for loading the pause-menu .sml (draconic.ui.vfs over a NativeFileSystem
    // rooted at Data/Assets/ui). Kept alive for the app's lifetime (Sedulous keeps mGuiResourceProvider).
    UniquePtr<vfs::NativeFileSystem> m_uiFs;
    UniquePtr<ui::vfs::VfsResourceProvider> m_resProvider;

    // UI.
    RefPtr<ui::RootView> m_root;
    RefPtr<ui::toolkit::ToastHost> m_toastHost; // notification overlay (Overlays tab triggers)
    RefPtr<ui::StyleSheet> m_sheet;
    RefPtr<ui::FlexLayout> m_main;
    UniquePtr<image::OwnedImageData> m_testImage; // borrowed by the ImageView/DrawableView demos
    RefPtr<ui::RepeatButton> m_repeatBtn;         // ticked each frame (hold-to-repeat)
    i32 m_themeIndex =
        0; // 0=Dark 1=Light 2=RoundedDark 3=Textured 4=Breeze(.sss) (Sedulous ApplyTheme)
    RefPtr<ui::Button> m_themeBtn; // shows the current theme name; cycles on click
    void ApplyTheme();
    [[nodiscard]] RefPtr<ui::StyleSheet> CreateTexturedTheme(); // procedurally-skinned image theme
    [[nodiscard]] RefPtr<ui::StyleSheet>
    LoadSSSTheme(StringView path, ui::ThemePalette palette); // .sss via provider
    void EnsureResourceProvider(); // creates m_uiFs + m_resProvider once (rooted at <assets>/ui)
    i32 m_repeatCount = 0;
    UniquePtr<DemoListAdapter> m_listAdapter; // borrowed by the ListView (Data Controls tab)
    UniquePtr<DemoTreeAdapter> m_treeAdapter;
    UniquePtr<DemoGridAdapter> m_gridAdapter;

    // draconic.ui.toolkit: the theme extension must outlive every theme build (ThemeRegistry stores it by
    // pointer), and the DraggableTreeView borrows its reorder adapter, so both live on the app.
    ui::toolkit::ToolkitThemeExtension m_toolkitThemeExt;
    UniquePtr<ReorderableListAdapter> m_reorderAdapter;

    // Docking: the runtime docking host (floats panels into real OS windows) + a handle to the DockManager
    // (the tab tree owns it via AddView; we keep a ref for lifetime clarity). Constructed in OnStartup.
    UniquePtr<ui::application::RuntimeDockableWindowHost> m_dockHost;
    RefPtr<ui::toolkit::DockManager> m_dockManager;

    // draconic.ui.viewport: a ViewportView hosting a raw-RHI spinning cube. The view owns the offscreen
    // RT + gated InputSurface; the app owns an InputRouter and a FlyCamera driven by that surface's gated
    // devices (occlusion-gated by IsHovered()/IsFocused()). Wired in OnStartup after AttachWindow.
    graphics::RenderWindow* m_mainRw = nullptr;
    RefPtr<ui::viewport::ViewportView> m_viewport;
    RefPtr<ui::toolkit::DockManager>
        m_viewportDock; // separate DockManager (the existing Docking tab is untouched)
    graphics::RenderWindow* m_viewportWindow =
        nullptr; // window currently hosting the viewport (tracks undock)
    UniquePtr<shell::InputRouter> m_vpRouter;
    samples::FlyCamera m_cam;
    SpinningCube m_cube;
    shaders::Compiler* m_cubeCompiler = nullptr;
    f32 m_time = 0.0f;
    void WireViewport(runtime::IApplicationHost& host);
    void UpdateViewportHostWindow(); // re-bind the viewport when its dockable panel moves windows

    // The reusable UI-on-runtime bridge (owns the UIContext, per-window VG + input). Declared LAST so it
    // tears down first.
    UniquePtr<ui::runtime::UIHost> m_uiHost;
};

void UISandbox::OnStartup(runtime::IApplicationHost& host)
{
    graphics::RenderWindow* mainRw = host.MainRenderWindow();
    if (mainRw == nullptr)
    {
        return;
    }
    m_width = mainRw->Window().Width();
    m_height = mainRw->Window().Height();

    // Fonts (CPU rasterization; no device needed).
    m_fontService = MakeUnique<fonts::TrueTypeFontService>(DefaultAllocator());
    if (HasFonts())
    {
        const StringView fontPath(reinterpret_cast<const utf8char*>(DRACONIC_UI_FONT_PATH));
        LoadFontSize(u8"Roboto", fontPath, 14.0f);
        LoadFontSize(u8"Roboto", fontPath, 16.0f);
        LoadFontSize(u8"Roboto", fontPath, 24.0f);

        // Decorative families for the pause-menu FontFamily demo (copied from Sedulous assets). GetFont
        // falls back to Roboto if a family is missing, so the demo still renders if these fail to load.
        const StringView monsterPath(reinterpret_cast<const utf8char*>(
            DRACONIC_UI_ASSET_DIR "/fonts/attack-of-monster/Attack Of Monster.ttf"));
        const StringView junglePath(reinterpret_cast<const utf8char*>(
            DRACONIC_UI_ASSET_DIR "/fonts/jungle-adventurer/JungleAdventurer.ttf"));
        const f32 decorativeSizes[] = {14.0f, 18.0f, 24.0f, 32.0f};
        for (f32 s : decorativeSizes)
        {
            LoadFontSize(u8"AttackOfMonster", monsterPath, s);
            LoadFontSize(u8"JungleAdventurer", junglePath, s);
        }
    }

    m_uiHost = MakeUnique<ui::runtime::UIHost>(DefaultAllocator(), *host.Graphics(), *host.Shell(),
                                               *m_fontService);
    // Docking host needs the runtime host + UIHost; construct before BuildUI (the Docking tab uses it).
    m_dockHost =
        MakeUnique<ui::application::RuntimeDockableWindowHost>(DefaultAllocator(), host, *m_uiHost);

    BuildUI(); // builds m_root, sets theme on m_uiHost->Context(), registers m_toolkitThemeExt
    m_uiHost->AttachWindow(mainRw,
                           m_root); // adds the root to the context + wires per-window VG + input

    m_mainRw = mainRw;
    WireViewport(host); // now that the window is attached, RendererFor(mainRw) is valid
}

// Wire the Viewport tab's 3D content after the main window is attached: register the ViewportView's
// color target into the main window's VGRenderer, build the spinning-cube renderer, and set up the gated
// input (own InputRouter + the view's InputSurface). Must run AFTER AttachWindow (RendererFor needs it).
void UISandbox::WireViewport(runtime::IApplicationHost& host)
{
    if (!m_viewport || m_mainRw == nullptr)
    {
        return;
    }

    rhi::Device* device = host.Graphics()->Raw();
    vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(m_mainRw);
    m_viewport->Initialize(device, renderer, host.Shell()->Input(), m_mainRw->Window().Id());
    m_viewportWindow =
        m_mainRw; // starts docked in the main window; UpdateViewportHostWindow tracks undock

    // The spinning-cube renderer (its own shader compiler, kept for the app lifetime).
    (void)shaders::createCompiler(shaders::CompilerDesc{}, m_cubeCompiler);
    if (m_cubeCompiler != nullptr)
    {
        // The cube's pipeline reads the viewport's owned formats - single source of truth, no mismatch.
        m_cube.Init(device, m_cubeCompiler, static_cast<i32>(host.Graphics()->FramesInFlight()),
                    m_viewport->ColorFormat(), m_viewport->DepthFormat());
    }

    // Camera framing the cube; slower move so it stays usable in a small panel.
    m_cam.position = Float3{0.0f, 0.0f, 4.5f};
    m_cam.yaw = 0.0f;
    m_cam.pitch = 0.0f;
    m_cam.moveSpeed = 4.0f;
    m_cam.fastSpeed = 12.0f;

    // Render callback: build the MVP from the camera + spin time and draw the cube into the viewport's
    // offscreen targets (already transitioned to their render states by RenderContent).
    SpinningCube* cube = &m_cube;
    samples::FlyCamera* cam = &m_cam;
    f32* time = &m_time;
    m_viewport->OnRender =
        [cube, cam, time](ui::viewport::ViewportView& v, rhi::CommandEncoder& enc, i32 frameIndex)
    {
        const u32 w = v.RenderWidth(), h = v.RenderHeight();
        if (w == 0 || h == 0)
        {
            return;
        }
        const f32 aspect = static_cast<f32>(w) / static_cast<f32>(h);
        const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0f, aspect, 0.1f, 100.0f);
        const Float4x4 view =
            Float4x4::LookAtRH(cam->position, cam->position + cam->Forward(), cam->Up());
        const Float4x4 model = Float4x4::RotationY(*time) * Float4x4::RotationX(*time * 0.5f);
        const Float4x4 mvp = model * view * proj; // row-vector order (v * M * V * P)
        cube->Render(enc, v.ColorTargetView(), v.DepthTargetView(), w, h, v.ClearColor, mvp,
                     frameIndex);
    };

    // Gated input: the app owns an InputRouter (UIHost's is private); register the view's surface.
    m_vpRouter = MakeUnique<shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());
    if (m_viewport->Surface() != nullptr)
    {
        m_vpRouter->AddSurface(m_viewport->Surface());
    }
}

// Detect when the viewport's dockable panel has moved to a different window (undocked into an OS float, or
// re-docked into the main window) and re-bind the viewport to that window's VGRenderer + window id, so its
// UI can sample the 3D target and input routes correctly. Called each frame after the UI has updated.
void UISandbox::UpdateViewportHostWindow()
{
    if (!m_viewport || !m_uiHost)
    {
        return;
    }
    ui::RootView* root = m_viewport->Root();
    if (root == nullptr)
    {
        return;
    }
    graphics::RenderWindow* host = m_uiHost->WindowForRoot(root);
    if (host == nullptr || host == m_viewportWindow)
    {
        return;
    } // unchanged (or not yet attached)

    // Only switch once the new window's renderer exists (the float's UIHost::AttachWindow ran).
    if (vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(host))
    {
        m_viewport->AttachToWindow(renderer, host->Window().Id());
        m_viewportWindow = host;
    }
}

void UISandbox::LoadFontSize(StringView family, StringView path, f32 pixelHeight)
{
    fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
    options.pixelHeight = pixelHeight;
    (void)m_fontService->LoadFont(family, path, options);
}

void UISandbox::BuildUI()
{
    // Register the toolkit theme extension so draconic.ui.toolkit controls get styled. Must happen before
    // the first theme is built (extensions only apply to themes created afterward).
    ui::ThemeRegistry::RegisterExtension(&m_toolkitThemeExt);
    ApplyTheme(); // Dark by default; the Theme button toggles Dark <-> Light

    m_root = MakeRef<ui::RootView>(DefaultAllocator());
    m_root->ViewportSize = Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)};
    m_root->DpiScale = 1.0f;

    // A 64x64 RGBA checkerboard (8px cells) for the ImageView/DrawableView demos - two blues, matching
    // Sedulous UISandbox's GenerateCheckerboard(64, 64, 8, (100,140,200), (40,50,70)).
    {
        constexpr i32 kSize = 64, kCell = 8;
        static u8 pixels[kSize * kSize * 4];
        const u8 lightC[4] = {100, 140, 200, 255};
        const u8 darkC[4] = {40, 50, 70, 255};
        for (i32 y = 0; y < kSize; ++y)
            for (i32 x = 0; x < kSize; ++x)
            {
                const usize o = static_cast<usize>((y * kSize + x) * 4);
                const bool light = ((x / kCell) + (y / kCell)) % 2 == 0;
                const u8* c = light ? lightC : darkC;
                pixels[o + 0] = c[0];
                pixels[o + 1] = c[1];
                pixels[o + 2] = c[2];
                pixels[o + 3] = c[3];
            }
        m_testImage = MakeUnique<image::OwnedImageData>(DefaultAllocator(), kSize, kSize,
                                                        image::PixelFormat::RGBA8,
                                                        Span<const u8>(pixels, sizeof(pixels)));
    }

    // Main vertical layout filling the window, with a TabView (mirrors Sedulous UISandbox).
    m_main = VFlex();
    m_root->AddView(m_main.Get());

    // Toast overlay across the whole window (input-transparent outside the cards).
    m_toastHost = MakeRef<ui::toolkit::ToastHost>(DefaultAllocator());
    m_root->AddView(m_toastHost.Get());

    // Theme button above the tabs - cycles Dark / Light / Rounded Dark / Textured / Breeze (Sedulous ApplyTheme).
    {
        m_themeBtn = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Theme: Dark"));
        UISandbox* self = this;
        m_themeBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [self](ui::ButtonBase*)
            {
                self->m_themeIndex = (self->m_themeIndex + 1) % 5;
                self->ApplyTheme();
            }});
        m_main->AddView(m_themeBtn.Get(),
                        LP(ui::SizeSpec::Wrap(), ui::SizeSpec::Fixed(ui::Unit::Px(34))));
    }

    auto tabView = MakeRef<ui::TabView>(DefaultAllocator());
    tabView->TabsClosable.SetValue(false);
    m_main->AddView(tabView.Get(), Grow(1));

    BuildControlsTab(tabView.Get());
    BuildScrollViewTab(tabView.Get());
    BuildLayoutsTab(tabView.Get());
    BuildTabPlacementTab(tabView.Get());
    BuildTextInputTab(tabView.Get());
    BuildDataControlsTab(tabView.Get());
    BuildOverlaysTab(tabView.Get());
    BuildDragDropTab(tabView.Get());
    BuildAnimationsTab(tabView.Get());
    BuildToolkitTab(tabView.Get());
    BuildPropertyGridTab(tabView.Get());
    BuildCurveEditorTab(tabView.Get());
    BuildNodeGraphTab(tabView.Get());
    BuildViewportTab(tabView.Get()); // draconic.ui.viewport: a 3D spinning cube in a UI panel
    BuildDockingTab(
        tabView.Get()); // draconic.ui.application: DockManager -> real OS floating windows (last)
    BuildPauseMenuTab(tabView.Get());
}

// Apply the current theme by index and refresh the Theme button label (Sedulous ApplyTheme).
void UISandbox::ApplyTheme()
{
    switch (m_themeIndex)
    {
    case 0:
        m_sheet = ui::DarkTheme::Create();
        break;
    case 1:
        m_sheet = ui::LightTheme::Create();
        break;
    case 2:
        m_sheet = ui::RoundedDarkTheme::Create();
        break;
    case 3:
        m_sheet = CreateTexturedTheme();
        break;
    default:
        m_sheet = LoadSSSTheme(u8"themes/breeze.sss", ui::ThemePalette::Dark());
        break;
    }
    m_uiHost->Context().SetStyleSheet(m_sheet);

    if (m_themeBtn)
    {
        static const char8_t* const kNames[5] = {u8"Dark", u8"Light", u8"Rounded Dark",
                                                 u8"Textured", u8"Breeze (.sss)"};
        String label(u8"Theme: ");
        label += kNames[m_themeIndex];
        m_themeBtn->SetText(label.AsView());
    }
}

// Load a .sss theme file through the VFS-backed resource provider (mirrors Sedulous LoadSSSTheme): a
// StyleSheetLoader with the provider + palette + the built-in SVG icons registered, fed the .sss text.
// Falls back to DarkTheme if the file can't be read.
// Create the VFS-backed resource provider once, rooted at <assets>/ui (shared by the .sss theme loader
// and the pause-menu .sml tab). No-op if already created or if no asset dir was compiled in.
void UISandbox::EnsureResourceProvider()
{
    if (m_resProvider)
    {
        return;
    }
    const StringView assetDir(reinterpret_cast<const utf8char*>(DRACONIC_UI_ASSET_DIR));
    if (assetDir.IsEmpty())
    {
        return;
    }
    String uiRoot(assetDir);
    uiRoot += u8"/ui";
    m_uiFs = MakeUnique<vfs::NativeFileSystem>(DefaultAllocator(), uiRoot.AsView());
    m_resProvider = MakeUnique<ui::vfs::VfsResourceProvider>(DefaultAllocator(), m_uiFs.Get());
}

RefPtr<ui::StyleSheet> UISandbox::LoadSSSTheme(StringView path, ui::ThemePalette palette)
{
    EnsureResourceProvider();
    if (!m_resProvider)
    {
        return ui::DarkTheme::Create();
    }

    // Register the drawable factories + built-in type names so .sss element selectors resolve.
    ui::StyleSheetLoader::InitializeGlobals();

    ui::StyleSheetLoader loader;
    loader.ResourceProvider = m_resProvider.Get();
    loader.SetPalette(palette);
    loader.RegisterSvg(u8"checkmark", ui::ThemeIcons::Checkmark());
    loader.RegisterSvg(u8"radio-mark-square", ui::ThemeIcons::RadioMarkSquare());
    loader.RegisterSvg(u8"radio-mark-round", ui::ThemeIcons::RadioMarkRound());
    loader.RegisterSvg(u8"close", ui::ThemeIcons::Close());
    loader.RegisterSvg(u8"chevron-down", ui::ThemeIcons::ChevronDown());
    loader.RegisterSvg(u8"chevron-right", ui::ThemeIcons::ChevronRight());
    loader.RegisterSvg(u8"arrow-down", ui::ThemeIcons::ArrowDown());
    loader.RegisterSvg(u8"arrow-up", ui::ThemeIcons::ArrowUp());

    String sss;
    if (!m_resProvider->LoadText(path, sss) || sss.IsEmpty())
    {
        return ui::DarkTheme::Create();
    }
    return loader.Load(sss.AsView());
}

// Build a fully image-skinned StyleSheet from procedurally generated rounded-rect images (faithful port
// of the Sedulous UISandbox CreateTexturedTheme). The source images only need to live through Create -
// TexturedTheme::Create packs their pixels into its own atlas - so the keep-alive array drops afterward.
RefPtr<ui::StyleSheet> UISandbox::CreateTexturedTheme()
{
    ui::ThemeImageSet images;
    Array<UniquePtr<image::OwnedImageData>>
        keep; // hold the images alive across the Create/atlas-build call
    auto hold = [&](UniquePtr<image::OwnedImageData> img) -> const image::ImageData*
    {
        const image::ImageData* p = img.Get();
        keep.PushBack(Move(img));
        return p;
    };

    // --- Button: soft blue ---
    const image::ImageData* btnN =
        hold(MakeRoundedRectImage(48, 32, Px{180, 200, 225, 255}, Px{140, 165, 195, 255}, 6));
    const image::ImageData* btnH =
        hold(MakeRoundedRectImage(48, 32, Px{190, 210, 235, 255}, Px{150, 175, 205, 255}, 6));
    const image::ImageData* btnP =
        hold(MakeRoundedRectImage(48, 32, Px{150, 175, 205, 255}, Px{120, 145, 175, 255}, 6));
    const image::ImageData* btnD =
        hold(MakeRoundedRectImage(48, 32, Px{195, 205, 215, 128}, Px{175, 185, 195, 128}, 6));
    images.AddStateImages(u8"button:Background", btnN, btnH, btnP, btnD, nullptr,
                          image::NineSlice(8, 8, 8, 8));

    // --- Panel: light blue-gray ---
    const image::ImageData* panelImg =
        hold(MakeRoundedRectImage(48, 48, Px{220, 230, 240, 255}, Px{185, 200, 220, 255}, 4));
    images.AddImage(u8"panel:Background", panelImg, image::NineSlice(8, 8, 8, 8));

    // --- EditText / NumericField: white with blue border ---
    const image::ImageData* etNorm =
        hold(MakeRoundedRectImage(48, 28, Px{245, 248, 252, 255}, Px{170, 185, 210, 255}, 4));
    const image::ImageData* etFocus =
        hold(MakeRoundedRectImage(48, 28, Px{245, 248, 252, 255}, Px{80, 130, 200, 255}, 4));
    images.AddStateImages(u8"edittext:Background", etNorm, nullptr, nullptr, nullptr, etFocus,
                          image::NineSlice(6, 6, 6, 6));
    images.AddStateImages(u8"numericfield:Background", etNorm, nullptr, nullptr, nullptr, etFocus,
                          image::NineSlice(6, 6, 6, 6));

    // --- NumericField spin buttons: light gray, rounded on one side only ---
    const image::ImageData* spinUpN = hold(
        MakeRoundedRectImage(20, 16, Px{225, 230, 240, 255}, Px{170, 185, 210, 255}, 0, 3, 0, 0));
    const image::ImageData* spinUpH = hold(
        MakeRoundedRectImage(20, 16, Px{210, 218, 230, 255}, Px{150, 170, 200, 255}, 0, 3, 0, 0));
    const image::ImageData* spinUpP = hold(
        MakeRoundedRectImage(20, 16, Px{195, 205, 220, 255}, Px{140, 160, 190, 255}, 0, 3, 0, 0));
    images.AddStateImages(u8"numericfield::spin-up", spinUpN, spinUpH, spinUpP, nullptr, nullptr,
                          image::NineSlice(4, 4, 4, 4));

    const image::ImageData* spinDnN = hold(
        MakeRoundedRectImage(20, 16, Px{225, 230, 240, 255}, Px{170, 185, 210, 255}, 0, 0, 3, 0));
    const image::ImageData* spinDnH = hold(
        MakeRoundedRectImage(20, 16, Px{210, 218, 230, 255}, Px{150, 170, 200, 255}, 0, 0, 3, 0));
    const image::ImageData* spinDnP = hold(
        MakeRoundedRectImage(20, 16, Px{195, 205, 220, 255}, Px{140, 160, 190, 255}, 0, 0, 3, 0));
    images.AddStateImages(u8"numericfield::spin-down", spinDnN, spinDnH, spinDnP, nullptr, nullptr,
                          image::NineSlice(4, 4, 4, 4));

    // --- CheckBox ---
    const image::ImageData* cbUnchecked =
        hold(MakeRoundedRectImage(16, 16, Px{240, 244, 250, 255}, Px{160, 175, 200, 255}, 3));
    const image::ImageData* cbChecked =
        hold(MakeRoundedRectImage(16, 16, Px{80, 140, 220, 255}, Px{60, 120, 200, 255}, 3));
    images.AddImage(u8"checkbox::box", cbUnchecked);
    images.AddImage(u8"checkbox::box:checked", cbChecked);

    // --- RadioButton ---
    const image::ImageData* rbCircle =
        hold(MakeRoundedRectImage(16, 16, Px{240, 244, 250, 255}, Px{160, 175, 200, 255}, 8));
    const image::ImageData* rbDot =
        hold(MakeRoundedRectImage(16, 16, Px{80, 140, 220, 255}, Px{60, 120, 200, 255}, 8));
    images.AddImage(u8"radiobutton::box", rbCircle);
    images.AddImage(u8"radiobutton::box:checked", rbDot);

    // --- Slider ---
    const image::ImageData* slTrack =
        hold(MakeRoundedRectImage(32, 6, Px{195, 205, 220, 255}, Px{195, 205, 220, 0}, 3));
    const image::ImageData* slFill =
        hold(MakeRoundedRectImage(32, 6, Px{80, 140, 220, 255}, Px{80, 140, 220, 0}, 3));
    const image::ImageData* slThumb =
        hold(MakeRoundedRectImage(14, 14, Px{255, 255, 255, 255}, Px{140, 165, 200, 255}, 7));
    images.AddImage(u8"slider::track", slTrack, image::NineSlice(3, 2, 3, 2));
    images.AddImage(u8"slider::fill", slFill, image::NineSlice(3, 2, 3, 2));
    images.AddImage(u8"slider::thumb", slThumb);

    // --- ProgressBar ---
    const image::ImageData* progTrack =
        hold(MakeRoundedRectImage(32, 12, Px{195, 205, 220, 255}, Px{195, 205, 220, 0}, 4));
    const image::ImageData* progFill =
        hold(MakeRoundedRectImage(32, 12, Px{80, 140, 220, 255}, Px{80, 140, 220, 0}, 4));
    images.AddImage(u8"progressbar::track", progTrack, image::NineSlice(4, 4, 4, 4));
    images.AddImage(u8"progressbar::fill", progFill, image::NineSlice(4, 4, 4, 4));

    // --- ToggleSwitch ---
    const image::ImageData* tsOff =
        hold(MakeRoundedRectImage(44, 24, Px{190, 200, 215, 255}, Px{170, 185, 205, 255}, 12));
    const image::ImageData* tsOn =
        hold(MakeRoundedRectImage(44, 24, Px{80, 140, 220, 255}, Px{60, 120, 200, 255}, 12));
    const image::ImageData* tsKnob =
        hold(MakeRoundedRectImage(20, 20, Px{255, 255, 255, 255}, Px{210, 215, 225, 255}, 10));
    images.AddImage(u8"toggleswitch::track", tsOff, image::NineSlice(12, 12, 12, 12));
    images.AddImage(u8"toggleswitch::track:checked", tsOn, image::NineSlice(12, 12, 12, 12));
    images.AddImage(u8"toggleswitch::knob", tsKnob);

    // --- ComboBox ---
    const image::ImageData* cbxN =
        hold(MakeRoundedRectImage(48, 28, Px{240, 244, 250, 255}, Px{170, 185, 210, 255}, 4));
    const image::ImageData* cbxH =
        hold(MakeRoundedRectImage(48, 28, Px{230, 238, 248, 255}, Px{150, 170, 200, 255}, 4));
    images.AddStateImages(u8"combobox:Background", cbxN, cbxH, nullptr, nullptr, nullptr,
                          image::NineSlice(6, 6, 6, 6));

    // --- ScrollBar ---
    const image::ImageData* scrollTrack =
        hold(MakeRoundedRectImage(12, 32, Px{210, 218, 230, 150}, Px{210, 218, 230, 0}, 3));
    const image::ImageData* scrollThumb =
        hold(MakeRoundedRectImage(12, 24, Px{150, 170, 200, 200}, Px{150, 170, 200, 0}, 3));
    images.AddImage(u8"scrollbar::track", scrollTrack, image::NineSlice(4, 6, 4, 6));
    images.AddImage(u8"scrollbar::thumb", scrollThumb, image::NineSlice(4, 6, 4, 6));

    // --- Dialog ---
    const image::ImageData* dialogImg =
        hold(MakeRoundedRectImage(64, 64, Px{235, 240, 248, 255}, Px{170, 185, 210, 255}, 8));
    images.AddImage(u8"dialog:Background", dialogImg, image::NineSlice(10, 10, 10, 10));

    // --- Tooltip ---
    const image::ImageData* tooltipImg =
        hold(MakeRoundedRectImage(32, 24, Px{255, 255, 225, 245}, Px{180, 175, 140, 255}, 4));
    images.AddImage(u8"tooltip:Background", tooltipImg, image::NineSlice(6, 6, 6, 6));

    // --- ContextMenu ---
    const image::ImageData* ctxMenuImg =
        hold(MakeRoundedRectImage(48, 48, Px{240, 244, 250, 255}, Px{175, 190, 215, 255}, 6));
    images.AddImage(u8"contextmenu:Background", ctxMenuImg, image::NineSlice(8, 8, 8, 8));

    const image::ImageData* ctxHover =
        hold(MakeRoundedRectImage(32, 24, Px{80, 140, 220, 60}, Px{80, 140, 220, 0}, 3));
    images.AddImage(u8"contextmenu:MenuItemHoverDrawable", ctxHover, image::NineSlice(4, 4, 4, 4));

    // --- TabView ---
    const image::ImageData* tabStrip =
        hold(MakeRoundedRectImage(48, 32, Px{210, 218, 230, 255}, Px{210, 218, 230, 0}, 0));
    const image::ImageData* tabContent =
        hold(MakeRoundedRectImage(48, 48, Px{228, 234, 244, 255}, Px{228, 234, 244, 0}, 0));
    const image::ImageData* tabActive =
        hold(MakeRoundedRectImage(64, 28, Px{240, 244, 250, 255}, Px{240, 244, 250, 0}, 4));
    const image::ImageData* tabHover =
        hold(MakeRoundedRectImage(64, 28, Px{220, 228, 240, 255}, Px{220, 228, 240, 0}, 4));
    images.AddImage(u8"tabview::strip", tabStrip, image::NineSlice(4, 4, 4, 4));
    images.AddImage(u8"tabview::content", tabContent, image::NineSlice(4, 4, 4, 4));
    images.AddImage(u8"tabview::tab:checked", tabActive, image::NineSlice(6, 6, 6, 4));
    images.AddImage(u8"tabview::tab:hover", tabHover, image::NineSlice(6, 6, 6, 4));

    // --- Expander header ---
    const image::ImageData* expN =
        hold(MakeRoundedRectImage(48, 24, Px{215, 222, 235, 255}, Px{215, 222, 235, 0}, 0));
    const image::ImageData* expH =
        hold(MakeRoundedRectImage(48, 24, Px{205, 215, 230, 255}, Px{205, 215, 230, 0}, 0));
    images.AddImage(u8"expander::header", expN, image::NineSlice(4, 4, 4, 4));
    images.AddImage(u8"expander::header:hover", expH, image::NineSlice(4, 4, 4, 4));

    return ui::TexturedTheme::Create(images, ui::ThemePalette::Light());
}

// === Tab 10: Pause Menu (.sml) - loads a screen from disk through a VFS-backed resource provider ===
// Faithful port of Sedulous UISandboxApp's pause-menu demo: a NativeFileSystem rooted at Data/Assets/ui is
// wrapped in a VfsResourceProvider (draconic.ui.vfs); we read the .sml text through it and hand it to
// === Docking demo (draconic.ui.application) ===
// A DockManager wired to the RuntimeDockableWindowHost: 5 panels laid out IDE-style (Scene center,
// Hierarchy left, Inspector right, Console+Assets bottom-tabbed). Docking/splitting/tabbing happen inside
// this tab; dragging a panel OUT floats it into a real borderless OS window (host.OpenWindow), redockable
// by double-clicking its title bar. Mirrors Sedulous UISandbox tab 9.
void UISandbox::BuildDockingTab(ui::TabView* tabView)
{
    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{8, 8};
    tabView->AddTab(u8"Docking", demo.Get());

    auto dm = MakeRef<ui::toolkit::DockManager>(DefaultAllocator());
    m_dockManager = dm;
    dm->DockableWindowHost = m_dockHost.Get();
    demo->AddView(dm.Get(), Grow(1.0f));

    ui::toolkit::DockablePanel* p1 = dm->AddPanel(
        u8"Scene", MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Scene viewport")).Get());
    ui::toolkit::DockablePanel* p2 = dm->AddPanel(
        u8"Inspector",
        MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Inspector properties")).Get());
    ui::toolkit::DockablePanel* p3 =
        dm->AddPanel(u8"Hierarchy",
                     MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Scene hierarchy")).Get());
    ui::toolkit::DockablePanel* p4 = dm->AddPanel(
        u8"Console", MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Console output")).Get());
    ui::toolkit::DockablePanel* p5 = dm->AddPanel(
        u8"Assets", MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Asset browser")).Get());

    // IDE layout: Scene center, Hierarchy left, Inspector right, Console bottom, Assets tabbed with Console.
    dm->DockPanel(p1, ui::toolkit::DockPosition::Center);
    dm->DockPanel(p3, ui::toolkit::DockPosition::Left);
    dm->DockPanel(p2, ui::toolkit::DockPosition::Right);
    dm->DockPanel(p4, ui::toolkit::DockPosition::Bottom);
    dm->DockPanelRelativeTo(p5, ui::toolkit::DockPosition::Center, p4->Parent);
}

// MarkupLoader::LoadFromString. Then we wire button clicks + inline style overrides + a scoped
// LocalStyleSheet (inline beats local beats theme). The two decorative FontFamilies were copied from
// Sedulous; if they fail to load, GetFont falls back to Roboto and the screen still renders.
void UISandbox::BuildPauseMenuTab(ui::TabView* tabView)
{
    const StringView assetDir(reinterpret_cast<const utf8char*>(DRACONIC_UI_ASSET_DIR));
    if (assetDir.IsEmpty())
    {
        return;
    }

    // Populate the markup element/property registry (idempotent) so the loader can build the .sml tree.
    ui::MarkupLoader::Initialize();
    EnsureResourceProvider();
    if (!m_resProvider)
    {
        return;
    }

    String sml;
    if (!m_resProvider->LoadText(u8"screens/pause-menu.sml", sml) || sml.IsEmpty())
    {
        return;
    }

    RefPtr<ui::View> pauseView =
        ui::MarkupLoader::LoadFromString(sml.AsView(), &m_uiHost->Context());
    if (!pauseView)
    {
        return;
    }

    tabView->AddTab(u8"Pause (.sml)", pauseView.Get(), true);

    // The root Flex is a ViewGroup - FindByName walks its subtree.
    ui::ViewGroup* pauseRoot = draconic::foundation::Cast<ui::ViewGroup>(pauseView.Get());
    if (pauseRoot == nullptr)
    {
        return;
    }

    // Wire button clicks by name (Sedulous logs to Console; we print to stdout).
    if (ui::Button* resumeBtn = pauseRoot->FindByName<ui::Button>(u8"resume-btn"))
    {
        resumeBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [](ui::ButtonBase*) { std::printf("Resume clicked!\n"); }});
        // Inline state-list override: green base with Palette-derived hover/pressed/disabled variants;
        // reactive because ButtonBase passes its ControlState into the drawable's state-aware Draw.
        resumeBtn->SetStyle(
            ui::StyleProperty::Background,
            ui::Palette::CreateStateRounded(Rgb(45, 130, 70), vg::CornerRadii(6.0f)));
    }
    if (ui::Button* settingsBtn = pauseRoot->FindByName<ui::Button>(u8"settings-btn"))
        settingsBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [](ui::ButtonBase*) { std::printf("Settings clicked!\n"); }});
    if (ui::Button* saveBtn = pauseRoot->FindByName<ui::Button>(u8"save-btn"))
        saveBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [](ui::ButtonBase*) { std::printf("Save clicked!\n"); }});
    if (ui::Button* loadBtn = pauseRoot->FindByName<ui::Button>(u8"load-btn"))
        loadBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [](ui::ButtonBase*) { std::printf("Load clicked!\n"); }});
    if (ui::Button* quitBtn = pauseRoot->FindByName<ui::Button>(u8"quit-btn"))
    {
        quitBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [](ui::ButtonBase*) { std::printf("Quit clicked!\n"); }});
        quitBtn->SetStyle(ui::StyleProperty::Background,
                          ui::Palette::CreateStateRounded(Rgb(150, 60, 60), vg::CornerRadii(6.0f)));
    }

    // Inline style demo on the title: override TextColor + FontSize + FontFamily without touching the
    // theme. The inline FontFamily "AttackOfMonster" wins over the local sheet's "JungleAdventurer".
    if (ui::Label* title = pauseRoot->FindByName<ui::Label>(u8"title"))
    {
        title->SetStyle(ui::StyleProperty::TextColor, Rgb(255, 220, 100));
        title->SetStyle(ui::StyleProperty::FontSize, 32.0f);
        title->SetStyle(ui::StyleProperty::FontFamily, StringView(u8"AttackOfMonster"));
    }

    // LocalStyleSheet demo: scope a theming change to the pause subtree. Every Label gets a size + soft
    // text color; every Button gets padding + a rounded gray-blue state-list. Inline overrides above still
    // win (title FontSize 32 beats 14; resume/quit inline state-lists beat the gray-blue default).
    RefPtr<ui::StyleSheet> pauseLocal = MakeRef<ui::StyleSheet>(DefaultAllocator());
    pauseLocal->ForAll().Set(ui::StyleProperty::FontFamily, StringView(u8"JungleAdventurer"));
    pauseLocal->ForType(&ui::Label::StaticType())
        .Set(ui::StyleProperty::FontSize, 14.0f)
        .Set(ui::StyleProperty::TextColor, Rgb(210, 215, 225));
    pauseLocal->ForType(&ui::Button::StaticType())
        .Set(ui::StyleProperty::Padding, ui::Thickness{14, 8})
        .Set(ui::StyleProperty::Background,
             ui::Palette::CreateStateRounded(Rgb(60, 65, 80), vg::CornerRadii(6.0f)));
    pauseView->SetLocalStyleSheet(Move(pauseLocal));
}

// === Tab 9: Animations (ViewAnimator / Storyboard + static-transform hit-testing) ===
void UISandbox::BuildAnimationsTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{12, 8};
    tabView->AddTab(u8"Animations", demo.Get());

    demo->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Animation Target")).Get());
    auto target = MakeRef<ui::ColorView>(
        DefaultAllocator(), Color{80.0f / 255.0f, 160.0f / 255.0f, 1.0f, 1.0f}, 0.0f, 30.0f);
    demo->AddView(target.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(30))));

    ui::View* animTarget = target.Get();
    ui::UIContext* ctx = &m_uiHost->Context();
    auto row = HFlex(6.0f);
    auto animBtn = [&](const char8_t* text, ui::Event<void(ui::ButtonBase*)>::Handler h)
    {
        auto b = MakeRef<ui::Button>(DefaultAllocator(), StringView(text));
        b->OnClick.Add(Move(h));
        row->AddView(b.Get());
    };
    animBtn(u8"Fade Out",
            ui::Event<void(ui::ButtonBase*)>::Handler{
                [ctx, animTarget](ui::ButtonBase*)
                {
                    ctx->Animations()->Add(
                        ui::ViewAnimator::FadeOut(animTarget, 0.5f, ui::Easing::EaseOutCubic));
                }});
    animBtn(u8"Fade In",
            ui::Event<void(ui::ButtonBase*)>::Handler{
                [ctx, animTarget](ui::ButtonBase*)
                {
                    ctx->Animations()->Add(
                        ui::ViewAnimator::FadeIn(animTarget, 0.5f, ui::Easing::EaseOutCubic));
                }});
    animBtn(u8"Bounce",
            ui::Event<void(ui::ButtonBase*)>::Handler{
                [ctx, animTarget](ui::ButtonBase*)
                {
                    auto sb = MakeUnique<ui::Storyboard>(DefaultAllocator(),
                                                         ui::Storyboard::Mode::Sequential);
                    sb->Add(ui::ViewAnimator::ScaleTo(animTarget, 1.0f, 1.3f, 0.15f,
                                                      ui::Easing::EaseOutCubic));
                    sb->Add(ui::ViewAnimator::ScaleTo(animTarget, 1.3f, 1.0f, 0.3f,
                                                      ui::Easing::BounceOut));
                    ctx->Animations()->Add(Move(sb));
                }});
    animBtn(u8"Slide",
            ui::Event<void(ui::ButtonBase*)>::Handler{
                [ctx, animTarget](ui::ButtonBase*)
                {
                    auto sb = MakeUnique<ui::Storyboard>(DefaultAllocator(),
                                                         ui::Storyboard::Mode::Sequential);
                    sb->Add(ui::ViewAnimator::TranslateX(animTarget, 0, 50, 0.3f,
                                                         ui::Easing::EaseOutCubic));
                    sb->Add(ui::ViewAnimator::TranslateX(animTarget, 50, 0, 0.3f,
                                                         ui::Easing::EaseInCubic));
                    ctx->Animations()->Add(Move(sb));
                }});
    demo->AddView(row.Get());

    // Static transforms.
    demo->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 8.0f).Get());
    demo->AddView(
        MakeRef<ui::Label>(DefaultAllocator(),
                           StringView(u8"Static Transforms (click to verify hit-testing)"))
            .Get());
    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    auto clickLabel =
        MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Click a transformed button..."));
    ui::Label* cl = clickLabel.Get();
    auto trow = HFlex(16.0f);
    auto tfBtn = [&](const char8_t* text, ui::ViewTransform t, const char8_t* msg)
    {
        auto b = MakeRef<ui::Button>(DefaultAllocator(), StringView(text));
        b->Transform = t;
        const char8_t* m = msg;
        b->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{[cl, m](ui::ButtonBase*)
                                                                 { cl->SetText(StringView(m)); }});
        trow->AddView(b.Get());
    };
    {
        ui::ViewTransform t;
        t.Rotation = 0.15f;
        tfBtn(u8"Rotated", t, u8"Rotated button clicked!");
    }
    {
        ui::ViewTransform t;
        t.Scale = Float2{1.2f, 1.2f};
        tfBtn(u8"Scaled 1.2x", t, u8"Scaled button clicked!");
    }
    {
        ui::ViewTransform t;
        t.Translation = Float2{10, 5};
        tfBtn(u8"Translated", t, u8"Translated button clicked!");
    }
    demo->AddView(trow.Get());
    demo->AddView(clickLabel.Get());
}

// === Tab 10: Toolkit (MenuBar / Toolbar / BreadcrumbBar / SplitView / DraggableTreeView / ColorPicker / StatusBar) ===
void UISandbox::BuildToolkitTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto demo = VFlex(0.0f);
    tabView->AddTab(u8"Toolkit", demo.Get());

    // MenuBar at top.
    auto menuBar = MakeRef<ui::toolkit::MenuBar>(DefaultAllocator());
    ui::ContextMenu* fileMenu = menuBar->AddMenu(u8"File");
    fileMenu->AddItem(u8"New", Function<void()>{[] {}});
    fileMenu->AddItem(u8"Open", Function<void()>{[] {}});
    fileMenu->AddSeparator();
    fileMenu->AddItem(u8"Exit", Function<void()>{[] {}});
    ui::ContextMenu* editMenu = menuBar->AddMenu(u8"Edit");
    editMenu->AddItem(u8"Undo", Function<void()>{[] {}});
    editMenu->AddItem(u8"Redo", Function<void()>{[] {}});
    editMenu->AddSeparator();
    editMenu->AddItem(u8"Cut", Function<void()>{[] {}});
    editMenu->AddItem(u8"Copy", Function<void()>{[] {}});
    editMenu->AddItem(u8"Paste", Function<void()>{[] {}});
    ui::ContextMenu* viewMenu = menuBar->AddMenu(u8"View");
    viewMenu->AddItem(u8"Zoom In", Function<void()>{[] {}});
    viewMenu->AddItem(u8"Zoom Out", Function<void()>{[] {}});
    demo->AddView(menuBar.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

    // Toolbar below the menu.
    auto toolbar = MakeRef<ui::toolkit::Toolbar>(DefaultAllocator());
    toolbar->AddButton(u8"New");
    toolbar->AddButton(u8"Open");
    toolbar->AddButton(u8"Save");
    toolbar->AddSeparator();
    toolbar->AddToggle(u8"Bold");
    toolbar->AddToggle(u8"Italic");
    demo->AddView(toolbar.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

    // BreadcrumbBar.
    auto breadcrumb = MakeRef<ui::toolkit::BreadcrumbBar>(DefaultAllocator());
    breadcrumb->SetPath(u8"Project/Assets/Textures/Environment");
    demo->AddView(breadcrumb.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

    // Center row: SplitView | DraggableTreeView | ColorPicker.
    auto centerRow = HFlex(4.0f);
    {
        auto p = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        p->Width = SizeSpec::Match();
        p->Grow = 1.0f;
        demo->AddView(centerRow.Get(), Move(p));
    }

    // SplitView with two labeled panes.
    auto splitView =
        MakeRef<ui::toolkit::SplitView>(DefaultAllocator(), ui::Orientation::Horizontal);
    splitView->SetPanes(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Left Pane")).Get(),
                        MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Right Pane")).Get());
    splitView->SetSplitRatio(0.4f);
    centerRow->AddView(splitView.Get(), Grow(1));

    // DraggableTreeView column (drag to reorder).
    auto dragCol = VFlex(4.0f);
    dragCol->AddView(
        MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Drag to reorder:")).Get());
    const StringView reorderItems[] = {u8"Alpha", u8"Bravo", u8"Charlie",
                                       u8"Delta", u8"Echo",  u8"Foxtrot"};
    m_reorderAdapter = MakeUnique<ReorderableListAdapter>(DefaultAllocator(),
                                                          Span<const StringView>(reorderItems, 6));
    auto dragTree = MakeRef<ui::toolkit::DraggableTreeView>(DefaultAllocator());
    dragTree->SetAdapter(m_reorderAdapter.Get());
    dragTree->SetItemHeight(22.0f);
    dragCol->AddView(dragTree.Get(), Grow(1));
    centerRow->AddView(dragCol.Get(), LP(SizeSpec::Fixed(Unit::Px(200)), SizeSpec::Wrap()));

    // ColorPicker.
    auto colorPicker = MakeRef<ui::toolkit::ColorPicker>(DefaultAllocator());
    colorPicker->SetColor(Rgb(80, 160, 240, 255));
    colorPicker->SetOriginalColor(Rgb(80, 160, 240, 255));
    centerRow->AddView(colorPicker.Get());

    // StatusBar at the bottom.
    auto statusBar = MakeRef<ui::toolkit::StatusBar>(DefaultAllocator());
    statusBar->SetText(u8"Ready");
    statusBar->AddSection(u8"Ln 42, Col 8");
    statusBar->AddSection(u8"UTF-8");
    demo->AddView(statusBar.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));
}

// === Tab 11: PropertyGrid (all editor kinds + a categorized Float3 group) ===
void UISandbox::BuildPropertyGridTab(ui::TabView* tabView)
{
    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{8, 8};
    tabView->AddTab(u8"PropertyGrid", demo.Get());

    auto propGrid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
    propGrid->AddProperty(
        MakeRef<ui::toolkit::BoolEditor>(DefaultAllocator(), StringView(u8"Enabled"), true));
    propGrid->AddProperty(
        MakeRef<ui::toolkit::BoolEditor>(DefaultAllocator(), StringView(u8"Visible"), true));
    propGrid->AddProperty(MakeRef<ui::toolkit::StringEditor>(
        DefaultAllocator(), StringView(u8"Name"), StringView(u8"Player")));
    propGrid->AddProperty(MakeRef<ui::toolkit::FloatEditor>(
        DefaultAllocator(), StringView(u8"Speed"), 5.0, 0.0, 100.0, 0.5, 1));
    propGrid->AddProperty(MakeRef<ui::toolkit::IntEditor>(
        DefaultAllocator(), StringView(u8"Health"), static_cast<i64>(100), static_cast<i64>(0),
        static_cast<i64>(999)));
    propGrid->AddProperty(MakeRef<ui::toolkit::RangeEditor>(
        DefaultAllocator(), StringView(u8"Volume"), 0.75f, 0.0f, 1.0f, 0.01f));
    const StringView modeItems[] = {u8"Easy", u8"Normal", u8"Hard"};
    propGrid->AddProperty(MakeRef<ui::toolkit::EnumEditor>(
        DefaultAllocator(), StringView(u8"Mode"), 0, Span<const StringView>(modeItems, 3)));
    propGrid->AddProperty(MakeRef<ui::toolkit::ColorEditor>(
        DefaultAllocator(), StringView(u8"Tint"), Rgb(255, 200, 100, 255)));
    propGrid->AddProperty(MakeRef<ui::toolkit::Float3Editor>(
        DefaultAllocator(), StringView(u8"Position"), Float3{1.0f, 2.5f, -3.0f}, -100000.0f,
        100000.0f, 0.1f, Function<void(Float3)>{}, StringView(u8"Transform")));
    propGrid->AddProperty(MakeRef<ui::toolkit::Float3Editor>(
        DefaultAllocator(), StringView(u8"Rotation"), Float3{0, 45, 0}, -100000.0f, 100000.0f, 0.1f,
        Function<void(Float3)>{}, StringView(u8"Transform")));
    propGrid->AddProperty(MakeRef<ui::toolkit::Float3Editor>(
        DefaultAllocator(), StringView(u8"Scale"), Float3{1, 1, 1}, -100000.0f, 100000.0f, 0.1f,
        Function<void(Float3)>{}, StringView(u8"Transform")));
    demo->AddView(propGrid.Get(), Grow(1));
}

// === Tab 12: Curve Editor (CurveCanvas tangent editor, one channel seeded with an ease-in/out shape) ===
void UISandbox::BuildCurveEditorTab(ui::TabView* tabView)
{
    using ui::SizeSpec;

    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{12, 8};
    tabView->AddTab(u8"Curve Editor", demo.Get());

    auto help = MakeRef<ui::Label>(
        DefaultAllocator(),
        StringView(u8"Left-click empty space: add key.  Left-click + drag key: move.  Right-click "
                   u8"key: delete.\n"
                   u8"Left-click + drag the colored handles on the selected key: edit tangent.  "
                   u8"Right-click handle: cycle TangentMode (Mirrored / Free / Flat)."));
    demo->AddView(help.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

    auto curve = MakeRef<ui::toolkit::CurveCanvas>(DefaultAllocator());
    curve->MaxKeys = 12;

    ui::toolkit::ChannelDescriptor ch;
    ch.Name = String(u8"easeOut");
    ch.StrokeColor = Rgb(120, 220, 160, 255);
    ch.DefaultValue = 0.0f;
    ch.DisplayMin = 0.0f;
    ch.DisplayMax = 1.0f;
    ch.Interpolation = ui::toolkit::CurveInterpolation::Hermite;
    ui::toolkit::ChannelDescriptor chans[1] = {ch};
    curve->SetChannels(Span<const ui::toolkit::ChannelDescriptor>(chans, 1));

    // Seed a default ease-in / ease-out shape so the curving tangents are visible immediately.
    ui::toolkit::CurveCanvas::Key seedKeys[3] = {
        {0.0f, 0.0f, 0.0f, 1.5f, ui::toolkit::TangentMode::Mirrored},
        {0.5f, 0.5f, 1.5f, 1.5f, ui::toolkit::TangentMode::Mirrored},
        {1.0f, 1.0f, 1.5f, 0.0f, ui::toolkit::TangentMode::Mirrored},
    };
    curve->SetKeys(0, Span<const ui::toolkit::CurveCanvas::Key>(seedKeys, 3));
    {
        auto p = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        p->Width = SizeSpec::Match();
        p->Grow = 1.0f;
        demo->AddView(curve.Get(), Move(p));
    }

    auto status = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Selected key: (none)"));
    demo->AddView(status.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

    // Refresh the status line on any key edit (position drags and tangent drags both hit OnKeyChanged).
    ui::toolkit::CurveCanvas* curveRaw = curve.Get();
    ui::Label* statusRaw = status.Get();
    auto doRefresh = [curveRaw, statusRaw]()
    {
        const i32 selCh = curveRaw->SelectedChannel();
        const i32 selKey = curveRaw->SelectedKeyIndex();
        if (selCh < 0 || selKey < 0 || selKey >= curveRaw->GetKeyCount(selCh))
        {
            statusRaw->SetText(StringView(u8"Selected key: (none)"));
            return;
        }
        const ui::toolkit::CurveCanvas::Key key = curveRaw->GetKey(selCh, selKey);
        const char* modeName = "Mirrored";
        switch (key.Mode)
        {
        case ui::toolkit::TangentMode::Mirrored:
            modeName = "Mirrored";
            break;
        case ui::toolkit::TangentMode::Free:
            modeName = "Free";
            break;
        case ui::toolkit::TangentMode::Flat:
            modeName = "Flat";
            break;
        }
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Key #%d  t=%.2f  v=%.2f  tIn=%.2f  tOut=%.2f  mode=%s",
                      selKey, static_cast<double>(key.Time), static_cast<double>(key.Value),
                      static_cast<double>(key.TangentIn), static_cast<double>(key.TangentOut),
                      modeName);
        statusRaw->SetText(StringView(reinterpret_cast<const char8_t*>(buf)));
    };
    curve->OnKeyChanged.Add(
        ui::Event<void(i32, i32)>::Handler{[doRefresh](i32, i32) { doRefresh(); }});
    curve->OnKeyAdded.Add(
        ui::Event<void(i32, i32)>::Handler{[doRefresh](i32, i32) { doRefresh(); }});
    curve->OnKeyRemoved.Add(
        ui::Event<void(i32, i32)>::Handler{[doRefresh](i32, i32) { doRefresh(); }});
}

// === Tab 13: Node Graph (4 nodes + 3 connections, console-logged interaction events) ===
void UISandbox::BuildNodeGraphTab(ui::TabView* tabView)
{
    auto graph = MakeRef<ui::toolkit::NodeGraphCanvas>(DefaultAllocator());
    graph->ShowGrid = true;

    // Idle (node 0).
    auto nodeA = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
    nodeA->Title = String(u8"Idle");
    nodeA->Position = Float2{50, 50};
    nodeA->HeaderColor = Rgb(70, 130, 80, 255);
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Output;
        p.Label = String(u8"Out");
        nodeA->OutputPorts.PushBack(p);
    }
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Input;
        p.Label = String(u8"In");
        nodeA->InputPorts.PushBack(p);
    }
    graph->AddNode(Move(nodeA));

    // Walk (node 1).
    auto nodeB = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
    nodeB->Title = String(u8"Walk");
    nodeB->Position = Float2{300, 50};
    nodeB->HeaderColor = Rgb(70, 100, 180, 255);
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Output;
        p.Label = String(u8"Out");
        nodeB->OutputPorts.PushBack(p);
    }
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Input;
        p.Label = String(u8"In");
        nodeB->InputPorts.PushBack(p);
    }
    graph->AddNode(Move(nodeB));

    // Run (node 2) - has a typed "Speed" input port.
    auto nodeC = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
    nodeC->Title = String(u8"Run");
    nodeC->Subtitle = String(u8"BlendTree1D");
    nodeC->Position = Float2{300, 200};
    nodeC->HeaderColor = Rgb(180, 100, 70, 255);
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Output;
        p.Label = String(u8"Out");
        nodeC->OutputPorts.PushBack(p);
    }
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Input;
        p.Label = String(u8"In");
        nodeC->InputPorts.PushBack(p);
    }
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Input;
        p.Label = String(u8"Speed");
        p.PortType = ui::toolkit::NodeGraphPortType(1, Rgb(100, 200, 100, 255));
        nodeC->InputPorts.PushBack(p);
    }
    graph->AddNode(Move(nodeC));

    // Any State (node 3) - not deletable.
    auto nodeD = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
    nodeD->Title = String(u8"Any State");
    nodeD->Position = Float2{50, 200};
    nodeD->HeaderColor = Rgb(100, 100, 110, 255);
    nodeD->IsDeletable = false;
    {
        ui::toolkit::NodeGraphPort p;
        p.Direction = ui::toolkit::PortDirection::Output;
        p.Label = String();
        nodeD->OutputPorts.PushBack(p);
    }
    graph->AddNode(Move(nodeD));

    // Connections: Idle -> Walk, Idle -> Run, Any State -> Walk.
    {
        ui::toolkit::NodeGraphConnection c;
        c.SourceNodeIndex = 0;
        c.SourcePortIndex = 0;
        c.DestNodeIndex = 1;
        c.DestPortIndex = 0;
        graph->AddConnection(c);
    }
    {
        ui::toolkit::NodeGraphConnection c;
        c.SourceNodeIndex = 0;
        c.SourcePortIndex = 0;
        c.DestNodeIndex = 2;
        c.DestPortIndex = 0;
        graph->AddConnection(c);
    }
    {
        ui::toolkit::NodeGraphConnection c;
        c.SourceNodeIndex = 3;
        c.SourcePortIndex = 0;
        c.DestNodeIndex = 1;
        c.DestPortIndex = 0;
        graph->AddConnection(c);
    }

    // Wire interaction events to stdout (Sedulous logs to Console).
    graph->OnNodeMoved.Add(
        ui::Event<void(i32)>::Handler{[](i32 idx) { std::printf("Node moved: %d\n", idx); }});
    graph->OnConnectionCreated.Add(ui::Event<void(i32)>::Handler{
        [](i32 idx) { std::printf("Connection created: %d\n", idx); }});
    graph->OnNodeDeleted.Add(
        ui::Event<void(i32)>::Handler{[](i32 idx) { std::printf("Node deleted: %d\n", idx); }});
    graph->OnSelectionChanged.Add(
        ui::Event<void()>::Handler{[]() { std::printf("Selection changed\n"); }});
    graph->OnCanvasContextMenu.Add(ui::Event<void(f32, f32)>::Handler{
        [](f32 x, f32 y)
        {
            std::printf("Canvas context menu at (%.1f, %.1f)\n", static_cast<double>(x),
                        static_cast<double>(y));
        }});
    graph->OnNodeContextMenu.Add(ui::Event<void(i32)>::Handler{
        [](i32 idx) { std::printf("Node context menu: %d\n", idx); }});
    graph->OnNodeDoubleClicked.Add(ui::Event<void(i32)>::Handler{
        [](i32 idx) { std::printf("Node double-clicked: %d\n", idx); }});

    tabView->AddTab(u8"Node Graph", graph.Get());
}

// === Tab 8: Drag & Drop (reorderable chips + a colour drop box) ===
void UISandbox::BuildDragDropTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{12, 8};
    tabView->AddTab(u8"Drag & Drop", demo.Get());

    demo->AddView(MakeRef<ui::Label>(DefaultAllocator(),
                                     StringView(u8"Drag chips to reorder, or drop onto the box"))
                      .Get());
    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    auto row = HFlex(8.0f);

    auto chips = MakeRef<ChipReorderContainer>(DefaultAllocator());
    chips->Direction = ui::Orientation::Horizontal;
    chips->Spacing = 4.0f;
    const Color chipColors[5] = {Color{220.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                 Color{60.0f / 255.0f, 180.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                 Color{60.0f / 255.0f, 100.0f / 255.0f, 220.0f / 255.0f, 1.0f},
                                 Color{220.0f / 255.0f, 180.0f / 255.0f, 40.0f / 255.0f, 1.0f},
                                 Color{180.0f / 255.0f, 60.0f / 255.0f, 220.0f / 255.0f, 1.0f}};
    for (const Color& c : chipColors)
    {
        chips->AddView(MakeRef<DragChip>(DefaultAllocator(), c).Get(),
                       LP(SizeSpec::Fixed(Unit::Px(30)), SizeSpec::Fixed(Unit::Px(30))));
    }
    row->AddView(chips.Get());

    {
        auto p = Grow(1);
        p->Height = SizeSpec::Fixed(Unit::Px(30));
        row->AddView(MakeRef<ColorDropBox>(DefaultAllocator()).Get(), p);
    }
    demo->AddView(row.Get());
}

// === Tab 7: Overlays (ComboBox / Dialog / ContextMenu / Tooltips) ===
void UISandbox::BuildOverlaysTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
    scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
    tabView->AddTab(u8"Overlays", scroll.Get());

    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{12, 8};
    scroll->AddView(demo.Get());
    auto section = [&](const char8_t* title)
    {
        demo->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(title)).Get());
        demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());
    };
    auto spacer = [&] { demo->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 4.0f).Get()); };
    const auto w200 = [&] { return LP(SizeSpec::Fixed(Unit::Px(200)), SizeSpec::Wrap()); };

    // ComboBox.
    section(u8"ComboBox");
    {
        auto c = MakeRef<ui::ComboBox>(DefaultAllocator());
        c->AddItem(u8"Option 1");
        c->AddItem(u8"Option 2");
        c->AddItem(u8"Option 3");
        demo->AddView(c.Get(), w200());
    }
    {
        auto c = MakeRef<ui::ComboBox>(DefaultAllocator());
        c->AddItem(u8"Red");
        c->AddItem(u8"Green");
        c->AddItem(u8"Blue");
        c->SetSelectedIndex(1);
        demo->AddView(c.Get(), w200());
    }

    // Dialog.
    spacer();
    section(u8"Dialog");
    {
        auto row = HFlex(8.0f);
        ui::UIContext* ctx = &m_uiHost->Context();
        auto alertBtn = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Alert"));
        alertBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [ctx](ui::ButtonBase*)
            { ui::Dialog::Alert(u8"Information", u8"This is an alert dialog.")->Show(ctx); }});
        row->AddView(alertBtn.Get());
        auto confirmBtn = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Confirm"));
        confirmBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [ctx](ui::ButtonBase*)
            {
                ui::Dialog::Confirm(u8"Confirm", u8"Are you sure you want to proceed?")->Show(ctx);
            }});
        row->AddView(confirmBtn.Get());
        demo->AddView(row.Get());
    }

    // ContextMenu.
    spacer();
    section(u8"ContextMenu (right-click below)");
    demo->AddView(MakeRef<ContextMenuDemoArea>(DefaultAllocator()).Get(),
                  LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(80))));

    // Tooltips.
    spacer();
    section(u8"Tooltips (hover below)");
    {
        auto row = HFlex(8.0f);
        auto tt = [&](const char8_t* text, const char8_t* tip, ui::TooltipPlacement placement,
                      bool interactive)
        {
            auto b = MakeRef<ui::Button>(DefaultAllocator(), StringView(text));
            b->TooltipText = String(tip);
            b->TooltipPlacement = placement;
            b->IsTooltipInteractive = interactive;
            row->AddView(b.Get());
        };
        tt(u8"Bottom tooltip", u8"This appears below", ui::TooltipPlacement::Bottom, false);
        tt(u8"Top tooltip", u8"This appears above", ui::TooltipPlacement::Top, false);
        tt(u8"Right tooltip", u8"This appears on the right", ui::TooltipPlacement::Right, false);
        tt(u8"Interactive", u8"This tooltip stays while you hover it", ui::TooltipPlacement::Bottom,
           true);
        row->AddView(
            MakeRef<RichTooltipButton>(DefaultAllocator(), StringView(u8"Rich content")).Get());
        demo->AddView(row.Get());
    }

    // Toasts (bottom-right overlay; timed severities + a sticky action toast).
    spacer();
    section(u8"Toasts (bottom-right)");
    {
        auto row = HFlex(8.0f);
        ui::toolkit::ToastHost* toasts = m_toastHost.Get();
        auto toastBtn = [&](const char8_t* text, ui::toolkit::ToastSeverity severity,
                            const char8_t* message, f32 duration)
        {
            auto b = MakeRef<ui::Button>(DefaultAllocator(), StringView(text));
            b->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
                [toasts, severity, message, duration](ui::ButtonBase*)
                {
                    ui::toolkit::ToastRequest request;
                    request.message = String(StringView(message));
                    request.severity = severity;
                    request.durationSeconds = duration;
                    (void)toasts->Show(Move(request));
                }});
            row->AddView(b.Get());
        };
        toastBtn(u8"Info", ui::toolkit::ToastSeverity::Info, u8"For your information.", 4.0f);
        toastBtn(u8"Success", ui::toolkit::ToastSeverity::Success, u8"Cook finished: 3 asset(s).",
                 4.0f);
        toastBtn(u8"Warning", ui::toolkit::ToastSeverity::Warning, u8"No importer for 'foo.xyz'.",
                 4.0f);
        toastBtn(u8"Error (sticky)", ui::toolkit::ToastSeverity::Error,
                 u8"Cook: 1 failed (close me).", 0.0f);
        auto actionBtn = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"With action"));
        actionBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [toasts](ui::ButtonBase*)
            {
                ui::toolkit::ToastRequest request;
                request.message = String(u8"Scene saved.");
                request.severity = ui::toolkit::ToastSeverity::Success;
                request.durationSeconds = 0.0f; // sticky so the action stays reachable
                request.actionLabel = String(u8"Undo");
                ui::toolkit::ToastHost* host = toasts;
                request.onAction = Function<void()>{[host]()
                                                    {
                                                        ui::toolkit::ToastRequest ack;
                                                        ack.message = String(u8"Undone.");
                                                        ack.severity =
                                                            ui::toolkit::ToastSeverity::Info;
                                                        ack.durationSeconds = 3.0f;
                                                        (void)host->Show(Move(ack));
                                                    }};
                (void)toasts->Show(Move(request));
            }});
        row->AddView(actionBtn.Get());
        demo->AddView(row.Get());
    }
}

// === Tab 6: Data Controls (virtualized ListView / TreeView / GridView + adapters) ===
void UISandbox::BuildDataControlsTab(ui::TabView* tabView)
{
    auto dataDemo = HFlex(8.0f);
    dataDemo->Padding = ui::Thickness{8};
    tabView->AddTab(u8"Data Controls", dataDemo.Get());

    auto column = [&](const char8_t* title)
    {
        auto col = VFlex(4.0f);
        col->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(title)).Get());
        dataDemo->AddView(col.Get(), Grow(1));
        return col;
    };

    // ListView (1000 items).
    {
        auto col = column(u8"ListView (1000 items)");
        m_listAdapter = MakeUnique<DemoListAdapter>(DefaultAllocator(), 1000);
        auto listView = MakeRef<ui::ListView>(DefaultAllocator());
        listView->SetAdapter(m_listAdapter.Get());
        col->AddView(listView.Get(), Grow(1));
    }

    // TreeView (hierarchy).
    {
        auto col = column(u8"TreeView");
        m_treeAdapter = MakeUnique<DemoTreeAdapter>(DefaultAllocator());
        auto treeView = MakeRef<ui::TreeView>(DefaultAllocator());
        treeView->SetAdapter(m_treeAdapter.Get());
        col->AddView(treeView.Get(), Grow(1));
    }

    // GridView (200 coloured cells).
    {
        auto col = column(u8"GridView (200 cells)");
        m_gridAdapter = MakeUnique<DemoGridAdapter>(DefaultAllocator(), 200);
        auto gridView = MakeRef<ui::GridView>(DefaultAllocator());
        gridView->SetAdapter(m_gridAdapter.Get());
        col->AddView(gridView.Get(), Grow(1));
    }
}

// A themed labelled colour box (Sedulous UISandbox's MakeBox helper).
RefPtr<ui::Panel> UISandbox::MakeBox(Color color, StringView text)
{
    auto panel = MakeRef<ui::Panel>(DefaultAllocator());
    panel->SetStyle(ui::StyleProperty::Background,
                    RefPtr<ui::Drawable>(MakeRef<ui::ColorDrawable>(DefaultAllocator(), color)));
    panel->Padding = ui::Thickness{8, 4, 8, 4};
    auto label = MakeRef<ui::Label>(DefaultAllocator(), text);
    label->FontSize.SetValue(Optional<f32>{11.0f});
    label->HAlign.SetValue(fonts::TextAlignment::Center);
    label->VAlign.SetValue(fonts::VerticalAlignment::Middle);
    panel->AddView(label.Get());
    return panel;
}

// === Tab 2: ScrollView (overlay / reserved / horizontal) ===
void UISandbox::BuildScrollViewTab(ui::TabView* tabView)
{
    auto scrollDemo = HFlex(8.0f);
    scrollDemo->Padding = ui::Thickness{12, 8};
    tabView->AddTab(u8"ScrollView", scrollDemo.Get());

    auto column = [&](const char8_t* title, ui::ScrollBarModeValue mode, ui::ScrollBarPolicy vPol,
                      ui::ScrollBarPolicy hPol, bool horizontal, i32 count)
    {
        auto col = VFlex(4.0f);
        col->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(title)).Get());
        auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
        scroll->ScrollBarMode.SetValue(mode);
        scroll->VScrollBarPolicy.SetValue(vPol);
        scroll->HScrollBarPolicy.SetValue(hPol);
        auto content = horizontal ? HFlex(4.0f) : VFlex(4.0f);
        for (i32 i = 0; i < count; ++i)
        {
            if (horizontal)
            {
                content->AddView(
                    MakeRef<ui::ColorView>(DefaultAllocator(),
                                           Color{(60 + i * 9) / 255.0f, (100 + i * 5) / 255.0f,
                                                 (180 - i * 6) / 255.0f, 1.0f},
                                           60.0f, 60.0f)
                        .Get());
            }
            else
            {
                char8_t buf[24];
                usize p = 0;
                const char8_t* pre = title;
                for (usize k = 0; pre[k] != 0 && k < 8; ++k)
                    buf[p++] = pre[k];
                buf[p++] = u8' ';
                i32 v = i + 1;
                char8_t d[4];
                usize dc = 0;
                if (v == 0)
                {
                    d[dc++] = u8'0';
                }
                while (v > 0)
                {
                    d[dc++] = static_cast<char8_t>(u8'0' + v % 10);
                    v /= 10;
                }
                for (usize k = 0; k < dc; ++k)
                {
                    buf[p++] = d[dc - 1 - k];
                }
                buf[p] = 0;
                content->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(buf)).Get());
            }
        }
        scroll->AddView(content.Get());
        col->AddView(scroll.Get(), Grow(1));
        scrollDemo->AddView(col.Get(), Grow(1));
    };

    column(u8"Overlay Mode", ui::ScrollBarModeValue::Overlay, ui::ScrollBarPolicy::Auto,
           ui::ScrollBarPolicy::Auto, false, 30);
    column(u8"Reserved Mode", ui::ScrollBarModeValue::Reserved, ui::ScrollBarPolicy::Auto,
           ui::ScrollBarPolicy::Auto, false, 30);
    column(u8"Horizontal", ui::ScrollBarModeValue::Reserved, ui::ScrollBarPolicy::Always,
           ui::ScrollBarPolicy::Always, true, 20);
}

// === Tab 3: Layouts (Flex / Dock / Grid / Frame / Flow / Absolute) ===
void UISandbox::BuildLayoutsTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto layoutScroll = MakeRef<ui::ScrollView>(DefaultAllocator());
    layoutScroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
    tabView->AddTab(u8"Layouts", layoutScroll.Get());

    auto demo = VFlex(16.0f);
    demo->Padding = ui::Thickness{12};
    {
        auto lp = MakeRef<ui::LayoutParams>(DefaultAllocator());
        lp->Width = SizeSpec::Match();
        layoutScroll->AddView(demo.Get(), lp);
    }

    auto dimLabel = [&](const char8_t* text)
    {
        auto l = MakeRef<ui::Label>(DefaultAllocator(), StringView(text));
        l->AddClass(u8"label-dim");
        l->FontSize.SetValue(Optional<f32>{12.0f});
        demo->AddView(l.Get());
    };

    // FlexLayout.
    dimLabel(u8"FlexLayout - rows and columns with grow/shrink");
    {
        auto flexH = HFlex(4.0f);
        flexH->AddView(
            MakeBox(Color{100.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"Fixed 80px")
                .Get(),
            LP(SizeSpec::Fixed(Unit::Px(80)), SizeSpec::Fixed(Unit::Px(50))));
        flexH->AddView(
            MakeBox(Color{60.0f / 255.0f, 100.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"Grow 1").Get(),
            [&]
            {
                auto p = Grow(1);
                p->Height = SizeSpec::Fixed(Unit::Px(50));
                return p;
            }());
        flexH->AddView(
            MakeBox(Color{60.0f / 255.0f, 60.0f / 255.0f, 100.0f / 255.0f, 1.0f}, u8"Grow 2").Get(),
            [&]
            {
                auto p = Grow(2);
                p->Height = SizeSpec::Fixed(Unit::Px(50));
                return p;
            }());
        demo->AddView(flexH.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));

        auto flexV = VFlex(4.0f);
        flexV->AddView(
            MakeBox(Color{90.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"Top").Get(),
            LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(30))));
        flexV->AddView(
            MakeBox(Color{50.0f / 255.0f, 90.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"Middle (grow)")
                .Get(),
            [&]
            {
                auto p = Grow(1);
                p->Width = SizeSpec::Match();
                return p;
            }());
        flexV->AddView(
            MakeBox(Color{50.0f / 255.0f, 50.0f / 255.0f, 90.0f / 255.0f, 1.0f}, u8"Bottom").Get(),
            LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(30))));
        demo->AddView(flexV.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(120))));
    }

    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // DockLayout.
    dimLabel(u8"DockLayout - dock children to edges, last fills remaining");
    {
        auto dock = MakeRef<ui::DockLayout>(DefaultAllocator());
        dock->LastChildFill = true;
        auto dockLp = [](ui::Dock d, SizeSpec w, SizeSpec h)
        {
            auto p = MakeRef<ui::DockLayoutParams>(DefaultAllocator(), d);
            p->Width = w;
            p->Height = h;
            return p;
        };
        dock->AddView(
            MakeBox(Color{100.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"Top").Get(),
            dockLp(ui::Dock::Top, SizeSpec::Wrap(), SizeSpec::Fixed(Unit::Px(30))));
        dock->AddView(
            MakeBox(Color{60.0f / 255.0f, 60.0f / 255.0f, 100.0f / 255.0f, 1.0f}, u8"Bottom").Get(),
            dockLp(ui::Dock::Bottom, SizeSpec::Wrap(), SizeSpec::Fixed(Unit::Px(30))));
        dock->AddView(
            MakeBox(Color{60.0f / 255.0f, 100.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"Left").Get(),
            dockLp(ui::Dock::Left, SizeSpec::Fixed(Unit::Px(60)), SizeSpec::Wrap()));
        dock->AddView(
            MakeBox(Color{100.0f / 255.0f, 100.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"Right").Get(),
            dockLp(ui::Dock::Right, SizeSpec::Fixed(Unit::Px(60)), SizeSpec::Wrap()));
        dock->AddView(
            MakeBox(Color{70.0f / 255.0f, 70.0f / 255.0f, 70.0f / 255.0f, 1.0f}, u8"Fill").Get());
        demo->AddView(dock.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(150))));
    }

    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // GridLayout.
    dimLabel(u8"GridLayout - rows and columns with flex/fixed sizing");
    {
        auto grid = MakeRef<ui::GridLayout>(DefaultAllocator());
        grid->Columns.PushBack(ui::TrackSize::Fixed(80));
        grid->Columns.PushBack(ui::TrackSize::Flex(1));
        grid->Columns.PushBack(ui::TrackSize::Flex(2));
        grid->Rows.PushBack(ui::TrackSize::Fixed(35));
        grid->Rows.PushBack(ui::TrackSize::Fixed(35));
        grid->Rows.PushBack(ui::TrackSize::Fixed(35));
        grid->ColumnSpacing = 4;
        grid->RowSpacing = 4;
        auto cell = [](i32 row, i32 col, i32 span)
        {
            auto p = MakeRef<ui::GridLayoutParams>(DefaultAllocator());
            p->Row = row;
            p->Column = col;
            p->ColumnSpan = span;
            return p;
        };
        grid->AddView(
            MakeBox(Color{80.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"0,0").Get(),
            cell(0, 0, 1));
        grid->AddView(
            MakeBox(Color{50.0f / 255.0f, 80.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"0,1").Get(),
            cell(0, 1, 1));
        grid->AddView(
            MakeBox(Color{50.0f / 255.0f, 50.0f / 255.0f, 80.0f / 255.0f, 1.0f}, u8"0,2").Get(),
            cell(0, 2, 1));
        grid->AddView(
            MakeBox(Color{70.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f}, u8"1,0").Get(),
            cell(1, 0, 1));
        grid->AddView(
            MakeBox(Color{40.0f / 255.0f, 70.0f / 255.0f, 40.0f / 255.0f, 1.0f}, u8"Span 2 cols")
                .Get(),
            cell(1, 1, 2));
        grid->AddView(
            MakeBox(Color{60.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f}, u8"Span 3 cols")
                .Get(),
            cell(2, 0, 3));
        demo->AddView(grid.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));
    }

    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // FrameLayout.
    dimLabel(u8"FrameLayout - overlapping children with gravity positioning");
    {
        auto frame = MakeRef<ui::FrameLayout>(DefaultAllocator());
        auto grav = [](ui::Gravity g)
        {
            auto p = MakeRef<ui::FrameLayoutParams>(DefaultAllocator());
            p->Gravity = g;
            return p;
        };
        frame->AddView(MakeBox(Color{40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f},
                               u8"Background (Fill)")
                           .Get(),
                       grav(ui::Gravity::Fill));
        frame->AddView(
            MakeBox(Color{100.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"TopLeft")
                .Get(),
            grav(ui::Gravity::TopLeft));
        frame->AddView(
            MakeBox(Color{50.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"TopRight")
                .Get(),
            grav(ui::Gravity::TopRight));
        frame->AddView(
            MakeBox(Color{50.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 1.0f}, u8"Center").Get(),
            grav(ui::Gravity::Center));
        frame->AddView(
            MakeBox(Color{100.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"BottomLeft")
                .Get(),
            grav(ui::Gravity::BottomLeft));
        frame->AddView(
            MakeBox(Color{100.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 1.0f}, u8"BottomRight")
                .Get(),
            grav(ui::Gravity::BottomRight));
        demo->AddView(frame.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(140))));
    }

    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // FlowLayout.
    dimLabel(u8"FlowLayout - wraps children to next line when space runs out");
    {
        auto flow = MakeRef<ui::FlowLayout>(DefaultAllocator());
        flow->HSpacing = 4.0f;
        flow->VSpacing = 4.0f;
        const char8_t* tags[15] = {u8"Fire", u8"Water",  u8"Earth",   u8"Wind",   u8"Electric",
                                   u8"Dark", u8"Light",  u8"Neutral", u8"Poison", u8"Burn",
                                   u8"Stun", u8"Freeze", u8"Shield",  u8"Heal",   u8"Speed Up"};
        const Color tagColors[15] = {Color{140.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f},
                                     Color{50.0f / 255.0f, 80.0f / 255.0f, 140.0f / 255.0f, 1.0f},
                                     Color{60.0f / 255.0f, 100.0f / 255.0f, 40.0f / 255.0f, 1.0f},
                                     Color{70.0f / 255.0f, 130.0f / 255.0f, 130.0f / 255.0f, 1.0f},
                                     Color{130.0f / 255.0f, 120.0f / 255.0f, 40.0f / 255.0f, 1.0f},
                                     Color{80.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 1.0f},
                                     Color{130.0f / 255.0f, 120.0f / 255.0f, 80.0f / 255.0f, 1.0f},
                                     Color{80.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f},
                                     Color{100.0f / 255.0f, 60.0f / 255.0f, 120.0f / 255.0f, 1.0f},
                                     Color{140.0f / 255.0f, 70.0f / 255.0f, 30.0f / 255.0f, 1.0f},
                                     Color{120.0f / 255.0f, 100.0f / 255.0f, 30.0f / 255.0f, 1.0f},
                                     Color{40.0f / 255.0f, 100.0f / 255.0f, 130.0f / 255.0f, 1.0f},
                                     Color{50.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f},
                                     Color{50.0f / 255.0f, 120.0f / 255.0f, 50.0f / 255.0f, 1.0f},
                                     Color{30.0f / 255.0f, 100.0f / 255.0f, 130.0f / 255.0f, 1.0f}};
        for (i32 i = 0; i < 15; ++i)
        {
            flow->AddView(MakeBox(tagColors[i], StringView(tags[i])).Get());
        }
        demo->AddView(flow.Get(), LP(SizeSpec::Match(), SizeSpec::Wrap()));
    }

    demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // AbsoluteLayout.
    dimLabel(u8"AbsoluteLayout - explicit pixel positioning");
    {
        auto abs = MakeRef<ui::AbsoluteLayout>(DefaultAllocator());
        auto at = [](f32 x, f32 y)
        {
            auto p = MakeRef<ui::AbsoluteLayoutParams>(DefaultAllocator());
            p->X = x;
            p->Y = y;
            return p;
        };
        abs->AddView(
            MakeBox(Color{60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f}, u8"x:0 y:0").Get(),
            at(0, 0));
        abs->AddView(
            MakeBox(Color{100.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"x:100 y:10")
                .Get(),
            at(100, 10));
        abs->AddView(
            MakeBox(Color{50.0f / 255.0f, 100.0f / 255.0f, 50.0f / 255.0f, 1.0f}, u8"x:50 y:60")
                .Get(),
            at(50, 60));
        abs->AddView(
            MakeBox(Color{50.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 1.0f}, u8"x:200 y:40")
                .Get(),
            at(200, 40));
        demo->AddView(abs.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(110))));
    }
}

// === Tab 4: Tab Placement (nested TabViews in a 2x2 grid, closable) ===
void UISandbox::BuildTabPlacementTab(ui::TabView* tabView)
{
    auto demo = MakeRef<ui::GridLayout>(DefaultAllocator());
    demo->Columns.PushBack(ui::TrackSize::Flex(1));
    demo->Columns.PushBack(ui::TrackSize::Flex(1));
    demo->Rows.PushBack(ui::TrackSize::Flex(1));
    demo->Rows.PushBack(ui::TrackSize::Flex(1));
    demo->ColumnSpacing = 4;
    demo->RowSpacing = 4;
    tabView->AddTab(u8"Tab Placement", demo.Get(), true);

    auto cell = [](i32 row, i32 col)
    {
        auto p = MakeRef<ui::GridLayoutParams>(DefaultAllocator());
        p->Row = row;
        p->Column = col;
        return p;
    };
    // Each mini TabView is packed with more tabs than its grid cell can show, so the strip overflows:
    // Top/Bottom overflow HORIZONTALLY, Left/Right overflow VERTICALLY. Wheel over a strip scrolls it;
    // arrow-keying (or clicking) to an off-screen tab auto-scrolls it into view.
    auto placed =
        [&](ui::TabPlacement placement, const char8_t* prefix, i32 count, i32 row, i32 col)
    {
        auto tabs = MakeRef<ui::TabView>(DefaultAllocator());
        tabs->Placement.SetValue(placement);
        for (i32 i = 1; i <= count; ++i)
        {
            StringBuilder sb(DefaultAllocator());
            sb.Append(StringView(prefix)).Append(StringView(u8" ")).AppendInt(i);
            const String title = sb.Take();
            tabs->AddTab(title.AsView(),
                         MakeRef<ui::Label>(DefaultAllocator(), title.AsView()).Get());
        }
        demo->AddView(tabs.Get(), cell(row, col));
    };
    placed(ui::TabPlacement::Top, u8"Top", 24, 0, 0);     // horizontal overflow
    placed(ui::TabPlacement::Bottom, u8"Bot", 24, 0, 1);  // horizontal overflow
    placed(ui::TabPlacement::Left, u8"Left", 24, 1, 0);   // vertical overflow
    placed(ui::TabPlacement::Right, u8"Right", 24, 1, 1); // vertical overflow
}

// === Tab 5: Text Input (EditText / PasswordBox / NumericField / EditableLabel) ===
void UISandbox::BuildTextInputTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
    scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
    tabView->AddTab(u8"Text Input", scroll.Get());

    auto demo = VFlex(8.0f);
    demo->Padding = ui::Thickness{12, 8};
    scroll->AddView(demo.Get());

    const auto w300 = [&] { return LP(SizeSpec::Fixed(Unit::Px(300)), SizeSpec::Wrap()); };
    const auto w200 = [&] { return LP(SizeSpec::Fixed(Unit::Px(200)), SizeSpec::Wrap()); };
    auto section = [&](const char8_t* title)
    {
        demo->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(title)).Get());
        demo->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());
    };

    // --- EditText ---
    section(u8"EditText");
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetText(u8"Editable text");
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetPlaceholder(u8"Enter name...");
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetText(u8"Read-only text");
        e->IsReadOnly.SetValue(true);
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->Multiline.SetValue(true);
        e->SetText(u8"Line 1\nLine 2\nLine 3");
        demo->AddView(e.Get(), LP(SizeSpec::Fixed(Unit::Px(300)), SizeSpec::Fixed(Unit::Px(80))));
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->MaxLength.SetValue(10);
        e->SetPlaceholder(u8"Max 10 chars");
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetFilter(ui::InputFilter::Digits());
        e->SetPlaceholder(u8"Digits only");
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetPrefix(StringView(u8"$"));
        e->SetText(u8"100");
        demo->AddView(e.Get(), w300());
    }
    {
        auto e = MakeRef<ui::EditText>(DefaultAllocator());
        e->SetSuffix(StringView(u8"px"));
        e->SetText(u8"16");
        demo->AddView(e.Get(), w300());
    }

    // --- PasswordBox ---
    demo->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 4.0f).Get());
    section(u8"PasswordBox");
    {
        auto p = MakeRef<ui::PasswordBox>(DefaultAllocator());
        p->SetPlaceholder(u8"Password");
        demo->AddView(p.Get(), w300());
    }
    {
        auto p = MakeRef<ui::PasswordBox>(DefaultAllocator());
        p->PasswordChar.SetValue(U'●');
        p->SetPlaceholder(u8"Custom mask");
        demo->AddView(p.Get(), w300());
    }

    // --- NumericField ---
    demo->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 4.0f).Get());
    section(u8"NumericField");
    {
        auto n = MakeRef<ui::NumericField>(DefaultAllocator());
        n->SetMin(0);
        n->SetMax(100);
        n->SetValue(42);
        demo->AddView(n.Get(), w200());
    }
    {
        auto n = MakeRef<ui::NumericField>(DefaultAllocator());
        n->SetMin(0);
        n->SetMax(100);
        n->ShowSpinButtons.SetValue(false);
        n->SetValue(25);
        demo->AddView(n.Get(), w200());
    }
    {
        auto n = MakeRef<ui::NumericField>(DefaultAllocator());
        n->SetMin(-10);
        n->SetMax(10);
        n->SetStep(0.5);
        n->SetDecimalPlaces(1);
        n->SetValue(0);
        demo->AddView(n.Get(), w200());
    }
    {
        auto n = MakeRef<ui::NumericField>(DefaultAllocator());
        n->SetMin(0);
        n->SetMax(999);
        n->SetDecimalPlaces(0);
        n->SetValue(100);
        demo->AddView(n.Get(), w200());
    }
    {
        auto n = MakeRef<ui::NumericField>(DefaultAllocator());
        n->SetMin(0);
        n->SetMax(360);
        n->SetDecimalPlaces(1);
        n->SetSuffix(StringView(u8"°"));
        n->SetValue(90);
        demo->AddView(n.Get(), w200());
    }

    // Float3 editor: 3 numeric fields with coloured axis prefix labels.
    demo->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Float3 Editor")).Get());
    {
        auto vecRow = HFlex(4.0f);
        auto axisField = [&](const char8_t* axis, Color color, f64 val)
        {
            auto n = MakeRef<ui::NumericField>(DefaultAllocator());
            n->SetMin(-999);
            n->SetMax(999);
            n->SetStep(0.1);
            n->SetDecimalPlaces(2);
            n->ShowSpinButtons.SetValue(false);
            n->SetValue(val);
            auto prefix = MakeRef<ui::Label>(DefaultAllocator(), StringView(axis));
            prefix->TextColor.SetValue(Optional<Color>{color});
            n->SetPrefix(prefix.Get());
            vecRow->AddView(n.Get(), Grow(1));
        };
        axisField(u8"X", Color{220.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f}, 1.06);
        axisField(u8"Y", Color{80.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f, 1.0f}, 0.0);
        axisField(u8"Z", Color{80.0f / 255.0f, 120.0f / 255.0f, 220.0f / 255.0f, 1.0f}, 2.17);
        demo->AddView(vecRow.Get(), LP(SizeSpec::Fixed(Unit::Px(400)), SizeSpec::Wrap()));
    }

    // --- EditableLabel ---
    demo->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 4.0f).Get());
    section(u8"EditableLabel (double-click to edit)");
    {
        auto el = MakeRef<ui::EditableLabel>(DefaultAllocator());
        el->SetText(u8"Double-click me");
        el->SlowClickToEdit.SetValue(false);
        demo->AddView(el.Get(), w300());
    }
    {
        auto el = MakeRef<ui::EditableLabel>(DefaultAllocator());
        el->SetText(u8"Slow-click me");
        el->DoubleClickToEdit.SetValue(false);
        demo->AddView(el.Get(), w300());
    }
    {
        auto el = MakeRef<ui::EditableLabel>(DefaultAllocator());
        el->SetText(u8"With validation");
        el->ValidateRename =
            Function<bool(StringView)>{[](StringView text)
                                       {
                                           const StringView bad = u8"bad";
                                           if (text.Size() < bad.Size())
                                           {
                                               return true;
                                           }
                                           for (usize i = 0; i + bad.Size() <= text.Size(); ++i)
                                           {
                                               if (StringView{text.Data() + i, bad.Size()} == bad)
                                               {
                                                   return false;
                                               }
                                           }
                                           return true;
                                       }};
        demo->AddView(el.Get(), w300());
    }
}

// === Tab 1: Controls === (faithful port of Sedulous UISandbox's Controls tab)
void UISandbox::BuildControlsTab(ui::TabView* tabView)
{
    using ui::SizeSpec;
    using ui::Unit;

    auto body = HFlex(4.0f);
    tabView->AddTab(u8"Controls", body.Get());

    // --- Left panel: control showcase ---
    auto leftPanel = VFlex(8.0f);
    leftPanel->Padding = ui::Thickness{12, 8};
    body->AddView(leftPanel.Get(), LP(SizeSpec::Fixed(Unit::Px(300)), SizeSpec::Wrap()));

    auto btnRow = HFlex(6.0f);
    btnRow->AddView(MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Click Me")).Get());
    {
        auto disabled = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Disabled"));
        disabled->IsEnabled = false;
        btnRow->AddView(disabled.Get());
    }
    btnRow->AddView(MakeRef<ui::ToggleButton>(DefaultAllocator(), StringView(u8"Toggle")).Get());
    leftPanel->AddView(btnRow.Get());

    // ContentButton (icon + text, and a two-line variant).
    auto contentBtnRow = HFlex(6.0f);
    {
        auto iconText = HFlex(6.0f);
        iconText->AlignItems = ui::Align::Center;
        iconText->AddView(
            MakeRef<ui::ColorView>(DefaultAllocator(),
                                   Color{80.0f / 255.0f, 180.0f / 255.0f, 80.0f / 255.0f, 1.0f},
                                   12.0f, 12.0f)
                .Get());
        iconText->AddView(
            MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Icon + Text")).Get());
        contentBtnRow->AddView(MakeRef<ui::ContentButton>(DefaultAllocator(), iconText).Get());

        auto multi = VFlex(2.0f);
        multi->AlignItems = ui::Align::Center;
        auto l1 = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Line 1"));
        l1->FontSize.SetValue(Optional<f32>{12.0f});
        multi->AddView(l1.Get());
        auto l2 = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Line 2"));
        l2->FontSize.SetValue(Optional<f32>{10.0f});
        multi->AddView(l2.Get());
        contentBtnRow->AddView(MakeRef<ui::ContentButton>(DefaultAllocator(), multi).Get());
    }
    leftPanel->AddView(contentBtnRow.Get());

    // RepeatButton + a live count label.
    {
        auto repeatRow = HFlex(6.0f);
        auto repeatLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Count: 0"));
        auto repeatBtn = MakeRef<ui::RepeatButton>(DefaultAllocator(), StringView(u8"Hold Me"));
        ui::Label* lbl = repeatLabel.Get();
        i32* count = &m_repeatCount;
        repeatBtn->OnClick.Add(ui::Event<void(ui::ButtonBase*)>::Handler{
            [lbl, count](ui::ButtonBase*)
            {
                ++(*count);
                char8_t buf[24] = u8"Count: ";
                usize p = 7;
                i32 v = *count;
                char8_t d[8];
                usize dc = 0;
                if (v == 0)
                {
                    d[dc++] = u8'0';
                }
                while (v > 0)
                {
                    d[dc++] = static_cast<char8_t>(u8'0' + v % 10);
                    v /= 10;
                }
                for (usize k = 0; k < dc; ++k)
                {
                    buf[p++] = d[dc - 1 - k];
                }
                buf[p] = 0;
                lbl->SetText(StringView(buf));
            }});
        repeatRow->AddView(repeatBtn.Get());
        repeatRow->AddView(repeatLabel.Get());
        leftPanel->AddView(repeatRow.Get());
        m_repeatBtn = repeatBtn; // ticked each frame in OnUpdate (hold-to-repeat)
    }

    leftPanel->AddView(MakeRef<ui::Spacer>(DefaultAllocator(), 0.0f, 4.0f).Get());

    // Toggle controls.
    leftPanel->AddView(
        MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(u8"Enable sounds"), true).Get());
    leftPanel->AddView(MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(u8"Fullscreen")).Get());
    leftPanel->AddView(MakeRef<ui::ToggleSwitch>(DefaultAllocator(), StringView(u8"VSync")).Get());

    leftPanel->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // Radio group.
    {
        auto radioGroup = MakeRef<ui::RadioGroup>(DefaultAllocator());
        radioGroup->AddRadioButton(
            MakeRef<ui::RadioButton>(DefaultAllocator(), StringView(u8"Low")).Get());
        radioGroup->AddRadioButton(
            MakeRef<ui::RadioButton>(DefaultAllocator(), StringView(u8"Medium")).Get());
        radioGroup->AddRadioButton(
            MakeRef<ui::RadioButton>(DefaultAllocator(), StringView(u8"High")).Get());
        radioGroup->CheckAt(1);
        leftPanel->AddView(radioGroup.Get());
    }

    leftPanel->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // Slider + progress bar.
    leftPanel->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Volume")).Get());
    leftPanel->AddView(MakeRef<ui::Slider>(DefaultAllocator(), 0.0f, 100.0f, 75.0f).Get());
    leftPanel->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Loading...")).Get());
    {
        auto progressBar = MakeRef<ui::ProgressBar>(DefaultAllocator());
        progressBar->Value.SetValue(0.65f);
        leftPanel->AddView(progressBar.Get());
    }

    // --- Center panel: themed Panel with Expanders ---
    auto centerPanel = VFlex(8.0f);
    centerPanel->Padding = ui::Thickness{8};
    body->AddView(centerPanel.Get(), Grow(1));

    {
        auto settingsPanel = MakeRef<ui::Panel>(DefaultAllocator());
        settingsPanel->Padding = ui::Thickness{8};
        settingsPanel->AddClass(u8"panel");
        auto settingsLayout = VFlex(4.0f);
        settingsPanel->AddView(settingsLayout.Get());
        centerPanel->AddView(settingsPanel.Get());

        auto expander1 =
            MakeRef<ui::Expander>(DefaultAllocator(), StringView(u8"Graphics Settings"));
        auto content1 = VFlex(4.0f);
        content1->AddView(
            MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(u8"Anti-Aliasing")).Get());
        content1->AddView(
            MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(u8"Shadows"), true).Get());
        content1->AddView(
            MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(u8"Bloom"), true).Get());
        expander1->SetContent(content1.Get());
        settingsLayout->AddView(expander1.Get());

        auto expander2 = MakeRef<ui::Expander>(DefaultAllocator(), StringView(u8"Audio Settings"));
        auto content2 = VFlex(4.0f);
        content2->AddView(
            MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Master Volume")).Get());
        content2->AddView(MakeRef<ui::Slider>(DefaultAllocator(), 0.0f, 100.0f, 80.0f).Get());
        content2->AddView(
            MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Music Volume")).Get());
        content2->AddView(MakeRef<ui::Slider>(DefaultAllocator(), 0.0f, 100.0f, 50.0f).Get());
        expander2->SetContent(content2.Get());
        settingsLayout->AddView(expander2.Get());
    }

    // --- Right panel: ImageView scale modes + color swatches + SVG drawables ---
    auto rightPanel = VFlex(4.0f);
    rightPanel->Padding = ui::Thickness{4};
    body->AddView(rightPanel.Get(), LP(SizeSpec::Fixed(Unit::Px(200)), SizeSpec::Wrap()));

    const image::ImageData* img = m_testImage.Get();
    auto addImage = [&](const char8_t* label, ui::ScaleType scale, bool clip, Optional<Color> tint)
    {
        rightPanel->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(label)).Get());
        auto iv = MakeRef<ui::ImageView>(DefaultAllocator(), img);
        iv->ScaleType.SetValue(scale);
        iv->ClipsContent = clip;
        if (tint.HasValue())
        {
            iv->Tint.SetValue(tint.Value());
        }
        rightPanel->AddView(iv.Get(), LP(SizeSpec::Match(), SizeSpec::Fixed(Unit::Px(48))));
    };
    addImage(u8"None", ui::ScaleType::None, true, Optional<Color>{});
    addImage(u8"FitCenter", ui::ScaleType::FitCenter, false, Optional<Color>{});
    addImage(u8"FillBounds", ui::ScaleType::FillBounds, false, Optional<Color>{});
    addImage(u8"CenterCrop", ui::ScaleType::CenterCrop, false, Optional<Color>{});
    addImage(u8"Tinted", ui::ScaleType::FitCenter, false,
             Optional<Color>{Color{1.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f}});

    rightPanel->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // Color swatches (FlowLayout).
    rightPanel->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"ColorView")).Get());
    {
        auto swatchFlow = MakeRef<ui::FlowLayout>(DefaultAllocator());
        swatchFlow->HSpacing = 4.0f;
        swatchFlow->VSpacing = 4.0f;
        const Color swatches[8] = {Color{220.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                   Color{60.0f / 255.0f, 180.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                   Color{60.0f / 255.0f, 60.0f / 255.0f, 220.0f / 255.0f, 1.0f},
                                   Color{220.0f / 255.0f, 180.0f / 255.0f, 40.0f / 255.0f, 1.0f},
                                   Color{180.0f / 255.0f, 60.0f / 255.0f, 180.0f / 255.0f, 1.0f},
                                   Color{60.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f},
                                   Color{220.0f / 255.0f, 120.0f / 255.0f, 60.0f / 255.0f, 1.0f},
                                   Color{120.0f / 255.0f, 60.0f / 255.0f, 220.0f / 255.0f, 1.0f}};
        for (const Color& c : swatches)
        {
            swatchFlow->AddView(MakeRef<ui::ColorView>(DefaultAllocator(), c, 40.0f, 40.0f).Get());
        }
        rightPanel->AddView(swatchFlow.Get());
    }

    rightPanel->AddView(MakeRef<ui::Separator>(DefaultAllocator()).Get());

    // DrawableView + SVG drawables.
    rightPanel->AddView(
        MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"DrawableView + SVG")).Get());
    {
        auto svgRow = MakeRef<ui::FlowLayout>(DefaultAllocator());
        svgRow->HSpacing = 6.0f;
        svgRow->VSpacing = 6.0f;
        auto addSvg = [&](StringView svg, f32 sz, Optional<Color> tint)
        {
            RefPtr<ui::SVGDrawable> d = tint.HasValue()
                                            ? ui::SVGDrawable::FromString(svg, tint.Value())
                                            : ui::SVGDrawable::FromString(svg);
            if (d)
            {
                svgRow->AddView(
                    MakeRef<ui::DrawableView>(DefaultAllocator(), RefPtr<ui::Drawable>(d), sz, sz)
                        .Get());
            }
        };
        addSvg(u8"<svg viewBox=\"0 0 48 48\"><circle cx=\"24\" cy=\"24\" r=\"22\" fill=\"#2A6BC0\" "
               u8"stroke=\"#1A4A90\" stroke-width=\"2\"/><text x=\"24\" y=\"30\" "
               u8"text-anchor=\"middle\" font-size=\"18\" font-weight=\"bold\" "
               u8"fill=\"#FFFFFF\">UI</text></svg>",
               40.0f, Optional<Color>{});
        addSvg(u8"<svg viewBox=\"0 0 24 24\"><path d=\"M12 2L15.09 8.26L22 9.27L17 14.14L18.18 "
               u8"21.02L12 17.77L5.82 21.02L7 14.14L2 9.27L8.91 8.26L12 2Z\" fill=\"#FFD700\" "
               u8"stroke=\"#B8960F\" stroke-width=\"0.8\"/></svg>",
               32.0f, Optional<Color>{});
        addSvg(u8"<svg viewBox=\"0 0 24 24\"><path d=\"M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 "
               u8"8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 "
               u8"3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z\" fill=\"#FF4444\"/></svg>",
               32.0f, Optional<Color>{});
        addSvg(u8"<svg viewBox=\"0 0 24 24\"><path d=\"M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 "
               u8"8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 "
               u8"3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z\" fill=\"#FF4444\"/></svg>",
               32.0f, Optional<Color>{Color{100.0f / 255.0f, 200.0f / 255.0f, 1.0f, 1.0f}});
        rightPanel->AddView(svgRow.Get());
    }
}

void UISandbox::BuildViewportTab(ui::TabView* tabView)
{
    auto body = VFlex(6.0f);
    body->Padding = ui::Thickness{8, 8};
    body->AddView(
        MakeRef<ui::Label>(DefaultAllocator(),
                           StringView(u8"draconic.ui.viewport - a 3D spinning cube hosted in a "
                                      u8"dockable UI panel. Hover + hold RMB to "
                                      u8"look, WASD/QE to move, wheel to zoom; input is gated to "
                                      u8"the viewport (move off and the camera "
                                      u8"stops, the cube keeps spinning). Drag the Viewport "
                                      u8"panel's tab OUT to float it into its own OS "
                                      u8"window - the cube keeps rendering and stays input-gated "
                                      u8"there (undock re-binds it to that "
                                      u8"window's renderer)."))
            .Get(),
        LP(ui::SizeSpec::Match(), ui::SizeSpec::Wrap()));

    // A SEPARATE DockManager (shares the app's RuntimeDockableWindowHost with the existing Docking tab,
    // which is left untouched). The viewport lives in a dockable panel, so it can be floated into an OS window.
    auto dm = MakeRef<ui::toolkit::DockManager>(DefaultAllocator());
    m_viewportDock = dm;
    dm->DockableWindowHost = m_dockHost.Get();
    body->AddView(dm.Get(), Grow(1.0f));

    m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
    m_viewport->SetFitMode(
        FitMode::Letterbox); // preserve the cube's aspect; bars visualize the fit region
    ui::toolkit::DockablePanel* vpPanel = dm->AddPanel(u8"Viewport", m_viewport.Get());
    ui::toolkit::DockablePanel* infoPanel = dm->AddPanel(
        u8"Inspector",
        MakeRef<ui::Label>(DefaultAllocator(),
                           StringView(u8"Drag the Viewport tab out to float it into an OS window."))
            .Get());
    dm->DockPanel(vpPanel, ui::toolkit::DockPosition::Center);
    dm->DockPanel(infoPanel, ui::toolkit::DockPosition::Right);

    const i32 idx = tabView->AddTab(u8"Viewport (3D)", body.Get());
    tabView->SetSelectedIndex(
        idx); // open on the viewport so the 3D cube + gating are the first thing shown
}

void UISandbox::OnUpdate(runtime::IApplicationHost&, f32 dt)
{
    // Hold-to-repeat: tick the RepeatButton each frame (mirrors Sedulous UISandbox).
    if (m_repeatBtn)
    {
        m_repeatBtn->UpdateRepeat(dt);
    }
    if (m_uiHost)
    {
        m_uiHost->Update(dt);
    }
    if (m_toastHost)
    {
        m_toastHost->Update(dt);
    }
    // Drag-follow: move a dragged floating dock window to track the desktop cursor.
    if (m_dockHost)
    {
        m_dockHost->Tick();
    }

    // Viewport: spin the cube + drive its camera from the gated surface. SyncInputRegion runs AFTER the
    // UI laid out this frame (uiHost->Update) so the surface tracks the view's current on-screen rect; the
    // camera reads the gated devices only when the UI says the viewport is genuinely hovered/focused (so
    // occluded/inactive-tab input can't leak, and a look-drag off the view keeps going while focused).
    m_time += dt;
    if (m_viewport && m_vpRouter)
    {
        UpdateViewportHostWindow(); // re-bind if the dockable panel was floated into / out of an OS window
        m_viewport->SyncInputRegion();
        m_vpRouter->Update();
        if (m_viewport->IsHovered() || m_viewport->IsFocused())
        {
            m_cam.Update(m_viewport->Keyboard(), m_viewport->Mouse(), dt);
        }
    }
}

void UISandbox::OnRenderWindow(runtime::IApplicationHost&, graphics::FrameContext& frame)
{
    // Render the 3D viewport content into its offscreen target BEFORE the UI draws (the UI samples that
    // target as an image). On whichever window currently hosts the viewport (main, or a floated OS window).
    if (m_viewport && m_viewport->IsReady() && frame.valid && frame.window == m_viewportWindow)
    {
        m_viewport->RenderContent(*frame.encoder, static_cast<i32>(frame.frameIndex));
    }
    if (m_uiHost)
    {
        m_uiHost->RenderWindow(frame);
    }
}

void UISandbox::OnShutdown(runtime::IApplicationHost&)
{
    // Release the viewport's GPU targets + external-texture registration while the device and the
    // per-window VGRenderer are still alive (the ViewportView outlives the window inside the view tree).
    if (m_viewport)
    {
        m_viewport->Shutdown();
    }
    m_cube.Shutdown();
    if (m_cubeCompiler != nullptr)
    {
        m_cubeCompiler->Destroy();
        m_cubeCompiler = nullptr;
    }
}

int main(int argc, char** argv)
{
    shell::WindowSettings ws;
    ws.title = u8"UI Sandbox (draconic.ui)";
    ws.width = 820;
    ws.height = 720;

    auto shellPtr = shell::CreateShell(ws);
    if (shellPtr.Get() == nullptr || shellPtr->MainWindow() == nullptr)
    {
        return 1;
    }

    graphics::GraphicsDeviceDesc gdd;
    gdd.backend = graphics::SelectBackendFromArguments(argc, argv);
    gdd.enableValidation = true;
    auto gpu = graphics::CreateGraphicsDevice(gdd);
    if (!gpu.HasValue())
    {
        return 1;
    }

    UISandbox app;
    return runtime::RunApplication(app, *shellPtr, gpu.Value().Get());
}
