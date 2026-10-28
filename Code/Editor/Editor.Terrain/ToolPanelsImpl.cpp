// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :tool_panels partition, its body. See ToolPanels.cppm.
//
// Each provider downcasts the active IViewportTool to its concrete tool (the id matched, so the
// type is known - the panel seam's contract) to read and write the brush's settings the UI-free
// tool cannot expose; the panel's kit asks them again each frame, so the tool's keys (1 to 4, Shift
// + wheel) show in it.

module;
#include "Core/Prelude.h"

module editor.terrain;

import foundation.core;
import foundation.gui;
import foundation.scene;    // Scene, ComponentManagerBase, EntityHandle
import foundation.content;  // Instance::Name (a layer's tooltip)
import engine.terrain;      // TerrainComponent(Manager)
import foundation.terrain;  // TerrainResource and its layers' albedo
import editor.core;          // EditorContext, ThumbnailService
import editor.app;          // the panel seam and its kit
import editor.viewporttools; // IViewportTool, ViewportToolHostContext
import :sculpt;
import :splat;
import :hole;
import :splatmap_thumbnail;
import :tool_panels;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;

    namespace
    {
        // The layers the Paint Layers panel offers: the FIRST terrain's in the scene. The brush
        // paints whichever terrain is under the cursor; the panel names the first one's (the one
        // terrain of most scenes).
        struct LayerSet
        {
            u32 count = 0;   // its palette's, unbounded
            Array<Guid> ids; // each palette layer's albedo
            Guid baseId;     // the base layer's albedo, never painted (erasing reveals it)
        };

        LayerSet ResolveLayers(scene::Scene& sc)
        {
            LayerSet out;
            auto* mgr = static_cast<engine::terrain::TerrainComponentManager*>(
                sc.FindManagerByComponentType(TypeOf<engine::terrain::TerrainComponent>()));
            if (mgr == nullptr)
                return out;
            mgr->ForEach([&](engine::terrain::TerrainComponent& c, scene::EntityHandle)
                         {
                             foundation::terrain::TerrainResource* res = c.terrain.Get();
                             if (out.count > 0 || res == nullptr)
                                 return; // the first terrain with layers
                             for (u32 i = 0; i < res->PaletteCount(); ++i)
                                 out.ids.PushBack(res->palette[i].albedo.id);
                             out.count = res->PaletteCount();
                             out.baseId = res->base.albedo.id;
                         });
            return out;
        }

        // A layer's albedo by its asset's name: at a swatch's size a displacement map picked in
        // place of its diffuse looks the same, the name tells them apart.
        String AlbedoName(EditorContext* ctx, const Guid& id)
        {
            if (id.IsNil())
                return String(u8"no albedo");
            if (ctx != nullptr && ctx->Project() != nullptr)
                if (foundation::content::Instance* instance = ctx->Project()->SourceDb().GetInstance(id))
                    return String(instance->Name());
            return String(u8"its albedo is missing");
        }

        app::ToolPanelBody::Choice Word(StringView label, StringView tooltip)
        {
            app::ToolPanelBody::Choice choice;
            choice.label = String(label);
            choice.tooltip = String(tooltip);
            return choice;
        }

        class SculptPanelProvider final : public IViewportToolPanelProvider
        {
        public:
            [[nodiscard]] StringView ToolId() const override { return u8"terrain.sculpt"; }
            [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext&) override
            {
                TerrainSculptTool* t = static_cast<TerrainSculptTool*>(&tool);
                auto body = MakeRef<app::ToolPanelBody>(EditorRootAllocator());
                Array<app::ToolPanelBody::Choice> modes;
                modes.PushBack(Word(u8"Raise", u8"Push the surface up (1)"));
                modes.PushBack(Word(u8"Lower", u8"Push it down (2)"));
                modes.PushBack(Word(u8"Smooth", u8"Even out bumps toward their neighbours (3)"));
                modes.PushBack(Word(u8"Flatten", u8"Pull toward the height Ctrl+click picked (4)"));
                body->AddChoices({}, Move(modes), [t]() { return static_cast<i32>(t->GetMode()); },
                                 [t](i32 i) { t->SetMode(static_cast<TerrainSculptTool::Mode>(i)); });
                body->AddSlider(u8"Radius", 0.5, 128.0, 0.5, [t]() { return static_cast<f64>(t->Radius()); },
                                [t](f64 v) { t->SetRadius(static_cast<f32>(v)); });
                body->AddSlider(u8"Strength", 0.0, 50.0, 0.5, [t]() { return static_cast<f64>(t->Strength()); },
                                [t](f64 v) { t->SetStrength(static_cast<f32>(v)); });
                body->AddHelp(u8"Drag to sculpt. 1 to 4 pick the mode, Shift+wheel sizes the brush, Ctrl+click picks Flatten's height.");
                body->Sync();
                return body;
            }
        };

        class HolePanelProvider final : public IViewportToolPanelProvider
        {
        public:
            [[nodiscard]] StringView ToolId() const override { return u8"terrain.hole"; }
            [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext&) override
            {
                TerrainHoleTool* t = static_cast<TerrainHoleTool*>(&tool);
                auto body = MakeRef<app::ToolPanelBody>(EditorRootAllocator());
                Array<app::ToolPanelBody::Choice> modes;
                modes.PushBack(Word(u8"Cut", u8"Cut the surface under the brush: nothing draws, collides, walks or grows there (1)"));
                modes.PushBack(Word(u8"Fill", u8"Fill a cut back in (2)"));
                body->AddChoices({}, Move(modes), [t]() { return t->GetMode() == TerrainHoleTool::Mode::Cut ? 0 : 1; },
                                 [t](i32 i) { t->SetMode(i == 0 ? TerrainHoleTool::Mode::Cut : TerrainHoleTool::Mode::Fill); });
                body->AddSlider(u8"Radius", 0.5, 128.0, 0.5, [t]() { return static_cast<f64>(t->Radius()); },
                                [t](f64 v) { t->SetRadius(static_cast<f32>(v)); });
                body->AddHelp(u8"Drag to cut or fill. 1 cuts, 2 fills, Shift+wheel sizes the brush.");
                body->Sync();
                return body;
            }
        };

        class SplatPanelProvider final : public IViewportToolPanelProvider
        {
        public:
            [[nodiscard]] StringView ToolId() const override { return u8"terrain.splat"; }
            [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext& ctx) override
            {
                TerrainSplatTool* t = static_cast<TerrainSplatTool*>(&tool);
                ThumbnailService* thumbs = ctx.editorContext != nullptr ? ctx.editorContext->Thumbnails() : nullptr;
                const LayerSet layers = ctx.scene != nullptr ? ResolveLayers(*ctx.scene) : LayerSet{};
                auto body = MakeRef<app::ToolPanelBody>(EditorRootAllocator());

                // The base, shown and never chosen: it is never painted, erasing reveals it.
                if (layers.count > 0)
                {
                    app::ToolPanelBody::Choice base;
                    base.tooltip = Format(u8"The base layer: {}. It is never painted; Erase reveals it.", AlbedoName(ctx.editorContext, layers.baseId));
                    base.icon = String(u8"Editor/Icons/texture.svg");
                    if (thumbs != nullptr && !layers.baseId.IsNil())
                        base.picture = [thumbs, baseId = layers.baseId]() { return thumbs->Get(baseId); };
                    body->AddPicture(u8"Base (erase to reveal)", Move(base));
                }

                // The layers, numbered from 1 as their keys are (and the Terrain page names them),
                // each its albedo's picture once there is one. With no terrain resolved, four.
                const u32 count = layers.count > 0 ? layers.count : 4u;
                Array<app::ToolPanelBody::Choice> choices;
                for (u32 i = 0; i < count; ++i)
                {
                    app::ToolPanelBody::Choice choice;
                    choice.label = Format(u8"{}", i + 1);
                    const Guid albedo = i < layers.ids.Size() ? layers.ids[i] : Guid{};
                    choice.tooltip = layers.count > 0 ? Format(u8"Layer {}: {}{}", i + 1, AlbedoName(ctx.editorContext, albedo), i < 9 ? Format(u8" ({})", i + 1) : String{})
                                                      : Format(u8"Layer {}", i + 1);
                    if (thumbs != nullptr && !albedo.IsNil())
                    {
                        choice.icon = String(u8"Editor/Icons/texture.svg");
                        choice.picture = [thumbs, albedo]() { return thumbs->Get(albedo); };
                    }
                    choices.PushBack(Move(choice));
                }
                body->AddChoices(u8"Layer", Move(choices), [t]() { return static_cast<i32>(t->PaletteIndex()); },
                                 [t](i32 i) { t->SetPaletteIndex(static_cast<u32>(i)); });
                Array<app::ToolPanelBody::Choice> modes;
                modes.PushBack(Word(u8"Paint", u8"Paint the layer"));
                modes.PushBack(Word(u8"Erase", u8"Fade what was painted, revealing the base layer (0)"));
                modes.PushBack(Word(u8"Smooth", u8"Feather painted seams without repainting either side (-)"));
                body->AddChoices(u8"Mode", Move(modes), [t]() { return t->IsEraser() ? 1 : t->IsSmooth() ? 2 : 0; },
                                 [t](i32 i)
                                 {
                                     if (i == 0)
                                         t->SetPaletteIndex(t->PaletteIndex()); // back to painting it
                                     else if (i == 1)
                                         t->SetEraser(true);
                                     else
                                         t->SetSmooth(true);
                                 });
                body->AddSlider(u8"Radius", 0.5, 128.0, 0.5, [t]() { return static_cast<f64>(t->Radius()); },
                                [t](f64 v) { t->SetRadius(static_cast<f32>(v)); });
                body->AddSlider(u8"Strength", 0.0, 1.0, 0.05, [t]() { return static_cast<f64>(t->Strength()); },
                                [t](f64 v) { t->SetStrength(static_cast<f32>(v)); });
                // Spacing: how close the stamps fall along a stroke, a share of the radius (low and
                // soft blends smoothly; high leaves dabs).
                body->AddSlider(u8"Spacing", 0.05, 1.0, 0.05, [t]() { return static_cast<f64>(t->Spacing()); },
                                [t](f64 v) { t->SetSpacing(static_cast<f32>(v)); });
                // Airbrush: stamps on a clock while held, building up where the brush rests.
                body->AddCheck(u8"Airbrush", [t]() { return t->IsAirbrush(); }, [t](bool on) { t->SetAirbrush(on); });
                body->AddHelp(u8"Drag to paint. 1 to 9 pick a layer, 0 erases, - smooths, Shift+wheel sizes the brush.");
                body->Sync();
                return body;
            }
        };
    }

    void RegisterTerrainToolPanels()
    {
        static SculptPanelProvider sculptPanel;
        static SplatPanelProvider splatPanel;
        static HolePanelProvider holePanel;
        ViewportToolPanelRegistry::Get().Register(&sculptPanel);
        ViewportToolPanelRegistry::Get().Register(&splatPanel);
        ViewportToolPanelRegistry::Get().Register(&holePanel);
    }

    void RegisterTerrainSceneTools(EditorContext& context)
    {
        RegisterTerrainViewportTools();
        RegisterTerrainToolPanels();
        if (context.Thumbnails() != nullptr)
            RegisterSplatmapThumbnailGenerator(*context.Thumbnails());
    }
}
