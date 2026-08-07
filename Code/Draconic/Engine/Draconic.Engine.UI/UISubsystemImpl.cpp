// Draconic::UISubsystem - implementation unit: VG/shader/render contact, the input pump,
// canvas syncing, and the component reflection bodies (GCC hygiene: none of this may sit
// in the interface's global fragment / partitions - see physics for the precedent).

module;
#define _CRT_SECURE_NO_WARNINGS
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

module draconic.engine.ui;

import draconic.foundation;
import draconic.runtime;
import draconic.scene;
import draconic.engine.scene;
import draconic.resource;
import draconic.shell;
import draconic.rhi;
import draconic.fonts;
import draconic.fonts.ttf;
import draconic.fonts.resource;
import draconic.input;
import draconic.engine.input;
import draconic.shaders;
import draconic.shaders.system; // ShaderSystemHost (cooked-pack-or-dev shader resolution)
import draconic.vg;
import draconic.vg.renderer;
import draconic.ui;
import draconic.ui.resource;
import draconic.render.api;
import draconic.engine.render; // RenderSubsystem (overlay-role registration)
import draconic.script;         // the Ui facade reflection body
import draconic.script.facades; // RegisterExtraFacadeName (behavior-prelude hook)

using namespace draconic::foundation;
namespace vg = draconic::vg;

namespace draconic::ui
{
    // The Ui.* facade reflection body + registration (kept out of the interface unit per the GCC
    // gcm-cluster rule). Owned by the UISubsystem (the out-of-tree facade pattern, like Net).
    DRACONIC_REFLECT(Ui, "draconic::ui")
    {
        builder.Method<&Ui::pushOverlay>("pushOverlay");
        builder.Method<&Ui::popOverlay>("popOverlay");
        builder.Method<&Ui::setText>("setText");
        builder.Method<&Ui::setProgress>("setProgress");
        builder.Method<&Ui::setVisible>("setVisible");
        builder.Method<&Ui::onClick>("onClick");
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    void RegisterUiScriptFacade()
    {
        static const bool once = []()
        {
            RegisterUIComponentReflection(); // build component TypeData (incl `of`) first
            GlobalTypeRegistry().Register(Ui::StaticType());
            draconic::script::RegisterExtraFacadeName(
                u8"Ui"); // Wren behavior prelude imports it (AngelScript binds by registry)

            // The WORLD-space UI components -> script .of (live data: order/visible/interactive/
            // orientation/size/...). The app SCREEN tier (IScreenOverlay, loading screen) is NOT
            // exposed as a component - it stays behind the app-global Ui facade.
            struct Entry
            {
                const TypeInfo* type;
                StringView name;
            };
            const Entry components[] = {
                {&TypeOf<UICanvasComponent>(), u8"UICanvasComponent"},
                {&TypeOf<UIBillboardComponent>(), u8"UIBillboardComponent"},
                {&TypeOf<UIWorldPanelComponent>(), u8"UIWorldPanelComponent"}};
            for (const Entry& component : components)
            {
                GlobalTypeRegistry().Register(*component.type);
                draconic::script::RegisterExtraScriptRootType(component.type);
                draconic::script::RegisterExtraFacadeName(component.name);
            }
            return true;
        }();
        (void)once;
    }

    // ---- UiScriptHost: backs the Ui.* facade with the live UISubsystem screen tier ----

    void UiScriptHost::Install(UiScriptBinding& binding)
    {
        UiScriptHost* self = this;
        binding.pushOverlay =
            Function<i32(const foundation::Guid&)>{[self](const foundation::Guid& d) { return self->PushOverlay(d); }};
        binding.popOverlay = Function<void(i32)>{[self](i32 h) { self->PopOverlay(h); }};
        binding.setText = Function<void(i32, StringView, StringView)>{
            [self](i32 h, StringView id, StringView t) { self->SetText(h, id, t); }};
        binding.setProgress = Function<void(i32, StringView, f64)>{
            [self](i32 h, StringView id, f64 v) { self->SetProgress(h, id, v); }};
        binding.setVisible = Function<void(i32, StringView, bool)>{
            [self](i32 h, StringView id, bool v) { self->SetVisible(h, id, v); }};
        binding.onClick = Function<void(i32, StringView, RefPtr<script::IScriptDelegate>)>{
            [self](i32 h, StringView id, RefPtr<script::IScriptDelegate> fn)
            { self->OnClick(h, id, Move(fn)); }};
    }

    i32 UiScriptHost::PushOverlay(const foundation::Guid& document)
    {
        if (m_ui == nullptr || !m_resolve)
        {
            return 0;
        }
        RefPtr<UIDocument> doc = m_resolve(document);
        if (!doc)
        {
            return 0;
        }
        // Instantiate + register the handle SYNCHRONOUSLY (the handle must be returned, and the
        // setters address this view immediately), but DEFER the tree ATTACH through the mutation
        // queue: a script may push from inside an event handler, and adding to the overlay layer
        // mid-dispatch violates the UI mutation-queue rule.
        RefPtr<View> view = m_ui->InstantiateScreenOverlay(*doc);
        if (!view)
        {
            return 0;
        }
        const i32 handle = ++m_next;
        m_overlays.InsertOrAssign(handle, view);
        UISubsystem* ui = m_ui;
        m_ui->Context().MutationQueueRef().QueueAction([ui, view]() { ui->PushScreenOverlay(view); });
        return handle;
    }

    View* UiScriptHost::FindOverlay(i32 handle) const
    {
        const RefPtr<View>* v = m_overlays.Find(handle);
        return v != nullptr ? v->Get() : nullptr;
    }

    View* UiScriptHost::FindControl(i32 handle, StringView id) const
    {
        View* overlay = FindOverlay(handle);
        if (overlay == nullptr)
        {
            return nullptr;
        }
        if (overlay->Name.Size() > 0 && overlay->Name.AsView() == id)
        {
            return overlay; // the overlay root itself carries the id
        }
        ViewGroup* group = Cast<ViewGroup>(overlay);
        return group != nullptr ? group->FindByName(id) : nullptr;
    }

    void UiScriptHost::PopOverlay(i32 handle)
    {
        RefPtr<View>* slot = m_overlays.Find(handle);
        if (slot == nullptr)
        {
            return; // unknown / already-popped handle: no-op
        }
        RefPtr<View> view = *slot; // hold it alive for the deferred detach
        m_overlays.Remove(handle); // the handle dies synchronously; repeat pops no-op
        if (m_ui == nullptr)
        {
            return;
        }
        // DEFER the tree DETACH: a script's standard "close menu" button pops its own overlay from
        // inside the onClick handler; destroying the view tree mid-dispatch is the mutation-queue
        // rule. The queued action holds `view`, so it survives until the drain.
        UISubsystem* ui = m_ui;
        m_ui->Context().MutationQueueRef().QueueAction(
            [ui, view]() { ui->RemoveScreenOverlay(view.Get()); });
    }

    void UiScriptHost::SetText(i32 handle, StringView id, StringView text)
    {
        View* control = FindControl(handle, id);
        if (Label* label = Cast<Label>(control))
        {
            label->SetText(text);
        }
        else if (Button* button = Cast<Button>(control))
        {
            button->SetText(text);
        }
    }

    void UiScriptHost::SetProgress(i32 handle, StringView id, f64 value)
    {
        if (ProgressBar* bar = Cast<ProgressBar>(FindControl(handle, id)))
        {
            const f32 clamped =
                value < 0.0 ? 0.0f : (value > 1.0 ? 1.0f : static_cast<f32>(value));
            bar->Value.SetValue(clamped);
        }
    }

    void UiScriptHost::SetVisible(i32 handle, StringView id, bool visible)
    {
        if (View* v = FindControl(handle, id))
        {
            v->Visibility = visible ? Visibility::Visible : Visibility::Gone;
        }
    }

    void UiScriptHost::OnClick(i32 handle, StringView id, RefPtr<script::IScriptDelegate> fn)
    {
        if (!fn)
        {
            return;
        }
        if (ButtonBase* button = Cast<ButtonBase>(FindControl(handle, id)))
        {
            RefPtr<script::IScriptDelegate> held = Move(fn);
            // A click carries no payload - fire with no args. The handler is a natural void()
            // (Wren `Fn.new { ... }`, AngelScript `Action(@onCancel)`); Invoke marshals against its
            // actual arity, so a handler that does take args just gets none. Holding `held` keeps
            // the script fn GC-alive.
            button->OnClick.Add([held](ButtonBase*) { (void)held->Invoke(Span<Variant>{}); });
        }
    }

    // Per-canvas host inside a scene root: carries the canvas's draw ORDER (the scene
    // root's canvas children are kept sorted by it - higher = later = on top; the
    // billboard layer stays child 0, below every canvas) and the SCALER transform
    // (ReferenceResolution lays the document out at the reference size and uniform-
    // scales it to fit, centered; ConstantPixel = 1 UI px = 1 target px). Hit-test
    // transparent: only the document tree consumes input, and the child's scale
    // transform is undone by the standard ViewGroup inverse-transform hit test.
    class CanvasHostView final : public ViewGroup
    {
        DRACONIC_OBJECT(CanvasHostView, ViewGroup)
    public:
        i32 Order = 0;
        bool Seen = false; // swept by SyncCanvases when the component vanished
        CanvasScalerMode ScalerMode = CanvasScalerMode::ConstantPixel;
        Float2 ReferenceResolution{0.0f, 0.0f};

        CanvasHostView() { IsHitTestVisible = false; }

        /// The size the document lays out at: the reference resolution when scaling,
        /// else the host's own (viewport) size.
        [[nodiscard]] Float2 LayoutSizeFor(f32 width, f32 height) const noexcept
        {
            if (ScalerMode == CanvasScalerMode::ReferenceResolution &&
                ReferenceResolution.x > 0.0f && ReferenceResolution.y > 0.0f)
            {
                return ReferenceResolution;
            }
            return Float2{width, height};
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(constraints.MaxHeight)};
            const Float2 inner = LayoutSizeFor(MeasuredSize.x, MeasuredSize.y);
            const BoxConstraints childConstraints = BoxConstraints::Tight(inner.x, inner.y);
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility != VisibilityValue::Gone)
                {
                    child->Measure(childConstraints);
                }
            }
        }

        void OnLayout(f32 /*left*/, f32 /*top*/, f32 width, f32 height) override
        {
            const Float2 inner = LayoutSizeFor(width, height);
            f32 scale = 1.0f;
            f32 offsetX = 0.0f;
            f32 offsetY = 0.0f;
            if (inner.x != width || inner.y != height)
            {
                // The standard rule: uniform min-fit, letterboxed and centered.
                scale = Min(width / inner.x, height / inner.y);
                offsetX = (width - inner.x * scale) * 0.5f;
                offsetY = (height - inner.y * scale) * 0.5f;
            }
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == VisibilityValue::Gone)
                {
                    continue;
                }
                child->Layout(offsetX, offsetY, inner.x, inner.y);
                // Scale around the top-left so drawn rect = offset + scale * content;
                // hit testing undoes the same transform (ViewGroup::HitTest).
                child->Transform.Scale = Float2{scale, scale};
                child->Transform.Origin = Float2{0.0f, 0.0f};
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(CanvasHostView, "draconic::ui")

    // Keep a scene root's canvas hosts sorted by Order, STABLE for ties (the child
    // sequence is component/insertion order between re-sorts). MoveView is a pure
    // reorder (no detach), so focus/hover survive an order change; targets start at
    // child 1 (the billboard layer stays 0) and RootView keeps its popup layer last.
    void SortCanvasHostsByOrder(RootView& root)
    {
        Array<CanvasHostView*> hosts;
        for (usize i = 0; i < root.ChildCount(); ++i)
        {
            if (auto* host = Cast<CanvasHostView>(root.GetChildAt(i)))
            {
                hosts.PushBack(host);
            }
        }
        for (usize i = 1; i < hosts.Size(); ++i) // stable insertion sort
        {
            CanvasHostView* key = hosts[i];
            usize j = i;
            while (j > 0 && hosts[j - 1]->Order > key->Order)
            {
                hosts[j] = hosts[j - 1];
                --j;
            }
            hosts[j] = key;
        }
        for (usize i = 0; i < hosts.Size(); ++i)
        {
            root.MoveView(hosts[i], 1 + i);
        }
    }

    // Per-target-format VG pipeline (backbuffer vs viewport formats differ).
    struct UISubsystem::RenderState
    {
        draconic::shaders::ShaderSystemHost shaderHost; // owns the ShaderSystem + VG modules
        rhi::Device* device = nullptr;
        rhi::ShaderModule* vertexShader = nullptr;       // borrowed from shaderHost
        rhi::ShaderModule* fragmentShader = nullptr;     // borrowed from shaderHost
        rhi::ShaderModule* gradRadialShader = nullptr;   // per-pixel radial gradient (borrowed)
        rhi::ShaderModule* dfShader = nullptr;           // MSDF text fragment (borrowed)
        rhi::ShaderModule* gradConicShader = nullptr;    // per-pixel conic gradient (borrowed)
        vg::VGContext vgContext;
        struct FormatRenderer
        {
            rhi::TextureFormat format = rhi::TextureFormat::RGBA8Unorm;
            bool stencil = false; // stencil-configured variant (canvas + overlay passes
                                  // carry a DS attachment; pipelines must match their pass)
            u32 sampleCount = 1;  // pipeline multisample state (MSAA canvas passes)
            UniquePtr<vg::renderer::VGRenderer> renderer;
            u64 begunSerial = 0; // last UI frame this renderer's ring was reset for
        };
        Array<FormatRenderer> renderers;
        i32 frameCount = 2;

        // RenderTexture canvases: one subsystem-owned offscreen target per RT canvas,
        // keyed by (scene, entity). Reconciled by RenderCanvasTextures each call.
        struct CanvasTarget
        {
            scene::Scene* scene = nullptr;
            scene::EntityHandle entity{};
            rhi::Texture* texture = nullptr;
            rhi::TextureView* view = nullptr;
            // Stencil attachment for stencil-then-cover fills in canvas UI (null when the
            // device offers no stencil format - the canvas falls back to tessellated fills).
            rhi::Texture* depthStencil = nullptr;
            rhi::TextureView* depthStencilView = nullptr;
            // MSAA color the canvas pass renders into, resolved into `texture` (the
            // sampled resolve target). Null = single-sampled fallback (creation failed).
            rhi::Texture* msaa = nullptr;
            rhi::TextureView* msaaView = nullptr;
            u32 sampleCount = 1;
            u32 width = 0;
            u32 height = 0;
            rhi::ResourceState state = rhi::ResourceState::Undefined;
            rhi::ResourceState msaaState = rhi::ResourceState::Undefined;
            bool seen = false;
        };
        Array<CanvasTarget> canvasTargets;
        // Probed once in EnsureRenderReady; Undefined = device offers no stencil format.
        rhi::TextureFormat canvasStencilFormat = rhi::TextureFormat::Undefined;
        // Canvas RTT MSAA factor (mirrors UIWindowData::kMsaaSamples on the window host;
        // 4x is universally supported for 8-bit color on Vulkan and guaranteed by WebGPU).
        static constexpr u32 kCanvasMsaaSamples = 4;

        explicit RenderState(draconic::fonts::IFontService* fonts) : vgContext(fonts) {}

        ~RenderState()
        {
            if (!canvasTargets.IsEmpty() && device != nullptr)
            {
                device->WaitIdle();
            }
            for (usize i = canvasTargets.Size(); i-- > 0;)
            {
                DestroyCanvasTarget(i, false);
            }
            renderers.Clear();
            // vertexShader/fragmentShader are BORROWED from shaderHost's ShaderSystem, which owns
            // and frees them (and the compiler) when it is destroyed with this struct.
        }

        // The (created-on-demand) target for one RT canvas, recreated on resize. Null
        // only when texture creation fails.
        [[nodiscard]] CanvasTarget* EnsureCanvasTarget(scene::Scene* scene,
                                                       scene::EntityHandle entity, u32 width,
                                                       u32 height, rhi::TextureFormat format)
        {
            CanvasTarget* found = nullptr;
            for (auto& target : canvasTargets)
            {
                if (target.scene == scene && target.entity == entity)
                {
                    found = &target;
                    break;
                }
            }
            if (found != nullptr && (found->width != width || found->height != height))
            {
                // Resize: the GPU may still sample the old target - idle before freeing
                // (the ViewportView resize precedent; RT resizes are rare, author-driven).
                device->WaitIdle();
                if (found->view != nullptr)
                {
                    device->DestroyTextureView(found->view);
                }
                if (found->texture != nullptr)
                {
                    device->DestroyTexture(found->texture);
                }
                if (found->depthStencilView != nullptr)
                {
                    device->DestroyTextureView(found->depthStencilView);
                }
                if (found->depthStencil != nullptr)
                {
                    device->DestroyTexture(found->depthStencil);
                }
                if (found->msaaView != nullptr)
                {
                    device->DestroyTextureView(found->msaaView);
                }
                if (found->msaa != nullptr)
                {
                    device->DestroyTexture(found->msaa);
                }
                found->texture = nullptr;
                found->view = nullptr;
                found->depthStencil = nullptr;
                found->depthStencilView = nullptr;
                found->msaa = nullptr;
                found->msaaView = nullptr;
                found->sampleCount = 1;
                found->msaaState = rhi::ResourceState::Undefined;
            }
            if (found == nullptr)
            {
                CanvasTarget target;
                target.scene = scene;
                target.entity = entity;
                canvasTargets.PushBack(target);
                found = &canvasTargets[canvasTargets.Size() - 1];
            }
            if (found->texture == nullptr)
            {
                rhi::TextureDesc desc =
                    rhi::TextureDesc::RenderTarget(format, width, height, 1, u8"UICanvasTexture");
                if (!device->CreateTexture(desc, found->texture).IsOk())
                {
                    found->texture = nullptr;
                    return nullptr;
                }
                rhi::TextureViewDesc viewDesc{};
                viewDesc.format = format;
                if (!device->CreateTextureView(found->texture, viewDesc, found->view).IsOk())
                {
                    device->DestroyTexture(found->texture);
                    found->texture = nullptr;
                    found->view = nullptr;
                    return nullptr;
                }
                // MSAA color (the UIRuntime window-host treatment for canvases): render
                // into a 4x target, resolve into `texture`. Best-effort - any failure
                // falls back to single-sampled rendering straight into `texture`.
                found->sampleCount = 1;
                found->msaaState = rhi::ResourceState::Undefined;
                {
                    rhi::TextureDesc msDesc = rhi::TextureDesc::RenderTarget(
                        format, width, height, kCanvasMsaaSamples, u8"UICanvasMsaa");
                    msDesc.usage = rhi::TextureUsage::RenderTarget; // never sampled
                    if (device->CreateTexture(msDesc, found->msaa).IsOk())
                    {
                        if (device->CreateTextureView(found->msaa, rhi::TextureViewDesc{},
                                                      found->msaaView)
                                .IsOk())
                        {
                            found->sampleCount = kCanvasMsaaSamples;
                        }
                        else
                        {
                            device->DestroyTexture(found->msaa);
                            found->msaa = nullptr;
                            found->msaaView = nullptr;
                        }
                    }
                    else
                    {
                        found->msaa = nullptr;
                    }
                }
                // Stencil twin (same size AND sample count as the pass's color
                // attachment; skipped when no stencil format resolved).
                if (canvasStencilFormat != rhi::TextureFormat::Undefined)
                {
                    rhi::TextureDesc dsDesc{};
                    dsDesc.dimension = rhi::TextureDimension::Texture2D;
                    dsDesc.format = canvasStencilFormat;
                    dsDesc.width = width;
                    dsDesc.height = height;
                    dsDesc.depth = 1;
                    dsDesc.usage = rhi::TextureUsage::DepthStencil;
                    dsDesc.sampleCount = found->sampleCount;
                    dsDesc.label = u8"UICanvasStencil";
                    if (device->CreateTexture(dsDesc, found->depthStencil).IsOk())
                    {
                        if (!device
                                 ->CreateTextureView(found->depthStencil, rhi::TextureViewDesc{},
                                                     found->depthStencilView)
                                 .IsOk())
                        {
                            device->DestroyTexture(found->depthStencil);
                            found->depthStencil = nullptr;
                            found->depthStencilView = nullptr;
                        }
                    }
                    else
                    {
                        found->depthStencil = nullptr;
                    }
                }
                found->width = width;
                found->height = height;
                found->state = rhi::ResourceState::Undefined;
            }
            return found;
        }

        void DestroyCanvasTarget(usize index, bool waitIdle = true)
        {
            CanvasTarget& target = canvasTargets[index];
            if (device != nullptr)
            {
                if (waitIdle && (target.texture != nullptr || target.view != nullptr))
                {
                    device->WaitIdle(); // the scene may still be sampling it
                }
                if (target.view != nullptr)
                {
                    device->DestroyTextureView(target.view);
                }
                if (target.texture != nullptr)
                {
                    device->DestroyTexture(target.texture);
                }
                if (target.depthStencilView != nullptr)
                {
                    device->DestroyTextureView(target.depthStencilView);
                }
                if (target.depthStencil != nullptr)
                {
                    device->DestroyTexture(target.depthStencil);
                }
                if (target.msaaView != nullptr)
                {
                    device->DestroyTextureView(target.msaaView);
                }
                if (target.msaa != nullptr)
                {
                    device->DestroyTexture(target.msaa);
                }
            }
            canvasTargets.RemoveAt(index);
        }

        // Fetch (or lazily create) the renderer for a target format, with its per-frame
        // ring reset EXACTLY once per UI frame (`frameSerial`): BeginFrame rewinds the
        // vertex/uniform rings and clears the command list, so calling it before every
        // draw would clobber the slices of draws recorded earlier in the SAME frame
        // (scene overlay + screen overlay + preview all share a format's renderer now).
        [[nodiscard]] vg::renderer::VGRenderer* RendererFor(rhi::TextureFormat format,
                                                            u64 frameSerial, i32 frameIndex,
                                                            bool stencil = false,
                                                            u32 sampleCount = 1)
        {
            FormatRenderer* found = nullptr;
            for (auto& entry : renderers)
            {
                if (entry.format == format && entry.stencil == stencil &&
                    entry.sampleCount == sampleCount)
                {
                    found = &entry;
                    break;
                }
            }
            if (found == nullptr)
            {
                if (vertexShader == nullptr || fragmentShader == nullptr)
                {
                    return nullptr;
                }
                vg::renderer::VGTargetConfig targetConfig;
                targetConfig.sampleCount = sampleCount;
                if (stencil)
                {
                    targetConfig.depthStencilFormat = canvasStencilFormat;
                }
                auto renderer = MakeUnique<vg::renderer::VGRenderer>(DefaultAllocator());
                if (!renderer
                         ->Initialize(*device, *vertexShader, *fragmentShader, format, frameCount,
                                      dfShader, gradRadialShader, gradConicShader, targetConfig)
                         .IsOk())
                {
                    return nullptr;
                }
                FormatRenderer entry;
                entry.format = format;
                entry.stencil = stencil;
                entry.sampleCount = sampleCount;
                entry.renderer = Move(renderer);
                renderers.PushBack(Move(entry));
                found = &renderers[renderers.Size() - 1];
            }
            if (found->begunSerial != frameSerial)
            {
                found->renderer->BeginFrame(frameIndex);
                found->begunSerial = frameSerial;
            }
            return found->renderer.Get();
        }
    };

    UISubsystem::UISubsystem() = default;
    UISubsystem::~UISubsystem() = default;

    void UISubsystem::OnInit()
    {
        MarkupLoader::Initialize();
        m_fonts = MakeUnique<draconic::fonts::TrueTypeFontService>(DefaultAllocator());
        StringView fontPath = m_fontPath.AsView();
        if (fontPath.IsEmpty())
        {
            fontPath = u8"Data/Assets/fonts/roboto/Roboto-Regular.ttf"; // dev-tree default
        }
        if (m_fonts->LoadFont(u8"Roboto", fontPath) == draconic::fonts::FontLoadResult::Success)
        {
            m_fonts->SetDefaultFamily(u8"Roboto");
        }
        else
        {
            DRACONIC_LOG_WARNING(
                u8"UI", u8"default font '{}' not loaded - game UI text will not render", fontPath);
        }
        m_context.SetFontService(m_fonts.Get());
        m_theme = GameTheme::Create();
        m_context.SetStyleSheet(m_theme);
        // The scene-LESS screen tier: the screen root holds ONLY the global overlay
        // layer (each scene's canvases + billboards live in that scene's own root). It
        // only hit-tests while it HOLDS overlays - an empty full-screen layer must never
        // swallow the clicks meant for the scene canvases below it.
        m_screenRoot = MakeRef<RootView>(DefaultAllocator());
        m_context.AddRootView(m_screenRoot.Get());
        auto overlay = MakeRef<FrameLayout>(DefaultAllocator());
        overlay->IsHitTestVisible = false;
        m_overlayLayer = overlay;
        m_screenRoot->AddView(m_overlayLayer.Get());
    }

    void UISubsystem::OnReady()
    {
        if (draconic::runtime::Context* context = GetContext())
        {
            m_input = context->GetSubsystem<draconic::input::InputSubsystem>();
            if (auto* scenes = context->GetSubsystem<scene::SceneSubsystem>())
            {
                scenes->RegisterSceneAware(this); // injects the canvas manager per scene
            }
            // The overlay roles: scene tier draws inside the compose per view; screen
            // tier draws when the host calls RenderOverlays per window target. Headless
            // contexts (tests, cooker) have no render subsystem - both stay unregistered.
            if (auto* render = context->GetSubsystem<draconic::render::RenderSubsystem>())
            {
                m_sceneRenderer = render;
                m_screenRenderer = render;
                m_sceneRenderer->RegisterOverlay(
                    static_cast<draconic::render::ISceneOverlay*>(this));
                m_screenRenderer->RegisterOverlay(
                    static_cast<draconic::render::IScreenOverlay*>(this));
            }
        }
    }

    void UISubsystem::OnShutdown()
    {
        if (m_sceneRenderer != nullptr)
        {
            m_sceneRenderer->UnregisterOverlay(static_cast<draconic::render::ISceneOverlay*>(this));
            m_sceneRenderer = nullptr;
        }
        if (m_screenRenderer != nullptr)
        {
            m_screenRenderer->UnregisterOverlay(
                static_cast<draconic::render::IScreenOverlay*>(this));
            m_screenRenderer = nullptr;
        }
        if (draconic::runtime::Context* context = GetContext())
        {
            if (auto* scenes = context->GetSubsystem<scene::SceneSubsystem>())
            {
                scenes->UnregisterSceneAware(this);
            }
        }
        for (SceneUI& ui : m_sceneUIs)
        {
            if (ui.root.Get() != nullptr)
            {
                m_context.RemoveRootView(ui.root.Get());
            }
        }
        m_sceneUIs.Clear();
        for (TextureCanvasRoot& entry : m_textureCanvasRoots)
        {
            m_context.RemoveRootView(entry.root.Get());
        }
        m_textureCanvasRoots.Clear();
        m_overlayLayer = nullptr;
        if (m_screenRoot.Get() != nullptr)
        {
            m_context.RemoveRootView(m_screenRoot.Get());
        }
        m_screenRoot = nullptr;
        m_render = nullptr;
        m_theme = nullptr;
    }

    void UISubsystem::BeginFrame(f32 deltaTime)
    {
        // UNSCALED time: menus animate while the game is paused (BeginFrame receives the
        // raw host dt - the whole reason this runs here and not in Update).
        ++m_frameSerial; // one VG ring reset per UI frame (see RenderState::RendererFor)
        m_context.BeginFrame(deltaTime);
        m_navDeltaTime = deltaTime;
        SyncCanvases();
        PumpInput();
    }

    void UISubsystem::SyncCanvases()
    {
        // Instantiate/rebuild each canvas's view tree from its cooked document. Documents
        // are TEMPLATES: a fresh tree per canvas; a document reload (different product
        // pointer) rebuilds; theme overrides parse per canvas as a LOCAL stylesheet.
        // Canvases and billboards parent into THEIR SCENE's root (the scene tier) - a
        // Simulate page's UI can never bleed into the Game tab by construction.
        for (TextureCanvasRoot& entry : m_textureCanvasRoots)
        {
            entry.seen = false;
        }
        for (SceneUI& sceneUI : m_sceneUIs)
        {
            scene::Scene* scene = sceneUI.scene;
            auto* canvases = scene->GetSystem<UICanvasComponentManager>();
            if (canvases == nullptr)
            {
                continue;
            }
            // Mark: hosts whose component vanished this frame get swept after the walk
            // (component managers have no destroy hook - despawning a menu entity must
            // still remove its tree from the scene root).
            for (usize i = 0; i < sceneUI.root->ChildCount(); ++i)
            {
                if (auto* host = Cast<CanvasHostView>(sceneUI.root->GetChildAt(i)))
                {
                    host->Seen = false;
                }
            }
            canvases->ForEach(
                [&](UICanvasComponent& c, scene::EntityHandle)
                {
                    const UIDocument* document = c.document.Get();
                    const bool wantsTexture = c.renderMode == CanvasRenderMode::RenderTexture;
                    const bool builtAsTexture = c.renderRoot.Get() != nullptr;
                    if (document != c.builtFrom ||
                        (c.root.Get() != nullptr && wantsTexture != builtAsTexture))
                    {
                        // Tear down whichever shape was built (document reload or a render-
                        // mode flip), then instantiate for the CURRENT mode.
                        if (c.root.Get() != nullptr)
                        {
                            if (c.host.Get() != nullptr)
                            {
                                c.host->RemoveView(c.root.Get());
                            }
                            if (c.renderRoot.Get() != nullptr)
                            {
                                c.renderRoot->RemoveView(c.root.Get());
                            }
                            c.root = nullptr;
                        }
                        if (c.renderRoot.Get() != nullptr)
                        {
                            m_context.RemoveRootView(c.renderRoot.Get());
                            c.renderRoot = nullptr;
                            c.renderTexture = nullptr;     // GPU objects swept by the next
                            c.renderTextureView = nullptr; // RenderCanvasTextures
                        }
                        if (document != nullptr && !document->markup.IsEmpty())
                        {
                            c.root =
                                MarkupLoader::LoadFromString(document->markup.AsView(), &m_context);
                            if (c.root.Get() == nullptr)
                            {
                                DRACONIC_LOG_WARNING(u8"UI",
                                                     u8"canvas document failed to instantiate");
                            }
                            else if (wantsTexture)
                            {
                                // RenderTexture: a STANDALONE root - never parented into a
                                // tier (not drawn by the overlay roles) and never an input
                                // root (v1 RT canvases are non-interactive).
                                c.renderRoot = MakeRef<RootView>(DefaultAllocator());
                                c.renderRoot->AddView(c.root.Get());
                                m_context.AddRootView(c.renderRoot.Get());
                                TextureCanvasRoot entry;
                                entry.root = c.renderRoot;
                                m_textureCanvasRoots.PushBack(Move(entry));
                            }
                        }
                        c.builtFrom = document;
                    }
                    if (c.renderRoot.Get() != nullptr)
                    {
                        for (TextureCanvasRoot& entry : m_textureCanvasRoots)
                        {
                            if (entry.root.Get() == c.renderRoot.Get())
                            {
                                entry.seen = true;
                                break;
                            }
                        }
                    }
                    if (!wantsTexture)
                    {
                        // Overlay canvases parent through their host: the order/scaler
                        // carrier in the scene root. (An unseen host - RT mode or a dead
                        // component - is swept below.)
                        if (c.host.Get() == nullptr)
                        {
                            c.host = MakeRef<CanvasHostView>(DefaultAllocator());
                            sceneUI.root->AddView(c.host.Get());
                        }
                        auto* host = static_cast<CanvasHostView*>(c.host.Get());
                        host->Seen = true;
                        host->Order = c.order;
                        host->ScalerMode = c.scalerMode;
                        host->ReferenceResolution = c.referenceResolution;
                        if (c.root.Get() != nullptr && c.root->Parent == nullptr)
                        {
                            host->AddView(c.root.Get());
                        }
                    }
                    else if (c.host.Get() != nullptr)
                    {
                        c.host = nullptr; // the stale host stays unseen -> swept below
                    }
                    const UITheme* theme = c.theme.Get();
                    if (theme != c.themeFrom)
                    {
                        c.themeSheet = nullptr;
                        if (theme != nullptr && !theme->stylesheet.IsEmpty())
                        {
                            StyleSheetLoader loader;
                            loader.SetPalette(GameTheme::Palette());
                            c.themeSheet = loader.Load(theme->stylesheet.AsView());
                        }
                        if (c.root.Get() != nullptr)
                        {
                            c.root->SetLocalStyleSheet(c.themeSheet);
                        }
                        c.themeFrom = theme;
                    }
                    if (c.root.Get() != nullptr)
                    {
                        c.root->Visibility =
                            c.visible ? VisibilityValue::Visible : VisibilityValue::Gone;
                        // Interactivity gates the SUBTREE; the stretched document root
                        // itself stays hit-TRANSPARENT. The canvas root is the canvas's
                        // screen AREA, not a widget - if it consumed hits, one full-screen
                        // HUD would swallow the pointer everywhere (eating world panels
                        // and gameplay clicks alike). Content that wants a clickable
                        // backdrop uses an explicit full-size child.
                        c.root->IsInteractionEnabled = c.interactive;
                        c.root->IsHitTestVisible = false;
                    }
                });
            // Sweep hosts orphaned by component/entity destruction, then keep the
            // canvases stacked by their authored order (billboard layer always below).
            for (usize i = sceneUI.root->ChildCount(); i-- > 0;)
            {
                auto* host = Cast<CanvasHostView>(sceneUI.root->GetChildAt(i));
                if (host != nullptr && !host->Seen)
                {
                    sceneUI.root->RemoveView(host);
                }
            }
            SortCanvasHostsByOrder(*sceneUI.root);

            auto* billboards = scene->GetSystem<UIBillboardComponentManager>();
            if (billboards == nullptr)
            {
                continue;
            }
            Array<View*> liveBillboards; // the sweep below removes everything else
            billboards->ForEach(
                [&](UIBillboardComponent& c, scene::EntityHandle)
                {
                    const UIDocument* document = c.document.Get();
                    if (document != c.builtFrom)
                    {
                        if (c.root.Get() != nullptr)
                        {
                            sceneUI.billboardLayer->RemoveView(c.root.Get());
                            c.root = nullptr;
                        }
                        if (document != nullptr && !document->markup.IsEmpty())
                        {
                            c.root =
                                MarkupLoader::LoadFromString(document->markup.AsView(), &m_context);
                            if (c.root.Get() != nullptr)
                            {
                                auto lp = MakeRef<AbsoluteLayoutParams>(DefaultAllocator());
                                c.root->LayoutParams = lp;
                                sceneUI.billboardLayer->AddView(c.root.Get());
                            }
                        }
                        c.builtFrom = document;
                    }
                    if (c.root.Get() != nullptr)
                    {
                        liveBillboards.PushBack(c.root.Get());
                    }
                });
            // Sweep nameplates whose component/entity vanished (the canvas hosts'
            // sweep, applied to the billboard layer - managers have no destroy hook).
            for (usize i = sceneUI.billboardLayer->ChildCount(); i-- > 0;)
            {
                View* child = sceneUI.billboardLayer->GetChildAt(i);
                bool live = false;
                for (View* root : liveBillboards)
                {
                    if (root == child)
                    {
                        live = true;
                        break;
                    }
                }
                if (!live)
                {
                    sceneUI.billboardLayer->RemoveView(child);
                }
            }

            // World panels (the world tier): standalone roots like RT canvases - never
            // parented into a tier, registered through the SAME texture-root registry
            // (its mark-sweep handles despawn); drawn + sprite-driven by
            // RenderCanvasTextures; the pump ray-routes the pointer into them.
            if (auto* panels = scene->GetSystem<UIWorldPanelComponentManager>())
            {
                panels->ForEach(
                    [&](UIWorldPanelComponent& c, scene::EntityHandle)
                    {
                        const UIDocument* document = c.document.Get();
                        if (document != c.builtFrom)
                        {
                            if (c.renderRoot.Get() != nullptr)
                            {
                                m_context.RemoveRootView(c.renderRoot.Get());
                                c.renderRoot = nullptr;
                                c.root = nullptr;
                                c.renderTexture = nullptr; // swept by RenderCanvasTextures
                                c.renderTextureView = nullptr;
                            }
                            if (document != nullptr && !document->markup.IsEmpty())
                            {
                                c.root = MarkupLoader::LoadFromString(document->markup.AsView(),
                                                                      &m_context);
                                if (c.root.Get() != nullptr)
                                {
                                    c.renderRoot = MakeRef<RootView>(DefaultAllocator());
                                    c.renderRoot->AddView(c.root.Get());
                                    m_context.AddRootView(c.renderRoot.Get());
                                    TextureCanvasRoot entry;
                                    entry.root = c.renderRoot;
                                    m_textureCanvasRoots.PushBack(Move(entry));
                                }
                                else
                                {
                                    DRACONIC_LOG_WARNING(
                                        u8"UI", u8"world panel document failed to instantiate");
                                }
                            }
                            c.builtFrom = document;
                        }
                        if (c.renderRoot.Get() != nullptr)
                        {
                            for (TextureCanvasRoot& entry : m_textureCanvasRoots)
                            {
                                if (entry.root.Get() == c.renderRoot.Get())
                                {
                                    entry.seen = true;
                                    break;
                                }
                            }
                        }
                        const UITheme* theme = c.theme.Get();
                        if (theme != c.themeFrom)
                        {
                            c.themeSheet = nullptr;
                            if (theme != nullptr && !theme->stylesheet.IsEmpty())
                            {
                                StyleSheetLoader loader;
                                loader.SetPalette(GameTheme::Palette());
                                c.themeSheet = loader.Load(theme->stylesheet.AsView());
                            }
                            if (c.root.Get() != nullptr)
                            {
                                c.root->SetLocalStyleSheet(c.themeSheet);
                            }
                            c.themeFrom = theme;
                        }
                    });
            }
        }
        // Sweep RenderTexture roots whose component vanished (despawn/removal/scene
        // teardown): unregister from the context - which stores roots NON-OWNING, so a
        // stale registration would dangle - then drop our keep-alive ref. (A mode-flip/
        // rebuild teardown above already unregistered; the second remove is a no-op.)
        for (usize i = m_textureCanvasRoots.Size(); i-- > 0;)
        {
            if (m_textureCanvasRoots[i].seen)
            {
                continue;
            }
            m_context.RemoveRootView(m_textureCanvasRoots[i].root.Get());
            m_textureCanvasRoots.RemoveAt(i);
        }
    }

    void UISubsystem::PumpInput()
    {
        // The global-overlay layer eats input only while it holds something INTERACTIVE
        // (see OnInit): a modal menu keeps the modal-while-occupied contract, but a
        // passive badge/watermark (pushed with IsHitTestVisible = false) must not turn
        // the full-window layer into a click shield over every scene HUD.
        const bool overlayActive = OverlayLayerWantsInput();
        if (m_overlayLayer.Get() != nullptr)
        {
            m_overlayLayer->IsHitTestVisible = overlayActive;
        }
        // The SAME facades the action layer evaluates: window coords in the player,
        // content coords in the Game tab (the InputSurface transform) - transparently.
        if (m_input == nullptr)
        {
            return;
        }
        draconic::input::IInputSourceProvider& devices = m_input->ActiveSource();
        draconic::shell::IMouse* mouse = devices.Mouse();
        InputManager& inputManager = *m_context.GetInputManager();

        // Per-surface scene binding (game-ui.md §9 known edge): which scene roots may
        // this frame's input reach? A BOUND source (the editor's Game tab binds its
        // scene on Play) confines routing + consumption to ITS scene's root - two
        // interactive scenes visible at once can no longer cross-route on overlapping
        // coordinates. An UN-BOUND source follows the input subsystem's policy:
        // AllScenes in the player (the shell owns the whole window - the historical
        // behavior), ScreenTierOnly in the editor's embedded runtime, where editing-
        // page HUDs render WYSIWYG but are deliberately NOT interactive (Simulate
        // included - the Game tab is the interactive-run surface). The scene-LESS
        // screen tier is never confined: global overlays sit above every scene and
        // stay modal while occupied.
        const void* boundSceneKey = m_input->BoundSceneKey();
        const bool unboundReachesScenes =
            m_input->UnboundScenePolicy() == draconic::input::UnboundInputScenePolicy::AllScenes;
        auto sceneRootEligible = [&](const SceneUI& ui) noexcept
        {
            if (boundSceneKey != nullptr)
            {
                return static_cast<const void*>(ui.scene) == boundSceneKey;
            }
            return unboundReachesScenes;
        };

        // World-panel pointer routing state: when the ray hits a panel, its root
        // becomes the active input root and the DISPATCH coordinates become the
        // panel-local pixels (the root's own space) instead of the raw pointer.
        bool panelPointer = false;
        Float2 panelPointerPx{0.0f, 0.0f};

        // The context dispatches input through ONE ActiveInputRoot (the UIHost multi-
        // window precedent) - with the tiers split across roots, pick it per frame:
        // an OCCUPIED screen tier is modal and always wins; otherwise the ELIGIBLE
        // root under the pointer; otherwise the nearest INTERACTIVE WORLD PANEL under
        // the camera ray; otherwise (pad/keyboard-only) the first eligible scene root
        // with canvas content, so gamepad nav reaches a pause menu no pointer hovered.
        {
            RootView* target = nullptr;
            if (overlayActive)
            {
                target = m_screenRoot.Get();
            }
            if (target == nullptr && mouse != nullptr)
            {
                const Float2 point{mouse->X(), mouse->Y()};
                if (m_screenRoot.Get() != nullptr)
                {
                    View* hit = m_screenRoot->HitTest(point);
                    if (hit != nullptr && hit != m_screenRoot.Get())
                    {
                        target = m_screenRoot.Get();
                    }
                }
                for (usize i = 0; target == nullptr && i < m_sceneUIs.Size(); ++i)
                {
                    if (!sceneRootEligible(m_sceneUIs[i]))
                    {
                        continue;
                    }
                    RootView* root = m_sceneUIs[i].root.Get();
                    if (root == nullptr)
                    {
                        continue;
                    }
                    View* hit = root->HitTest(point);
                    if (hit != nullptr && hit != root)
                    {
                        target = root;
                    }
                }
                // World tier: the pointer missed every overlay - cast the scene
                // camera's ray and take the NEAREST interactive panel it crosses.
                // In CAPTURED/look mode (FPS flying) the cursor is parked wherever it
                // was grabbed - the crosshair IS the pointer, so the ray goes through
                // the view center instead.
                if (target == nullptr)
                {
                    const bool centerAim = mouse->RelativeMode();
                    const bool rayDebug = std::getenv("DRACONIC_UI_RAY_DEBUG") != nullptr;
                    f32 bestDistance = 0.0f;
                    for (SceneUI& sceneUI : m_sceneUIs)
                    {
                        if (!sceneRootEligible(sceneUI))
                        {
                            continue;
                        }
                        auto* panels = sceneUI.scene->GetSystem<UIWorldPanelComponentManager>();
                        if (panels == nullptr)
                        {
                            continue;
                        }
                        // The surface's content size = the scene root's last-drawn
                        // viewport (zero before the first frame renders - no ray yet).
                        const Float2 viewSize = sceneUI.root.Get() != nullptr
                                                    ? sceneUI.root->ViewportSize
                                                    : Float2{0.0f, 0.0f};
                        if (viewSize.x <= 0.0f || viewSize.y <= 0.0f)
                        {
                            continue;
                        }
                        draconic::render::ViewCamera camera;
                        if (!draconic::render::ExtractPrimaryCamera(*sceneUI.scene, camera))
                        {
                            continue;
                        }
                        const Float2 rayPoint =
                            centerAim ? Float2{viewSize.x * 0.5f, viewSize.y * 0.5f} : point;
                        Float3 rayOrigin, rayDirection;
                        PointerRayFromCamera(camera, rayPoint, viewSize, rayOrigin, rayDirection);
                        if (rayDebug)
                        {
                            std::fprintf(stderr,
                                         "[UIRAY] view=%.0fx%.0f point=(%.0f,%.0f) center=%d "
                                         "origin=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f)\n",
                                         viewSize.x, viewSize.y, rayPoint.x, rayPoint.y,
                                         centerAim ? 1 : 0, rayOrigin.x, rayOrigin.y, rayOrigin.z,
                                         rayDirection.x, rayDirection.y, rayDirection.z);
                        }
                        panels->ForEach(
                            [&](UIWorldPanelComponent& c, scene::EntityHandle e)
                            {
                                if (!c.interactive || !c.visible)
                                {
                                    return;
                                }
                                if (c.renderRoot.Get() == nullptr)
                                {
                                    return;
                                }
                                const WorldPanelHit hit = RayHitWorldPanel(
                                    rayOrigin, rayDirection, sceneUI.scene->GetWorldMatrix(e),
                                    c.sizeMeters);
                                if (rayDebug)
                                {
                                    const Float4x4 w = sceneUI.scene->GetWorldMatrix(e);
                                    std::fprintf(stderr,
                                                 "[UIRAY]   panel at (%.2f,%.2f,%.2f) "
                                                 "size=(%.1f,%.1f) hit=%d uv=(%.2f,%.2f) d=%.2f\n",
                                                 w.m[3][0], w.m[3][1], w.m[3][2], c.sizeMeters.x,
                                                 c.sizeMeters.y, hit.hit ? 1 : 0, hit.uv.x,
                                                 hit.uv.y, hit.distance);
                                }
                                if (!hit.hit)
                                {
                                    return;
                                }
                                if (target != nullptr && hit.distance >= bestDistance)
                                {
                                    return;
                                }
                                target = c.renderRoot.Get();
                                bestDistance = hit.distance;
                                const Float2 rootSize = c.renderRoot->ViewportSize;
                                panelPointerPx =
                                    Float2{hit.uv.x * rootSize.x, hit.uv.y * rootSize.y};
                                panelPointer = true;
                            });
                    }
                    if (target == nullptr)
                    {
                        panelPointer = false;
                    }
                }
            }
            if (target == nullptr)
            {
                for (SceneUI& ui : m_sceneUIs)
                {
                    if (!sceneRootEligible(ui))
                    {
                        continue;
                    }
                    // Beyond the billboard layer = at least one canvas instantiated.
                    if (ui.root.Get() != nullptr && ui.root->ChildCount() > 1)
                    {
                        target = ui.root.Get();
                        break;
                    }
                }
            }
            if (std::getenv("DRACONIC_UI_RAY_DEBUG") != nullptr)
            {
                const char* kind = "none";
                if (target == m_screenRoot.Get())
                {
                    kind = overlayActive ? "screen(modal)" : "screen(hit)";
                }
                else if (target != nullptr)
                {
                    kind = "scene-or-panel";
                    for (SceneUI& ui : m_sceneUIs)
                    {
                        if (target == ui.root.Get())
                        {
                            kind = "scene-root";
                            break;
                        }
                    }
                }
                std::fprintf(stderr, "[UIRAY] frame target=%s mouse=%d pos=(%.0f,%.0f)\n", kind,
                             mouse != nullptr ? 1 : 0, mouse != nullptr ? mouse->X() : -1.0f,
                             mouse != nullptr ? mouse->Y() : -1.0f);
            }
            if (target == nullptr)
            {
                target = m_screenRoot.Get();
            }
            if (target != nullptr)
            {
                m_context.SetActiveInputRoot(target);
            }
        }
        if (mouse != nullptr)
        {
            // Panel routing swaps in panel-local pixels: the active root IS the panel's
            // standalone root, so dispatch coordinates live in its texture space.
            const f32 x = panelPointer ? panelPointerPx.x : mouse->X();
            const f32 y = panelPointer ? panelPointerPx.y : mouse->Y();
            inputManager.ProcessMouseMove(x, y);
            const draconic::shell::MouseButton shellButtons[3] = {
                draconic::shell::MouseButton::Left, draconic::shell::MouseButton::Right,
                draconic::shell::MouseButton::Middle};
            const MouseButton uiButtons[3] = {MouseButton::Left, MouseButton::Right,
                                              MouseButton::Middle};
            for (u32 i = 0; i < 3; ++i)
            {
                const bool down = mouse->IsButtonDown(shellButtons[i]);
                if (down && !m_prevButtons[i])
                {
                    inputManager.ProcessMouseDown(uiButtons[i], x, y, m_context.TotalTime());
                }
                else if (!down && m_prevButtons[i])
                {
                    inputManager.ProcessMouseUp(uiButtons[i], x, y);
                }
                m_prevButtons[i] = down;
            }
            const f32 wheel = mouse->ScrollY(); // per-frame delta already
            if (wheel != 0.0f)
            {
                inputManager.ProcessMouseWheel(x, y, mouse->ScrollX(), wheel);
            }
        }

        // ---- keyboard + text (game-ui.md P3): the tagged event stream off the SAME
        // provider seam - ordered key events and TextInput payloads that polling cannot
        // carry. The bridge applies the standard shell->UI key mapping (Return stays
        // dispatch-first) and reconciles the IME after every event; mouse/pad kinds are
        // skipped here (mouse is polled above - dispatching both would double-fire).
        // The provider gates: the player streams its window's events, the Game tab's
        // viewport source streams only while the viewport owns keyboard focus. ----
        for (const draconic::shell::InputEvent& event : devices.Events())
        {
            switch (event.kind)
            {
            case draconic::shell::InputEventKind::KeyDown:
            case draconic::shell::InputEventKind::KeyUp:
            case draconic::shell::InputEventKind::TextInput:
                (void)m_bridge.Dispatch(event);
                break;
            default:
                break;
            }
        }
        // Reconcile the IME even on event-less frames: focus can move without a key or
        // click (gamepad navigation onto/off an EditText).
        m_bridge.SyncTextInput();

        // ---- gamepad focus navigation (game-ui.md P2): dpad/left stick move focus
        // through the framework's geometric MoveFocus; South = Submit (synthesized
        // Return - the existing dispatch-first activation path), East = Cancel
        // (synthesized Escape). Hold-repeat: 0.4s initial, 0.12s after. The pad is
        // deliberately NOT a consumption class - gameplay pad actions keep working
        // (menus that want exclusivity push an input SET, the existing mechanism). ----
        draconic::shell::IGamepad* pad = devices.Gamepad(0);
        if (pad != nullptr && pad->Connected() && m_screenRoot.Get() != nullptr)
        {
            const f32 stickX = pad->Axis(draconic::shell::GamepadAxis::LeftX);
            const f32 stickY = pad->Axis(draconic::shell::GamepadAxis::LeftY);
            constexpr f32 kThreshold = 0.6f;
            const bool wants[4] = {
                pad->IsButtonDown(draconic::shell::GamepadButton::DPadUp) || stickY < -kThreshold,
                pad->IsButtonDown(draconic::shell::GamepadButton::DPadDown) || stickY > kThreshold,
                pad->IsButtonDown(draconic::shell::GamepadButton::DPadLeft) || stickX < -kThreshold,
                pad->IsButtonDown(draconic::shell::GamepadButton::DPadRight) || stickX > kThreshold,
            };
            const FocusDirection directions[4] = {FocusDirection::Up, FocusDirection::Down,
                                                  FocusDirection::Left, FocusDirection::Right};
            FocusManager* focus = m_context.GetFocusManager();
            for (u32 i = 0; i < 4; ++i)
            {
                if (!wants[i])
                {
                    m_navHeld[i] = false;
                    continue;
                }
                bool fire = false;
                if (!m_navHeld[i])
                {
                    fire = true;
                    m_navHeld[i] = true;
                    m_navRepeat[i] = 0.4f;
                }
                else
                {
                    m_navRepeat[i] -= m_navDeltaTime;
                    if (m_navRepeat[i] <= 0.0f)
                    {
                        fire = true;
                        m_navRepeat[i] = 0.12f;
                    }
                }
                if (!fire || focus == nullptr)
                {
                    continue;
                }
                if (focus->FocusedView() == nullptr)
                {
                    focus->FocusNext();
                } // bootstrap
                else
                {
                    (void)focus->MoveFocus(directions[i]);
                }
            }
            if (pad->IsButtonPressed(draconic::shell::GamepadButton::South))
            {
                inputManager.ProcessKeyDown(KeyCode::Return, KeyModifiers::None, false,
                                            m_context.TotalTime());
                inputManager.ProcessKeyUp(KeyCode::Return, KeyModifiers::None,
                                          m_context.TotalTime());
            }
            if (pad->IsButtonPressed(draconic::shell::GamepadButton::East))
            {
                inputManager.ProcessKeyDown(KeyCode::Escape, KeyModifiers::None, false,
                                            m_context.TotalTime());
                inputManager.ProcessKeyUp(KeyCode::Escape, KeyModifiers::None,
                                          m_context.TotalTime());
            }
        }

        // Consumption: pointer = an INTERACTIVE canvas under the cursor (or a pressed
        // view); keyboard = a focused text editor. Published to the action layer -
        // UI-consumed input never reaches gameplay actions. The pointer probes the
        // screen tier first (topmost), then the ELIGIBLE scene roots - the same
        // binding rule as routing, so an un-bound editor context never publishes a
        // spurious mask from HUDs it cannot interact with.
        bool pointer = false;
        if (mouse != nullptr)
        {
            const Float2 point{mouse->X(), mouse->Y()};
            if (m_screenRoot.Get() != nullptr)
            {
                View* hit = m_screenRoot->HitTest(point);
                pointer = hit != nullptr && hit != m_screenRoot.Get();
            }
            for (usize i = 0; !pointer && i < m_sceneUIs.Size(); ++i)
            {
                if (!sceneRootEligible(m_sceneUIs[i]))
                {
                    continue;
                }
                RootView* root = m_sceneUIs[i].root.Get();
                if (root == nullptr)
                {
                    continue;
                }
                View* hit = root->HitTest(point);
                pointer = hit != nullptr && hit != root;
            }
        }
        // A ray-hit interactive panel consumes the pointer like any hovered canvas. The pressed
        // view counts too - but NOT a root: a press over empty space parks on the RootView (so
        // mouse-up still routes), which is no UI interaction. Without the root exclusion, ANY
        // held click published a consumed mask and gated gameplay input for the press duration
        // (the PhysicsPlayground crosshair shove polls IsButtonPressed on exactly that frame).
        const View* pressedView = m_context.GetViewById(inputManager.PressedId());
        const bool pressedOnUI = pressedView != nullptr && pressedView->Parent != nullptr;
        pointer = pointer || panelPointer || pressedOnUI;
        const bool keyboard = m_context.WantsTextInput();
        m_pointerConsumed = pointer;
        m_input->Runtime().SetConsumptionMask(
            draconic::input::ActionRuntime::ConsumptionMask{pointer, keyboard});
    }

    // The scene-tier per-view sync: canvas visibility from the authored flag, then
    // billboard projection through the VIEW's real camera (world -> clip -> NDC -> px;
    // behind-camera parks at (-10000,-10000); distance scale as a 2D view-transform).
    // Public + encoder-free so headless tests drive it with a synthetic view.
    void UISubsystem::UpdateSceneView(scene::Scene& scene, const render::SceneOverlayView& view)
    {
        if (auto* canvases = scene.GetSystem<UICanvasComponentManager>())
        {
            canvases->ForEach(
                [&](UICanvasComponent& c, scene::EntityHandle)
                {
                    if (c.root.Get() != nullptr)
                    {
                        c.root->Visibility =
                            c.visible ? VisibilityValue::Visible : VisibilityValue::Gone;
                    }
                });
        }
        if (auto* billboards = scene.GetSystem<UIBillboardComponentManager>())
        {
            billboards->ForEach(
                [&](UIBillboardComponent& c, scene::EntityHandle e)
                {
                    if (c.root.Get() == nullptr)
                    {
                        return;
                    }
                    if (!c.visible)
                    {
                        c.root->Visibility = VisibilityValue::Gone;
                        return;
                    }
                    c.root->Visibility = VisibilityValue::Visible;
                    const Float4x4 entity = scene.GetWorldMatrix(e);
                    Float3 worldPos;
                    if (c.orientation == BillboardOrientation::Cylindrical)
                    {
                        const Float3 anchor = TransformPoint(Float3{0, 0, 0}, entity);
                        worldPos = Float3{anchor.x + c.offset.x, anchor.y + c.offset.y,
                                          anchor.z + c.offset.z};
                    }
                    else
                    {
                        worldPos = TransformPoint(c.offset, entity);
                    }
                    const Float4 clip =
                        Float4{worldPos.x, worldPos.y, worldPos.z, 1.0f} * view.viewProjection;
                    auto* lp = Cast<AbsoluteLayoutParams>(c.root->LayoutParams.Get());
                    if (lp == nullptr)
                    {
                        return;
                    }
                    if (clip.w <= 0.0f)
                    {
                        lp->X = -10000.0f; // behind the camera: clipped + unhit, no tree churn
                        lp->Y = -10000.0f;
                    }
                    else
                    {
                        // Pixels in VIEWPORT space: the scene root lays out at the viewport
                        // size and the VG viewport seam places it at the view's rect. The
                        // plate hangs BOTTOM-CENTERED on the projected point (and scales
                        // toward it - Origin below), so it hovers over the anchor instead
                        // of sliding off to the lower-right as distance changes. First
                        // frame uses a 0x0 unlaid-out size; it converges next frame.
                        const f32 ndcX = clip.x / clip.w;
                        const f32 ndcY = clip.y / clip.w;
                        const f32 px = (ndcX * 0.5f + 0.5f) * static_cast<f32>(view.viewportWidth);
                        const f32 py =
                            (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<f32>(view.viewportHeight);
                        lp->X = px - c.root->Width() * 0.5f;
                        lp->Y = py - c.root->Height();
                    }
                    f32 scale = 1.0f;
                    if (c.scaleMode == BillboardScale::Distance)
                    {
                        const Float3 toCamera{worldPos.x - view.cameraPosition.x,
                                              worldPos.y - view.cameraPosition.y,
                                              worldPos.z - view.cameraPosition.z};
                        const f32 distance = Max(Length(toCamera), 0.001f);
                        scale = Clamp(c.referenceDistance / distance, c.minScale, c.maxScale);
                    }
                    c.root->Transform.Scale = Float2{scale, scale};
                    // Distance scaling shrinks/grows toward the ANCHOR (bottom-center),
                    // matching the placement above.
                    c.root->Transform.Origin = Float2{0.5f, 1.0f};
                });
        }
    }

    // Scene tier (ISceneOverlay): called inside the compose's shared overlay pass, once
    // per view. Draws the view's scene root - matched by SceneKey - with the view's
    // REAL camera, so scene UI lands wherever the scene renders and billboards project
    // correctly in every viewport.
    void UISubsystem::Render(rhi::RenderPassEncoder& encoder, const render::SceneOverlayView& view)
    {
        if (m_render.Get() == nullptr || m_render->device == nullptr)
        {
            return;
        }
        if (view.viewportWidth == 0 || view.viewportHeight == 0)
        {
            return;
        }
        SceneUI* sceneUI = nullptr;
        for (SceneUI& ui : m_sceneUIs)
        {
            if (static_cast<const void*>(ui.scene) == view.sceneKey)
            {
                sceneUI = &ui;
                break;
            }
        }
        if (sceneUI == nullptr || sceneUI->root.Get() == nullptr)
        {
            return;
        }
        // The scene root lays out at the VIEWPORT size and draws at the view's rect
        // (split-screen halves each lay out their own HUD, clipped to their half).
        UpdateSceneView(*sceneUI->scene, view);
        // Stencil fills only when the pass carries a DS attachment in the SAME format our
        // stencil pipelines were built against (both sides probe the device in the same
        // candidate order, so agreement is the expected case).
        const bool stencil = view.depthStencilFormat != rhi::TextureFormat::Undefined &&
                             view.depthStencilFormat == m_render->canvasStencilFormat;
        DrawRootInPass(*sceneUI->root, encoder, view.targetFormat, view.viewportX, view.viewportY,
                       view.viewportWidth, view.viewportHeight, static_cast<i32>(view.frameIndex),
                       stencil);
    }

    // Screen tier (IScreenOverlay): called from the host's RenderOverlays per window
    // target, after the scene composed. Draws the scene-less global overlays.
    void UISubsystem::Render(rhi::RenderPassEncoder& encoder, const render::ScreenOverlayView& view)
    {
        if (m_render.Get() == nullptr || m_render->device == nullptr)
        {
            return;
        }
        if (m_screenRoot.Get() == nullptr || view.width == 0 || view.height == 0)
        {
            return;
        }
        const bool stencil = view.depthStencilFormat != rhi::TextureFormat::Undefined &&
                             view.depthStencilFormat == m_render->canvasStencilFormat;
        DrawRootInPass(*m_screenRoot, encoder, view.targetFormat, 0, 0, view.width, view.height,
                       static_cast<i32>(view.frameIndex), stencil);
    }

    // Records one root into an ALREADY-ACTIVE render pass: layout at the CONTENT size
    // (width/height), batch through the shared VGContext, upload a slice (pure mapped-
    // memory writes - legal during pass recording), draw at (viewportX, viewportY) via
    // the VG viewport seam. The per-format renderer's ring resets once per UI frame
    // (m_frameSerial) so same-frame draws never clobber each other.
    void UISubsystem::DrawRootInPass(RootView& root, rhi::RenderPassEncoder& encoder,
                                     rhi::TextureFormat format, i32 viewportX, i32 viewportY,
                                     u32 width, u32 height, i32 frameIndex, bool stencilCapable,
                                     u32 sampleCount)
    {
        root.ViewportSize = Float2{static_cast<f32>(width), static_cast<f32>(height)};
        m_context.UpdateRootView(&root);

        // The emission must match the pass this batch will render into: stencil-fill
        // commands only when the target pass carries the DS attachment (canvas RTTs and
        // the renderer/host-owned scene/screen overlay passes, which advertise their DS
        // format through the overlay view).
        const bool stencil =
            stencilCapable && m_render->canvasStencilFormat != rhi::TextureFormat::Undefined;
        m_render->vgContext.SetStencilFills(stencil);
        m_render->vgContext.Clear();
        m_context.DrawRootView(&root, m_render->vgContext);
        vg::VGBatch& batch = m_render->vgContext.GetBatch();
        m_render->vgContext.SetStencilFills(false); // reset; each tier opts in per batch
        if (batch.commands.IsEmpty())
        {
            return;
        }

        vg::renderer::VGRenderer* renderer =
            m_render->RendererFor(format, m_frameSerial, frameIndex, stencil, sampleCount);
        if (renderer == nullptr)
        {
            return;
        }
        const vg::renderer::VGRenderSlice slice =
            renderer->Prepare(batch, frameIndex, width, height);
        renderer->Render(encoder, viewportX, viewportY, width, height, frameIndex, slice);
    }

    // RenderTexture canvases (game-ui.md P3): draw each RT canvas's standalone root into
    // its subsystem-owned offscreen texture. Runs on the HOST's encoder BEFORE the scene
    // render (the RenderCanvasTextures seam next to EnsureRenderReady/RenderOverlays),
    // so materials sampling the texture see this frame's UI. Targets are created and
    // resized on demand, swept when their canvas vanishes or leaves the mode, and end
    // in ShaderRead. The VG ring gating is the shared one: DrawRootInPass goes through
    // RendererFor(format, m_frameSerial, frameIndex), which resets a format renderer's
    // ring at most once per UI frame - same-frame overlay draws are never clobbered.
    void UISubsystem::RenderCanvasTextures(rhi::CommandEncoder& encoder, i32 frameIndex)
    {
        // sRGB so the stored encoding matches the swapchain path: the VG shader emits
        // linear, the hardware encodes on write and decodes on sample - the panel's
        // sprite feeds the SAME linear values into the scene the HUD feeds the window.
        // (A world panel still tone-maps with the scene afterwards - it's IN the world;
        // that residual difference vs the post-tonemap HUD is by design.)
        constexpr rhi::TextureFormat kCanvasTextureFormat = rhi::TextureFormat::RGBA8UnormSrgb;
        if (m_render.Get() == nullptr || m_render->device == nullptr)
        {
            return;
        }
        // At most ONCE per UI frame: several hosts share one runtime context in the
        // editor (the Game tab + every open scene page call this seam), and one call
        // already renders EVERY scene's RT canvases - repeat calls would draw the same
        // targets again on the same encoder.
        if (m_canvasTexturesSerial == m_frameSerial)
        {
            return;
        }
        m_canvasTexturesSerial = m_frameSerial;
        for (auto& target : m_render->canvasTargets)
        {
            target.seen = false;
        }
        for (SceneUI& sceneUI : m_sceneUIs)
        {
            auto* canvases = sceneUI.scene->GetSystem<UICanvasComponentManager>();
            if (canvases == nullptr)
            {
                continue;
            }
            auto* sprites = sceneUI.scene->GetSystem<draconic::render::SpriteComponentManager>();
            auto* decals = sceneUI.scene->GetSystem<draconic::render::DecalComponentManager>();
            // Declarative RT-canvas -> material binding: the canvas ENTITY's own sprite/
            // decal runtime `texture` override tracks the canvas's CURRENT view (which
            // changes on resize). Deliberately UI-side: it writes the SAME override
            // manual/script assignment uses, so the render subsystem stays UI-unaware -
            // no new render fields, no importer/inspector surface. Cross-entity binding
            // stays manual (script refreshes from CanvasRenderTextureView per frame).
            auto bindEntityMaterials = [&](scene::EntityHandle entity, rhi::TextureView* oldView,
                                           rhi::TextureView* newView)
            {
                if (auto* sprite = sprites != nullptr ? sprites->Get(entity) : nullptr)
                {
                    if (newView != nullptr || sprite->texture == oldView)
                    {
                        sprite->texture = newView;
                    }
                }
                if (auto* decal = decals != nullptr ? decals->Get(entity) : nullptr)
                {
                    if (newView != nullptr || decal->texture == oldView)
                    {
                        decal->texture = newView;
                    }
                }
            };
            canvases->ForEach(
                [&](UICanvasComponent& c, scene::EntityHandle entity)
                {
                    if (c.renderMode != CanvasRenderMode::RenderTexture)
                    {
                        return;
                    }
                    if (c.renderRoot.Get() == nullptr)
                    {
                        return;
                    } // no document instantiated
                    const u32 width = Max(c.renderTextureWidth, 1u);
                    const u32 height = Max(c.renderTextureHeight, 1u);
                    rhi::TextureView* previousView = c.renderTextureView;
                    RenderState::CanvasTarget* target = m_render->EnsureCanvasTarget(
                        sceneUI.scene, entity, width, height, kCanvasTextureFormat);
                    if (target == nullptr)
                    {
                        c.renderTexture = nullptr;
                        c.renderTextureView = nullptr;
                        bindEntityMaterials(entity, previousView,
                                            nullptr); // never leave a freed view bound
                        return;
                    }
                    target->seen = true;
                    c.renderTexture = target->texture; // the component-level accessor
                    c.renderTextureView = target->view;
                    bindEntityMaterials(entity, previousView, target->view);
                    if (!c.visible)
                    {
                        return;
                    } // keep the texture, skip the draw
                    encoder.TransitionTexture(target->texture, target->state,
                                              rhi::ResourceState::RenderTarget);
                    const bool canvasMsaa = target->msaaView != nullptr;
                    if (canvasMsaa && target->msaaState == rhi::ResourceState::Undefined)
                    {
                        encoder.TransitionTexture(target->msaa, rhi::ResourceState::Undefined,
                                                  rhi::ResourceState::RenderTarget);
                        target->msaaState = rhi::ResourceState::RenderTarget;
                    }
                    rhi::RenderPassDesc pass;
                    rhi::ColorAttachment color;
                    // MSAA: render into the multisampled target, resolve into the sampled
                    // texture; the MSAA contents die with the pass (DontCare).
                    color.view = canvasMsaa ? target->msaaView : target->view;
                    color.resolveTarget = canvasMsaa ? target->view : nullptr;
                    color.loadOp = rhi::LoadOp::Clear; // fresh transparent background
                    color.storeOp = canvasMsaa ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
                    color.clearValue = rhi::ClearColor{0.0f, 0.0f, 0.0f, 0.0f};
                    pass.colorAttachments.Add(color);
                    const bool canvasStencil = target->depthStencilView != nullptr;
                    if (canvasStencil)
                    {
                        encoder.TransitionTexture(target->depthStencil,
                                                  rhi::ResourceState::Undefined,
                                                  rhi::ResourceState::DepthStencilWrite);
                        rhi::DepthStencilAttachment ds;
                        ds.view = target->depthStencilView;
                        ds.depthLoadOp = rhi::LoadOp::Clear;
                        ds.depthStoreOp = rhi::StoreOp::DontCare;
                        ds.stencilLoadOp = rhi::LoadOp::Clear; // stencil-then-cover expects 0
                        ds.stencilStoreOp = rhi::StoreOp::DontCare;
                        ds.stencilClearValue = 0;
                        pass.depthStencilAttachment = ds;
                    }
                    if (rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(pass))
                    {
                        DrawRootInPass(*c.renderRoot, *rp, kCanvasTextureFormat, 0, 0, width,
                                       height, frameIndex, canvasStencil, target->sampleCount);
                        rp->End();
                    }
                    encoder.TransitionTexture(target->texture, rhi::ResourceState::RenderTarget,
                                              rhi::ResourceState::ShaderRead);
                    target->state = rhi::ResourceState::ShaderRead;
                });

            // World panels: same target machinery, sized by PIXELS-PER-METER, and the
            // sibling sprite is DRIVEN outright (EntityOriented + sizeMeters + texture)
            // - the panel IS the authoring surface, the sprite is its render vehicle.
            // One RT consumer per entity: a panel and an RT canvas on the same entity
            // would collide on the (scene, entity) target key - warned, panel wins.
            if (auto* panels = sceneUI.scene->GetSystem<UIWorldPanelComponentManager>())
            {
                panels->ForEach(
                    [&](UIWorldPanelComponent& c, scene::EntityHandle entity)
                    {
                        if (c.renderRoot.Get() == nullptr)
                        {
                            return;
                        }
                        // The panel content is drawn INSET by a transparent border: the
                        // sprite quad's silhouette then lies in fully transparent texels,
                        // so the panel's outer edge is texture content smoothed by
                        // bilinear sampling (+ the canvas MSAA) instead of the quad's
                        // geometric edge, which the non-MSAA scene pass cannot antialias.
                        // The quad inflates by the same border so on-screen content scale
                        // and the authored-size input ray mapping are unchanged (rays
                        // over the skirt miss the authored quad - nothing lives there).
                        constexpr u32 kEdgePadding = 2;
                        const f32 ppm = Max(c.pixelsPerMeter, 1.0f);
                        const u32 width =
                            Clamp<u32>(static_cast<u32>(c.sizeMeters.x * ppm + 0.5f), 16u, 2044u);
                        const u32 height =
                            Clamp<u32>(static_cast<u32>(c.sizeMeters.y * ppm + 0.5f), 16u, 2044u);
                        const u32 paddedWidth = width + 2 * kEdgePadding;
                        const u32 paddedHeight = height + 2 * kEdgePadding;
                        RenderState::CanvasTarget* target = m_render->EnsureCanvasTarget(
                            sceneUI.scene, entity, paddedWidth, paddedHeight,
                            kCanvasTextureFormat);
                        if (target == nullptr)
                        {
                            c.renderTexture = nullptr;
                            c.renderTextureView = nullptr;
                            return;
                        }
                        target->seen = true;
                        c.renderTexture = target->texture;
                        c.renderTextureView = target->view;

                        // Drive the sprite (auto-added): the panel's quad in the world.
                        if (sprites != nullptr)
                        {
                            draconic::render::SpriteComponent* sprite = sprites->Get(entity);
                            if (sprite == nullptr)
                            {
                                sprite = &sprites->Add(entity);
                            }
                            sprite->orientation =
                                draconic::render::SpriteOrientation::EntityOriented;
                            // Inflate by the transparent border so the CONTENT keeps the
                            // authored world size (texture maps whole-quad).
                            sprite->size = Float2{
                                c.sizeMeters.x * (static_cast<f32>(paddedWidth) /
                                                  static_cast<f32>(width)),
                                c.sizeMeters.y * (static_cast<f32>(paddedHeight) /
                                                  static_cast<f32>(height))};
                            sprite->texture = target->view;
                            sprite->visible = c.visible;
                            // Post-tonemap: the panel keeps its AUTHORED colors (matching
                            // the screen-tier HUD) while still depth-testing into the scene.
                            sprite->postTonemap = true;
                        }
                        if (!c.visible)
                        {
                            return;
                        } // keep the texture, skip the draw
                        encoder.TransitionTexture(target->texture, target->state,
                                                  rhi::ResourceState::RenderTarget);
                        const bool panelMsaa = target->msaaView != nullptr;
                        if (panelMsaa && target->msaaState == rhi::ResourceState::Undefined)
                        {
                            encoder.TransitionTexture(target->msaa, rhi::ResourceState::Undefined,
                                                      rhi::ResourceState::RenderTarget);
                            target->msaaState = rhi::ResourceState::RenderTarget;
                        }
                        rhi::RenderPassDesc pass;
                        rhi::ColorAttachment color;
                        color.view = panelMsaa ? target->msaaView : target->view;
                        color.resolveTarget = panelMsaa ? target->view : nullptr;
                        color.loadOp = rhi::LoadOp::Clear;
                        color.storeOp =
                            panelMsaa ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
                        color.clearValue = rhi::ClearColor{0.0f, 0.0f, 0.0f, 0.0f};
                        pass.colorAttachments.Add(color);
                        if (rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(pass))
                        {
                            DrawRootInPass(*c.renderRoot, *rp, kCanvasTextureFormat,
                                           static_cast<i32>(kEdgePadding),
                                           static_cast<i32>(kEdgePadding), width, height,
                                           frameIndex, false, target->sampleCount);
                            rp->End();
                        }
                        encoder.TransitionTexture(target->texture, rhi::ResourceState::RenderTarget,
                                                  rhi::ResourceState::ShaderRead);
                        target->state = rhi::ResourceState::ShaderRead;
                    });
            }
        }
        // Sweep targets whose canvas vanished (despawn, scene destroyed, mode flip) -
        // and un-bind any sprite/decal override still pointing at the dying view (only
        // while the scene itself is alive; a destroyed scene took its components along).
        for (usize i = m_render->canvasTargets.Size(); i-- > 0;)
        {
            RenderState::CanvasTarget& target = m_render->canvasTargets[i];
            if (target.seen)
            {
                continue;
            }
            bool sceneAlive = false; // pointer compare only - the scene may be freed
            for (const SceneUI& ui : m_sceneUIs)
            {
                if (ui.scene == target.scene)
                {
                    sceneAlive = true;
                    break;
                }
            }
            if (target.view != nullptr && sceneAlive)
            {
                if (auto* sprites =
                        target.scene->GetSystem<draconic::render::SpriteComponentManager>())
                {
                    if (auto* sprite = sprites->Get(target.entity);
                        sprite != nullptr && sprite->texture == target.view)
                    {
                        sprite->texture = nullptr;
                    }
                }
                if (auto* decals =
                        target.scene->GetSystem<draconic::render::DecalComponentManager>())
                {
                    if (auto* decal = decals->Get(target.entity);
                        decal != nullptr && decal->texture == target.view)
                    {
                        decal->texture = nullptr;
                    }
                }
            }
            m_render->DestroyCanvasTarget(i);
        }
    }

    // Pass-owning draw body (the preview seam): opens its own Load-op pass on the
    // caller's encoder, then records through DrawRootInPass.
    void UISubsystem::DrawRootInto(RootView& root, rhi::CommandEncoder& encoder,
                                   rhi::TextureView* target, rhi::TextureFormat format, u32 width,
                                   u32 height, i32 frameIndex)
    {
        rhi::RenderPassDesc pass;
        rhi::ColorAttachment color;
        color.view = target;
        color.loadOp = rhi::LoadOp::Load;
        color.storeOp = rhi::StoreOp::Store;
        pass.colorAttachments.Add(color);
        if (rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(pass))
        {
            DrawRootInPass(root, *rp, format, 0, 0, width, height, frameIndex);
            rp->End();
        }
    }

    RefPtr<RootView> UISubsystem::CreatePreview(const UIDocument& document)
    {
        if (document.markup.IsEmpty())
        {
            return {};
        }
        RefPtr<View> tree = MarkupLoader::LoadFromString(document.markup.AsView(), &m_context);
        if (tree.Get() == nullptr)
        {
            return {};
        }
        RefPtr<RootView> root = MakeRef<RootView>(DefaultAllocator());
        root->AddView(tree.Get());
        // Registered on the GAME context (styles/fonts/ids resolve there) but NEVER on
        // the screen root or a scene root - the overlay roles only draw those, so
        // previews cannot appear in game targets; input stays with the active roots.
        m_context.AddRootView(root.Get());
        return root;
    }

    void UISubsystem::DestroyPreview(RootView* root)
    {
        if (root != nullptr)
        {
            m_context.RemoveRootView(root);
        }
    }

    void UISubsystem::RenderPreview(RootView& root, rhi::CommandEncoder& encoder,
                                    rhi::TextureView* target, rhi::TextureFormat format, u32 width,
                                    u32 height, i32 frameIndex)
    {
        if (target == nullptr || width == 0 || height == 0)
        {
            return;
        }
        if (m_render.Get() == nullptr || m_render->device == nullptr)
        {
            return;
        }
        DrawRootInto(root, encoder, target, format, width, height, frameIndex);
    }

    void UISubsystem::SetDefaultFont(const draconic::fonts::Font* font)
    {
        if (font == nullptr || font->EntryCount() == 0)
        {
            // Restore the TTF fallback service (dev path / no cooked font).
            m_resourceFonts = nullptr;
            m_context.SetFontService(m_fonts.Get());
            return;
        }
        m_resourceFonts = MakeUnique<draconic::fonts::ResourceFontService>(DefaultAllocator());
        m_resourceFonts->AddFont(font);
        m_context.SetFontService(m_resourceFonts.Get());
        DRACONIC_LOG_INFO(u8"UI", u8"default font bound: '{}' ({} baked size(s))",
                          font->Family(), static_cast<u64>(font->EntryCount()));
    }

    void UISubsystem::SetDefaultTheme(const UITheme* theme)
    {
        RefPtr<StyleSheet> sheet;
        if (theme != nullptr && !theme->stylesheet.IsEmpty())
        {
            StyleSheetLoader loader;
            loader.SetPalette(GameTheme::Palette());
            sheet = loader.Load(theme->stylesheet.AsView());
            if (sheet.Get() == nullptr)
            {
                DRACONIC_LOG_WARNING(
                    u8"UI", u8"default UI theme failed to parse - keeping the built-in GameTheme");
            }
        }
        m_theme = sheet.Get() != nullptr ? sheet : GameTheme::Create();
        m_context.SetStyleSheet(m_theme);
    }

    // Device/shader bring-up, called once by the application layer that owns graphics.
    void UISubsystem::EnsureRenderReady(rhi::Device& device, i32 frameCount)
    {
        if (m_render.Get() == nullptr)
        {
            m_render = MakeUnique<RenderState>(DefaultAllocator(), m_fonts.Get());
        }
        if (m_render->device != nullptr)
        {
            return;
        }
        m_render->device = &device;
        m_render->frameCount = frameCount;

        // Resolve the VG shaders through the shared ShaderSystemHost: cooked WGSL from shaders.dpak
        // (dist/browser, no compiler) or on-demand DXC over Data/Shaders (dev). The VG shaders ship
        // in the engine corpus (vg.vs/vg.ps) like every other shader.
#ifdef DRACONIC_ENGINE_SHADER_DIR
        constexpr StringView kShaderRoot = u8"" DRACONIC_ENGINE_SHADER_DIR;
#else
        constexpr StringView kShaderRoot = u8"Shaders";
#endif
        if (!m_render->shaderHost.Initialize(device, kShaderRoot))
        {
            DRACONIC_LOG_ERROR(u8"UI",
                               u8"no shader compiler and no shader pack - game UI will not render");
            return;
        }
        m_render->vertexShader = m_render->shaderHost.GetVariant(
            u8"vg", draconic::shaders::ShaderStage::Vertex, draconic::shaders::ShaderFlags::None);
        m_render->fragmentShader = m_render->shaderHost.GetVariant(
            u8"vg", draconic::shaders::ShaderStage::Fragment, draconic::shaders::ShaderFlags::None);
        m_render->gradRadialShader =
            m_render->shaderHost.GetVariant(u8"vg_grad_radial", draconic::shaders::ShaderStage::Fragment,
                                            draconic::shaders::ShaderFlags::None);
        m_render->gradConicShader =
            m_render->shaderHost.GetVariant(u8"vg_grad_conic", draconic::shaders::ShaderStage::Fragment,
                                            draconic::shaders::ShaderFlags::None);
        // MSDF text fragment (the DistanceField draw-mode pipeline).
        m_render->dfShader =
            m_render->shaderHost.GetVariant(u8"vg_df", draconic::shaders::ShaderStage::Fragment,
                                            draconic::shaders::ShaderFlags::None);
        // Per-pixel radial/conic gradients only if both shaders resolved (a pre-cooked pack may
        // predate them); otherwise every renderer + the context fall back to the affine LUT.
        m_render->vgContext.SetPerPixelGradients(m_render->gradRadialShader != nullptr &&
                                                 m_render->gradConicShader != nullptr);
        // Stencil support for CANVAS render-to-texture passes (the only passes this
        // subsystem OWNS - the scene/screen overlay passes belong to the renderer/host
        // and stay color-only until they provide a DS attachment themselves). Canvas UI
        // gets stencil-then-cover fill correctness; single-sampled, so complex-fill edges
        // rely on fringe-less coverage (fine for UI shapes).
        m_render->canvasStencilFormat =
            vg::renderer::PickStencilCapableFormat(device, /*sampleCount*/ 1);
    }
}

// ---- reflection (impl unit per the GCC rule) ----
namespace draconic::ui
{
    DRACONIC_REFLECT_ENUM(CanvasScalerMode, "draconic::ui")
    {
        builder.Value("ConstantPixel", CanvasScalerMode::ConstantPixel);
        builder.Value("ReferenceResolution", CanvasScalerMode::ReferenceResolution);
    }

    DRACONIC_REFLECT_ENUM(CanvasRenderMode, "draconic::ui")
    {
        builder.Value("ScreenOverlay", CanvasRenderMode::ScreenOverlay);
        builder.Value("RenderTexture", CanvasRenderMode::RenderTexture);
    }

    DRACONIC_REFLECT_ENUM(BillboardOrientation, "draconic::ui")
    {
        builder.Value("Screen", BillboardOrientation::Screen);
        builder.Value("Cylindrical", BillboardOrientation::Cylindrical);
    }

    DRACONIC_REFLECT_ENUM(BillboardScale, "draconic::ui")
    {
        builder.Value("Fixed", BillboardScale::Fixed);
        builder.Value("Distance", BillboardScale::Distance);
    }

    DRACONIC_REFLECT_VALUE(UIBillboardComponent, "draconic::ui")
    {
        builder.Attribute("displayName", String(u8"UI Billboard"))
            .Attribute("category", String(u8"UI")).DataVersion(1);
        // Script (Track A): world-space UI component .of(entity) -> live data (offset/orientation/
        // scale/visible). The SCREEN tier (IScreenOverlay, loading screen) is deliberately NOT exposed.
        builder.Method<&draconic::script::ComponentOf<UIBillboardComponent>, UIBillboardComponent>(
            "of");
        builder.Property<&UIBillboardComponent::document>("document");
        builder.Property<&UIBillboardComponent::offset>("offset");
        builder.Property<&UIBillboardComponent::orientation>("orientation");
        builder.Property<&UIBillboardComponent::scaleMode>("scaleMode");
        builder.Property<&UIBillboardComponent::referenceDistance>("referenceDistance");
        builder.Property<&UIBillboardComponent::minScale>("minScale");
        builder.Property<&UIBillboardComponent::maxScale>("maxScale");
        builder.Property<&UIBillboardComponent::visible>("visible");
    }

    DRACONIC_REFLECT_VALUE(UICanvasComponent, "draconic::ui")
    {
        builder.Attribute("displayName", String(u8"UI Canvas"))
            .Attribute("category", String(u8"UI")).DataVersion(2); // v2 added the RenderTexture canvas mode
        builder.Method<&draconic::script::ComponentOf<UICanvasComponent>, UICanvasComponent>("of");
        builder.Property<&UICanvasComponent::document>("document");
        builder.Property<&UICanvasComponent::theme>("theme");
        builder.Property<&UICanvasComponent::order>("order");
        builder.Property<&UICanvasComponent::visible>("visible");
        builder.Property<&UICanvasComponent::interactive>("interactive");
        builder.Property<&UICanvasComponent::scalerMode>("scalerMode");
        builder.Property<&UICanvasComponent::referenceResolution>("referenceResolution");
        builder.Property<&UICanvasComponent::renderMode>("renderMode");
        builder.Property<&UICanvasComponent::renderTextureWidth>("renderTextureWidth");
        builder.Property<&UICanvasComponent::renderTextureHeight>("renderTextureHeight");
    }

    DRACONIC_REFLECT_VALUE(UIWorldPanelComponent, "draconic::ui")
    {
        builder.Attribute("displayName", String(u8"UI World Panel"))
            .Attribute("category", String(u8"UI")).DataVersion(1);
        builder.Method<&draconic::script::ComponentOf<UIWorldPanelComponent>, UIWorldPanelComponent>(
            "of");
        builder.Property<&UIWorldPanelComponent::document>("document");
        builder.Property<&UIWorldPanelComponent::theme>("theme");
        builder.Property<&UIWorldPanelComponent::sizeMeters>("sizeMeters");
        builder.Property<&UIWorldPanelComponent::pixelsPerMeter>("pixelsPerMeter");
        builder.Property<&UIWorldPanelComponent::interactive>("interactive");
        builder.Property<&UIWorldPanelComponent::visible>("visible");
    }

    void RegisterUIComponentReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_CanvasScalerMode();
            DraconicRegisterEnum_CanvasRenderMode();
            DraconicRegisterEnum_BillboardOrientation();
            DraconicRegisterEnum_BillboardScale();
            DraconicRegisterValue_UICanvasComponent();
            DraconicRegisterValue_UIWorldPanelComponent();
            DraconicRegisterValue_UIBillboardComponent();
            return true;
        }();
        (void)once;
    }
}
