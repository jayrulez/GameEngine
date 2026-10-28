// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_hud partition, its body. See SceneHud.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.vg;
import engine.render;
import editor.core;
import editor.app;
import editor.camera;
import editor.viewporttools;
import :edit;
import :gizmo;
import :tools;
import :view_gizmo;
import :zoom_readout;
import :scene_overlays;
import :scene_hud;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace render = foundation::render;

namespace editor
{
    namespace
    {
        // The debug terms the shading's submenu offers (Albedo is Unlit, above them).
        struct Term
        {
            render::ViewDebugSemantic semantic;
            const char8_t* label;
        };
        constexpr Term kTerms[] = {
            {render::ViewDebugSemantic::Normal, u8"Normals"},
            {render::ViewDebugSemantic::Roughness, u8"Roughness"},
            {render::ViewDebugSemantic::Metallic, u8"Metallic"},
            {render::ViewDebugSemantic::Cascades, u8"Shadow Cascades"},
            {render::ViewDebugSemantic::ClusterHeat, u8"Light Clusters"},
            {render::ViewDebugSemantic::Overbright, u8"Overbright"},
        };
        // The effects as what is on: checked while the override leaves it.
        struct Effect
        {
            bool render::ViewPostOverride::* off;
            const char8_t* label;
        };
        constexpr Effect kEffects[] = {
            {&render::ViewPostOverride::disableBloom, u8"Bloom"},
            {&render::ViewPostOverride::disableAo, u8"Ambient Occlusion"},
            {&render::ViewPostOverride::disableSsr, u8"Reflections (SSR)"},
            {&render::ViewPostOverride::disableSsgi, u8"Global Illumination (SSGI)"},
            {&render::ViewPostOverride::disableAa, u8"Anti-aliasing"},
        };
        // The axis views, each the axis it looks along from.
        struct AxisView
        {
            Float3 axis;
            const char8_t* label;
        };
        constexpr AxisView kAxisViews[] = {
            {Float3{0, 1, 0}, u8"Top"},   {Float3{0, -1, 0}, u8"Bottom"}, {Float3{0, 0, 1}, u8"Front"},
            {Float3{0, 0, -1}, u8"Back"}, {Float3{1, 0, 0}, u8"Right"},   {Float3{-1, 0, 0}, u8"Left"},
        };
        // A texture's pass: what its name says before its first '.' ("ssgi.filtered": ssgi).
        String PassOf(StringView name)
        {
            for (usize i = 0; i < name.Size(); ++i)
                if (name[i] == u8'.')
                    return String(name.SubStr(0, i));
            return String(u8"other");
        }
        // A step as the snap menu writes it: to two decimals, none it does not need ("0.25", "1").
        String StepText(f32 value)
        {
            const i64 hundredths = static_cast<i64>(value * 100.0f + 0.5f);
            const i64 whole = hundredths / 100;
            const i64 part = hundredths % 100;
            if (part == 0)
                return Format(u8"{}", whole);
            if (part % 10 == 0)
                return Format(u8"{}.{}", whole, part / 10);
            return Format(u8"{}.{}{}", whole, part / 10, part % 10);
        }
        StringView NameOfTerm(render::ViewDebugSemantic semantic)
        {
            if (semantic == render::ViewDebugSemantic::Albedo)
                return u8"Unlit";
            for (const Term& term : kTerms)
                if (term.semantic == semantic)
                    return StringView(term.label);
            return u8"Lit";
        }
    }

    // === ViewGizmoView ===

    ViewGizmoView::ViewGizmoView()
    {
        AddClass(u8"hud-gizmo");
        for (RefPtr<gui::Label>& label : m_labels)
        {
            label = MakeRef<gui::Label>(MemoryAllocator());
            label->AddClass(u8"axis-label");
            label->SetTextAlignment(gui::TextHAlign::Center, gui::TextVAlign::Middle);
            label->SetHitTestVisible(false);
            AddChild(label.Get());
        }
        SetTooltip(u8"Click an axis to look along it; the centre switches perspective and orthographic");
    }

    void ViewGizmoView::Show(Float3 right, Float3 up, Float3 forward, bool orthographic)
    {
        if (m_shown && right == m_basis[0] && up == m_basis[1] && forward == m_basis[2] && orthographic == m_orthographic)
            return;
        m_basis[0] = right;
        m_basis[1] = up;
        m_basis[2] = forward;
        m_orthographic = orthographic;
        m_shown = true;
        RequestLayout();
        Invalidate();
    }

    void ViewGizmoView::OnLayout()
    {
        const gui::Rect box = GetContentBounds();
        m_layout = ViewGizmo::LayoutAround(m_basis[0], m_basis[1], m_basis[2], Float2{box.x + 0.5f * kSide, box.y + 0.5f * kSide});
        const f32 half = 0.5f * ViewGizmo::kKnob;
        for (i32 i = 0; i < 6; ++i)
        {
            const ViewGizmoPole& pole = m_layout.poles[i];
            m_labels[i]->SetText(ViewGizmo::Label(pole));
            if (pole.positive)
                m_labels[i]->RemoveClass(u8"negative");
            else
                m_labels[i]->AddClass(u8"negative");
            m_labels[i]->Arrange(gui::Rect{pole.screen.x - half, pole.screen.y - half, ViewGizmo::kKnob, ViewGizmo::kKnob});
        }
    }

    void ViewGizmoView::OnDraw(gui::DrawContext& ctx, const gui::Rect& localBounds)
    {
        (void)localBounds;
        // Back to front, so a knob toward the viewer covers the arms and knobs behind it.
        for (i32 i = 0; i < 6; ++i)
        {
            const ViewGizmoPole& pole = m_layout.poles[i];
            const Color color = ViewGizmo::ColorOf(pole, i == m_hovered);
            if (pole.positive)
            {
                foundation::vg::PathBuilder arm;
                arm.MoveTo(m_layout.center.x, m_layout.center.y);
                arm.LineTo(pole.screen.x, pole.screen.y);
                foundation::vg::StrokeStyle style;
                style.width = 2.0f;
                style.cap = foundation::vg::VGLineCap::Round;
                ctx.VG().StrokePath(arm.ToPath(), color, style);
            }
            ctx.VG().FillCircle(pole.screen, 0.5f * ViewGizmo::kKnob, color);
        }
        const Color centre = m_hovered == 6 ? Color{1.0f, 1.0f, 1.0f, 1.0f} : Color{0.75f, 0.75f, 0.75f, 1.0f};
        ctx.VG().FillCircle(m_layout.center, 0.5f * ViewGizmo::kCentre, centre);
    }

    bool ViewGizmoView::PressAt(Float2 local)
    {
        const i32 hit = ViewGizmo::Hit(m_layout, local.x, local.y);
        if (hit >= 0)
        {
            if (OnAxis)
                OnAxis(m_layout.poles[hit].axis);
            return true;
        }
        if (ViewGizmo::HitCentre(m_layout, local.x, local.y))
        {
            if (OnCentre)
                OnCentre();
            return true;
        }
        return false;
    }

    void ViewGizmoView::OnMouseDown(const gui::MouseEvent& event)
    {
        if (event.Button != gui::MouseButton::Left)
            return;
        if (PressAt(ConvertToNodeSpace(event.Position)))
            event.Handled = true;
    }

    void ViewGizmoView::Hover(Float2 local)
    {
        i32 hovered = ViewGizmo::Hit(m_layout, local.x, local.y);
        if (hovered < 0 && ViewGizmo::HitCentre(m_layout, local.x, local.y))
            hovered = 6;
        if (hovered != m_hovered)
        {
            m_hovered = hovered;
            Invalidate();
        }
    }
    void ViewGizmoView::OnMouseMove(const gui::MouseEvent& event) { Hover(ConvertToNodeSpace(event.Position)); }
    void ViewGizmoView::OnMouseLeave(const gui::MouseEvent&)
    {
        if (m_hovered != -1)
        {
            m_hovered = -1;
            Invalidate();
        }
    }

    // === ScaleBarView ===

    void ScaleBarView::OnDraw(gui::DrawContext& ctx, const gui::Rect& localBounds)
    {
        const Color ink = GetTextColor();
        const f32 y = localBounds.y + 0.5f * localBounds.height;
        ctx.VG().FillRect(gui::Rect{localBounds.x, y - 1.0f, m_pixels, 2.0f}.ToRectangle(), ink);
        ctx.VG().FillRect(gui::Rect{localBounds.x, localBounds.y, 1.0f, localBounds.height}.ToRectangle(), ink);
        ctx.VG().FillRect(gui::Rect{localBounds.x + m_pixels - 1.0f, localBounds.y, 1.0f, localBounds.height}.ToRectangle(), ink);
    }

    // === SceneHud ===

    SceneHud::SceneHud(IAllocator& allocator, Services services, const Function<gui::Node*(StringView)>& find)
        : m_allocator(allocator), m_services(Move(services)), m_checks(allocator)
    {
        const auto chip = [&](StringView id, Chip& out, void (SceneHud::*build)(gui::Menu&))
        {
            out.button = Cast<gui::ButtonBase>(find(id));
            if (auto* menu = Cast<gui::ToolMenuButton>(out.button))
                (this->*build)(menu->GetMenu());
        };
        chip(u8"chip-projection", m_projection, &SceneHud::BuildProjection);
        chip(u8"chip-shading", m_shading, &SceneHud::BuildShading);
        chip(u8"chip-overlays", m_overlays, &SceneHud::BuildOverlays);
        m_stats = Cast<gui::Label>(find(u8"hud-stats"));
        if (gui::Node* zone = find(u8"hud-top-end"))
        {
            m_gizmo = MakeRef<ViewGizmoView>(m_allocator);
            SceneHud* self = this;
            m_gizmo->OnAxis = [self](Float3 axis) { self->LookAlong(axis); };
            m_gizmo->OnCentre = [self]()
            {
                if (self->m_services.camera != nullptr)
                    self->SetProjection(self->m_services.camera->orthographic ? Projection::Perspective : Projection::Orthographic);
            };
            zone->AddChild(m_gizmo.Get());
        }
        m_zoomBox = find(u8"hud-zoom");
        m_zoomFocus = Cast<gui::Label>(find(u8"hud-zoom-focus"));
        m_zoomCell = Cast<gui::Label>(find(u8"hud-zoom-cell"));
        m_zoomLength = Cast<gui::Label>(find(u8"hud-zoom-length"));
        if (gui::Node* row = find(u8"hud-zoom-bar"))
        {
            m_zoomBar = MakeRef<ScaleBarView>(m_allocator);
            row->AddChildAt(m_zoomBar.Get(), 0);
        }
        m_status = find(u8"hud-status");
        m_statusIcon = Cast<gui::Image>(find(u8"hud-status-icon"));
        const char8_t* const parts[] = {u8"hud-status-mode", u8"hud-status-space", u8"hud-status-snap", u8"hud-status-selection"};
        for (usize i = 0; i < 4; ++i)
            m_statusParts[i] = Cast<gui::Label>(find(StringView(parts[i])));
        Update(0.0f, 0);
    }

    gui::MenuItem* SceneHud::AddCheck(gui::Menu& menu, StringView label, Function<bool()> on, Function<void()> pick)
    {
        gui::MenuItem* item = menu.AddCheckItem(label, on ? on() : false, [onPick = Move(pick)](bool) { if (onPick) onPick(); });
        m_checks.PushBack(Checked{item, Move(on)});
        return item;
    }

    void SceneHud::BuildProjection(gui::Menu& menu)
    {
        SceneHud* self = this;
        EditorCamera* camera = m_services.camera;
        if (camera == nullptr)
            return;
        AddCheck(menu, u8"Perspective", [camera]() { return !camera->orthographic; }, [self]() { self->SetProjection(Projection::Perspective); });
        AddCheck(menu, u8"Orthographic", [camera]() { return camera->orthographic; }, [self]() { self->SetProjection(Projection::Orthographic); });
        menu.AddSeparator();
        for (const AxisView& view : kAxisViews)
        {
            const Float3 axis = view.axis;
            (void)menu.AddItem(StringView(view.label), [self, axis]() { self->LookAlong(axis); });
        }
    }

    void SceneHud::BuildShading(gui::Menu& menu)
    {
        SceneHud* self = this;
        render::ViewDebugView* debug = m_services.debugView;
        render::ViewPostOverride* post = m_services.post;
        if (debug == nullptr || post == nullptr)
            return;
        const auto showing = [debug](render::ViewDebugSemantic semantic)
        { return [debug, semantic]() { return debug->resource.IsEmpty() && debug->semantic == semantic; }; };
        AddCheck(menu, u8"Lit", showing(render::ViewDebugSemantic::Off), [self]() { self->SetShading(render::ViewDebugSemantic::Off); });
        AddCheck(menu, u8"Unlit", showing(render::ViewDebugSemantic::Albedo), [self]() { self->SetShading(render::ViewDebugSemantic::Albedo); });
        gui::Menu* terms = menu.AddSubMenu(u8"Debug View");
        for (const Term& term : kTerms)
        {
            const render::ViewDebugSemantic semantic = term.semantic;
            AddCheck(*terms, StringView(term.label), showing(semantic), [self, semantic]() { self->SetShading(semantic); });
        }
        gui::MenuItem* textures = AddCheck(menu, u8"Render Textures...", [debug]() { return !debug->resource.IsEmpty(); },
                                           [self]() { self->OpenTextures(); });
        textures->SetEnabled(static_cast<bool>(m_services.debugResources) && static_cast<bool>(m_services.pick));
        menu.AddSeparator();
        gui::Menu* effects = menu.AddSubMenu(u8"Effects");
        AddCheck(*effects, u8"Post-processing", [post]() { return !post->disablePost; }, [post]() { post->disablePost = !post->disablePost; });
        effects->AddSeparator();
        for (const Effect& effect : kEffects)
        {
            bool render::ViewPostOverride::* off = effect.off;
            AddCheck(*effects, StringView(effect.label), [post, off]() { return !(post->*off); }, [post, off]() { post->*off = !(post->*off); });
        }
        effects->AddSeparator();
        AddCheck(*effects, u8"Frustum Culling", [post]() { return !post->disableCulling; }, [post]() { post->disableCulling = !post->disableCulling; });
        gui::Menu* msaa = menu.AddSubMenu(u8"MSAA");
        for (u32 i = 0; i < engine::render::MsaaLevelCount(); ++i)
        {
            const u8 samples = static_cast<u8>(engine::render::kMsaaLevels[i].samples);
            AddCheck(*msaa, engine::render::kMsaaLevels[i].label,
                     [post, samples]() { return samples <= 1 ? post->msaaOverride <= 1 : post->msaaOverride == samples; },
                     [post, samples]() { post->msaaOverride = samples; });
        }
    }

    void SceneHud::BuildOverlays(gui::Menu& menu)
    {
        SceneOverlays* overlays = m_services.overlays;
        if (overlays == nullptr)
            return;
        SceneHud* self = this;
        const auto toggle = [self, overlays](bool SceneOverlaySettings::* field)
        {
            return [self, overlays, field]()
            {
                overlays->settings.*field = !(overlays->settings.*field);
                if (self->m_services.overlaysChanged)
                    self->m_services.overlaysChanged();
            };
        };
        const auto shown = [overlays](bool SceneOverlaySettings::* field) { return [overlays, field]() { return overlays->settings.*field; }; };
        AddCheck(menu, u8"Grid", shown(&SceneOverlaySettings::showGrid), toggle(&SceneOverlaySettings::showGrid));
        gui::Menu* planes = menu.AddSubMenu(u8"Grid Plane");
        struct Plane
        {
            GridPlane plane;
            const char8_t* label;
        };
        constexpr Plane kPlanes[] = {{GridPlane::XZ, u8"XZ (ground)"}, {GridPlane::XY, u8"XY"}, {GridPlane::YZ, u8"YZ"}};
        for (const Plane& plane : kPlanes)
        {
            const GridPlane value = plane.plane;
            AddCheck(*planes, StringView(plane.label), [overlays, value]() { return overlays->settings.gridPlane == value; },
                     [self, overlays, value]()
                     {
                         overlays->settings.gridPlane = value;
                         if (self->m_services.overlaysChanged)
                             self->m_services.overlaysChanged();
                     });
        }
        AddCheck(menu, u8"Entity Markers", shown(&SceneOverlaySettings::showMarkers), toggle(&SceneOverlaySettings::showMarkers));
        AddCheck(menu, u8"Colliders", shown(&SceneOverlaySettings::showColliders), toggle(&SceneOverlaySettings::showColliders));
        AddCheck(menu, u8"LOD Tint", shown(&SceneOverlaySettings::showLodOverlay), toggle(&SceneOverlaySettings::showLodOverlay));
        menu.AddSeparator();
        AddCheck(menu, u8"Stats", shown(&SceneOverlaySettings::showFps), toggle(&SceneOverlaySettings::showFps));
    }

    void SceneHud::SetProjection(Projection projection)
    {
        if (m_services.camera != nullptr)
            m_services.camera->SetOrthographic(projection == Projection::Orthographic);
    }

    void SceneHud::LookAlong(Float3 axis)
    {
        EditorCamera* camera = m_services.camera;
        if (camera == nullptr)
            return;
        f32 yaw = camera->yaw;
        f32 pitch = camera->pitch;
        ViewGizmo::SnapAngles(axis, camera->yaw, yaw, pitch);
        camera->autoOrthographic = m_services.context != nullptr && AutoOrthographicViews(m_services.context->UserEditorSettings());
        camera->SnapToAxis(yaw, pitch);
    }

    void SceneHud::SetShading(render::ViewDebugSemantic semantic)
    {
        if (m_services.debugView == nullptr)
            return;
        m_services.debugView->resource.Clear(); // a term and a texture are one or the other
        m_services.debugView->semantic = semantic;
    }

    void SceneHud::ShowTexture(StringView name)
    {
        render::ViewDebugView* debug = m_services.debugView;
        if (debug == nullptr)
            return;
        debug->resource = String(name);
        debug->semantic = render::ViewDebugSemantic::Off;
        // A depth texture shows as distance between the editor camera's planes.
        if (m_services.camera != nullptr)
        {
            debug->nearZ = m_services.camera->nearZ;
            debug->farZ = m_services.camera->DrawFar();
        }
    }

    void SceneHud::OpenTextures()
    {
        if (!m_services.debugResources || !m_services.pick)
            return;
        Array<render::DebugResourceInfo> rows(m_allocator);
        m_services.debugResources(rows);
        app::PickerRequest request;
        request.Placeholder = String(u8"Search render textures");
        request.EmptyTitle = String(u8"No render textures yet");
        request.EmptyText = String(u8"The renderer lists a frame's textures once it has drawn one.");
        if (m_services.debugView != nullptr)
            request.Current = m_services.debugView->resource;
        // By their pass, in the order the frame made them; a multisampled one is shown by its
        // resolved twin (a blit cannot read it).
        Array<String> passes(m_allocator);
        for (const render::DebugResourceInfo& row : rows)
        {
            if (row.samples > 1)
                continue;
            const String pass = PassOf(row.name.AsView());
            bool known = false;
            for (const String& seen : passes)
                known = known || seen == pass;
            if (!known)
                passes.PushBack(pass);
        }
        for (const String& pass : passes)
            for (const render::DebugResourceInfo& row : rows)
            {
                if (row.samples > 1 || PassOf(row.name.AsView()) != pass)
                    continue;
                app::PickerItem item;
                item.Key = row.name;
                item.Name = row.name;
                item.Detail = Format(u8"{}x{}{}", row.width, row.height, row.isDepth ? StringView(u8"  depth") : StringView{});
                item.Section = pass;
                item.Glyph = String(u8"Editor/Icons/texture.svg");
                request.Items.PushBack(Move(item));
            }
        SceneHud* self = this;
        request.OnPicked = [self](StringView key)
        {
            if (!key.IsEmpty())
                self->ShowTexture(key);
        };
        m_services.pick(Move(request));
    }

    String SceneHud::AxisViewName() const
    {
        const EditorCamera* camera = m_services.camera;
        if (camera == nullptr)
            return String{};
        const Float3 forward = camera->Forward();
        for (const AxisView& view : kAxisViews)
            if (Dot(forward, view.axis) < -0.9999f) // looking along it from its side
                return String(view.label);
        return String{};
    }

    void SceneHud::ShowChip(Chip& chip, StringView text, bool on)
    {
        if (chip.button == nullptr)
            return;
        if (chip.text.AsView() != text)
        {
            chip.button->SetText(text);
            chip.text = String(text);
        }
        if (chip.on != on)
        {
            if (on)
                chip.button->AddClass(u8"on");
            else
                chip.button->RemoveClass(u8"on");
            chip.on = on;
        }
    }

    void SceneHud::Update(f32 dt, u32 viewHeight)
    {
        for (Checked& check : m_checks)
            if (check.on)
            {
                const bool on = check.on();
                if (check.item->IsChecked() != on)
                    check.item->SetCheckedSilently(on);
            }
        if (const EditorCamera* camera = m_services.camera)
        {
            const String axis = AxisViewName();
            if (!axis.IsEmpty())
                ShowChip(m_projection, axis.AsView(), true);
            else
                ShowChip(m_projection, camera->orthographic ? StringView(u8"Orthographic") : StringView(u8"Perspective"), camera->orthographic);
            if (m_gizmo)
                m_gizmo->Show(camera->Right(), camera->Up(), camera->Forward(), camera->orthographic);
        }
        if (const render::ViewDebugView* debug = m_services.debugView)
        {
            const bool lit = debug->resource.IsEmpty() && debug->semantic == render::ViewDebugSemantic::Off;
            ShowChip(m_shading, !debug->resource.IsEmpty() ? debug->resource.AsView() : NameOfTerm(debug->semantic), !lit);
        }
        ShowChip(m_overlays, u8"Overlays", false);
        UpdateStats(dt);
        UpdateStatus();
        UpdateZoom(dt, viewHeight);
    }

    void SceneHud::UpdateStats(f32 dt)
    {
        const bool shown = m_services.overlays != nullptr && m_services.overlays->settings.showFps;
        if (m_stats == nullptr)
            return;
        if (!shown)
        {
            m_stats->SetDisplayed(false);
            m_statsSeconds = 0.0;
            m_statsFrames = 0;
            m_statsText.Clear();
            return;
        }
        // Sampled over half a second, so it can be read.
        m_statsSeconds += static_cast<f64>(dt);
        m_statsFrames += 1;
        if (m_statsSeconds >= 0.5 || m_statsText.IsEmpty())
        {
            m_statsText = FrameRateOverlayText(m_statsSeconds, m_statsFrames);
            m_statsSeconds = 0.0;
            m_statsFrames = 0;
            m_stats->SetText(m_statsText.AsView());
        }
        m_stats->SetDisplayed(true);
    }

    void SceneHud::UpdateStatus()
    {
        if (m_status == nullptr)
            return;
        const IViewportTool* active = m_services.tools != nullptr ? m_services.tools->ActiveTool() : nullptr;
        StringView parts[4];
        String icon;
        String snapText;
        String selectionText;
        GizmoController* gizmos = m_services.gizmos ? m_services.gizmos() : nullptr;
        if (active != nullptr && active == m_services.selectTool && gizmos != nullptr)
        {
            // The select tool in parts: what a drag does, in which space, by which step, to what.
            if (!gizmos->HandlesShown())
            {
                icon = String(u8"Editor/Icons/select.svg");
                parts[0] = u8"Select";
            }
            else
            {
                switch (gizmos->Mode())
                {
                case GizmoMode::Translate:
                    icon = String(u8"Editor/Icons/move.svg");
                    parts[0] = u8"Move";
                    break;
                case GizmoMode::Rotate:
                    icon = String(u8"Editor/Icons/rotate.svg");
                    parts[0] = u8"Rotate";
                    break;
                case GizmoMode::Scale:
                    icon = String(u8"Editor/Icons/scale.svg");
                    parts[0] = u8"Scale";
                    break;
                }
                // Scale is always in the selection's own space.
                parts[1] = gizmos->Mode() == GizmoMode::Scale || gizmos->Space() == GizmoSpace::Local ? StringView(u8"Local") : StringView(u8"World");
                if (gizmos->SnapMode())
                {
                    const TransformGizmo& gizmo = gizmos->Gizmo();
                    snapText = gizmos->Mode() == GizmoMode::Translate ? Format(u8"Snap {} m", StepText(gizmo.translateSnap))
                               : gizmos->Mode() == GizmoMode::Rotate ? Format(u8"Snap {}°", StepText(gizmo.rotateSnapDegrees))
                                                                     : Format(u8"Snap {}", StepText(gizmo.scaleSnap));
                    parts[2] = snapText.AsView();
                }
            }
            if (m_services.edit != nullptr)
            {
                Selection<Guid>& selection = m_services.edit->EntitySelection();
                const Guid* primary = selection.Primary();
                const foundation::scene::EntityHandle e = primary != nullptr ? m_services.edit->Resolve(*primary) : foundation::scene::EntityHandle{};
                if (e.IsAssigned())
                {
                    const StringView name = m_services.edit->Scene().GetEntityName(e);
                    selectionText = selection.Size() > 1 ? Format(u8"{} +{}", name, selection.Size() - 1) : String(name);
                    parts[3] = selectionText.AsView();
                }
            }
        }
        else if (active != nullptr)
            parts[0] = active->StatusText(); // a tool's own words, until each says it in parts (part 3)
        if (m_statusIcon != nullptr)
        {
            m_statusIcon->SetDisplayed(!icon.IsEmpty());
            if (!icon.IsEmpty() && m_statusIcon->GetSource() != icon)
                m_statusIcon->SetSource(icon);
        }
        for (usize i = 0; i < 4; ++i)
            if (gui::Label* label = m_statusParts[i])
            {
                if (label->GetText() != parts[i])
                    label->SetText(parts[i]);
                label->SetDisplayed(!parts[i].IsEmpty());
            }
        m_status->SetDisplayed(!parts[0].IsEmpty());
    }

    void SceneHud::UpdateZoom(f32 dt, u32 viewHeight)
    {
        m_zoom.Advance(dt);
        const f32 opacity = m_zoom.Opacity();
        if (m_zoomBox == nullptr)
            return;
        m_zoomBox->SetDisplayed(opacity > 0.0f);
        if (opacity <= 0.0f || m_services.camera == nullptr || m_services.overlays == nullptr)
            return;
        m_zoomBox->SetAlpha(opacity);
        const EditorCamera& camera = *m_services.camera;
        const ScaleBar bar = ScaleBarAt(camera.focusDistance, camera.fovY, static_cast<f32>(Max(viewHeight, 1u)));
        if (m_zoomFocus != nullptr)
            m_zoomFocus->SetText(Format(u8"{} to focus", FormatMetres(camera.focusDistance)).AsView());
        if (m_zoomCell != nullptr)
            m_zoomCell->SetText(Format(u8"Grid cell {}", FormatMetres(m_services.overlays->CurrentGridSpacing(camera).spacing)).AsView());
        if (m_zoomLength != nullptr)
            m_zoomLength->SetText(FormatMetres(bar.metres).AsView());
        if (m_zoomBar)
            m_zoomBar->SetLength(bar.pixels);
    }

    StringView SceneHud::ChipText(StringView id) const
    {
        const Chip* chip = id == StringView(u8"chip-projection") ? &m_projection : id == StringView(u8"chip-shading") ? &m_shading : &m_overlays;
        return chip->text.AsView();
    }
    bool SceneHud::ChipOn(StringView id) const
    {
        const Chip* chip = id == StringView(u8"chip-projection") ? &m_projection : id == StringView(u8"chip-shading") ? &m_shading : &m_overlays;
        return chip->on;
    }
    gui::Menu* SceneHud::ChipMenu(StringView id) const
    {
        const Chip* chip = id == StringView(u8"chip-projection") ? &m_projection : id == StringView(u8"chip-shading") ? &m_shading : &m_overlays;
        auto* menu = Cast<gui::ToolMenuButton>(chip->button);
        return menu != nullptr ? &menu->GetMenu() : nullptr;
    }
    String SceneHud::StatusText() const
    {
        String out;
        if (m_status == nullptr || !m_status->IsDisplayed())
            return out;
        for (gui::Label* label : m_statusParts)
            if (label != nullptr && label->IsDisplayed())
            {
                if (!out.IsEmpty())
                    out += StringView(u8" | ");
                out += label->GetText();
            }
        return out;
    }
    bool SceneHud::ZoomShown() const { return m_zoomBox != nullptr && m_zoomBox->IsDisplayed(); }
    bool SceneHud::StatsShown() const { return m_stats != nullptr && m_stats->IsDisplayed(); }
}
