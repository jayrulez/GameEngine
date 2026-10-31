// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - the vegetation brushes' panels, made from the editor's tool panel kit like
// the terrain's, so all the footprint brushes look the same. Paint Vegetation has its plane
// (numbered from 1 like its keys, each tooltip naming what grows on that plane, with the chosen
// plane's growth noted under the row) and its mode (Paint, Erase, Smooth). Paint Props has its
// layer (by name) and its mode (Paint, Erase). Each also has its radius and other settings, and a
// line of help for its keys.

module;
#include "Core/Prelude.h"

module editor.vegetation;

import foundation.core;
import foundation.gui;
import foundation.scene;               // Scene, ComponentManagerBase, EntityHandle
import foundation.vegetation.resource; // VegetationMask (the plane count)
import foundation.vegetation;          // VegetationPlacement (which layers read a plane)
import engine.vegetation;              // TerrainVegetationComponent, TerrainVegetationComponentManager
import editor.core;
import editor.app;                     // the tool panel registry and its kit
import editor.viewporttools;           // IViewportTool, ViewportToolHostContext

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;

    namespace
    {
        engine::vegetation::TerrainVegetationComponentManager* ManagerOf(scene::Scene& sc)
        {
            return static_cast<engine::vegetation::TerrainVegetationComponentManager*>(
                sc.FindManagerByComponentType(TypeOf<engine::vegetation::TerrainVegetationComponent>()));
        }

        // The plane count of the scene's first resolved vegetation mask (0 when none resolves).
        u32 ResolvePlaneCount(scene::Scene& sc)
        {
            u32 planes = 0;
            if (auto* mgr = ManagerOf(sc))
                mgr->ForEach([&](engine::vegetation::TerrainVegetationComponent& c, scene::EntityHandle)
                             {
                                 if (planes == 0 && c.mask.Get() != nullptr && !c.mask.Get()->IsEmpty())
                                     planes = c.mask.Get()->PlaneCount();
                             });
            return planes;
        }

        // The procedural layers that read each mask plane ("Grass, Flowers"; empty for none). The
        // brush paints a plane, and the layers reading it are what give the plane meaning.
        Array<String> ResolvePlaneReaders(scene::Scene& sc, u32 planeCount)
        {
            Array<String> readers;
            for (u32 plane = 0; plane < planeCount; ++plane)
                readers.PushBack(String{});
            if (auto* mgr = ManagerOf(sc))
                mgr->ForEach([&](engine::vegetation::TerrainVegetationComponent& c, scene::EntityHandle)
                             {
                                 for (usize i = 0; i < c.proceduralLayers.Size(); ++i)
                                 {
                                     const engine::vegetation::ProceduralVegetationLayer& layer = c.proceduralLayers[i];
                                     using foundation::vegetation::VegetationPlacement;
                                     const bool reads = layer.placement == VegetationPlacement::Mask ||
                                                        layer.placement == VegetationPlacement::SplatTimesMask;
                                     if (!reads || layer.maskPlane >= planeCount)
                                         continue;
                                     String& line = readers[layer.maskPlane];
                                     const String name = layer.name.IsEmpty() ? Format(u8"Procedural layer {}", i + 1) : layer.name;
                                     line = line.IsEmpty() ? name : Format(u8"{}, {}", line.AsView(), name.AsView());
                                 }
                             });
            return readers;
        }

        // The prop layers of the scene's first vegetation component that has any, and whether
        // each one's mesh resolves (a layer without a mesh still places props, but nothing draws).
        struct LayerNames
        {
            Array<String> names;
            Array<bool> hasMesh;
        };
        LayerNames ResolveLayerNames(scene::Scene& sc)
        {
            LayerNames out;
            bool taken = false;
            if (auto* mgr = ManagerOf(sc))
                mgr->ForEach([&](engine::vegetation::TerrainVegetationComponent& c, scene::EntityHandle)
                             {
                                 if (taken || c.propLayers.IsEmpty())
                                     return;
                                 taken = true;
                                 for (usize i = 0; i < c.propLayers.Size(); ++i)
                                 {
                                     out.names.PushBack(c.propLayers[i].name.IsEmpty() ? Format(u8"Prop layer {}", i + 1) : c.propLayers[i].name);
                                     out.hasMesh.PushBack(c.propLayers[i].mesh.Get() != nullptr);
                                 }
                             });
            return out;
        }

        app::ToolPanelBody::Choice Word(StringView label, StringView tooltip)
        {
            app::ToolPanelBody::Choice choice;
            choice.label = String(label);
            choice.tooltip = String(tooltip);
            return choice;
        }

        class VegetationPaintPanelProvider final : public IViewportToolPanelProvider
        {
        public:
            [[nodiscard]] StringView ToolId() const override { return u8"vegetation.paint"; }
            [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext& ctx) override
            {
                VegetationPaintTool* t = static_cast<VegetationPaintTool*>(&tool);
                const u32 resolved = ctx.scene != nullptr ? ResolvePlaneCount(*ctx.scene) : 0u;
                const u32 count = resolved > 0 ? resolved : 4u;
                const Array<String> readers = ctx.scene != nullptr ? ResolvePlaneReaders(*ctx.scene, count) : Array<String>{};
                auto body = MakeRef<app::ToolPanelBody>(EditorRootAllocator());

                // The planes, numbered from 1 like their keys, each saying what grows on it.
                Array<app::ToolPanelBody::Choice> planes;
                for (u32 i = 0; i < count; ++i)
                {
                    const String grows = i < readers.Size() && !readers[i].IsEmpty() ? Format(u8"grows {}", readers[i]) : String(u8"nothing reads it yet");
                    planes.PushBack(Word(Format(u8"{}", i + 1).AsView(), Format(u8"Plane {}: {}{}", i + 1, grows, i < 9 ? Format(u8" ({})", i + 1) : String{}).AsView()));
                }
                body->AddChoices(u8"Plane", Move(planes), [t]() { return static_cast<i32>(t->Plane()); },
                                 [t](i32 i) { t->SetPlane(static_cast<u32>(i)); });
                // What a stroke grows: the layers reading the chosen plane.
                body->AddNote([t, readers]() -> String
                              {
                                  const u32 plane = t->Plane();
                                  if (plane < readers.Size() && !readers[plane].IsEmpty())
                                      return Format(u8"Grows {}.", readers[plane]);
                                  return String(u8"No procedural layer reads this plane: a layer with Mask placement names it.");
                              });
                Array<app::ToolPanelBody::Choice> modes;
                modes.PushBack(Word(u8"Paint", u8"Paint the plane"));
                modes.PushBack(Word(u8"Erase", u8"Clear the plane under the brush (0)"));
                modes.PushBack(Word(u8"Smooth", u8"Feather its edges without repainting (-)"));
                body->AddChoices(u8"Mode", Move(modes), [t]() { return t->IsSmooth() ? 2 : t->IsEraser() ? 1 : 0; },
                                 [t](i32 i)
                                 {
                                     t->SetEraser(i == 1);
                                     t->SetSmooth(i == 2);
                                 });
                body->AddSlider(u8"Radius", 0.5, 128.0, 0.5, [t]() { return static_cast<f64>(t->Radius()); },
                                [t](f64 v) { t->SetRadius(static_cast<f32>(v)); });
                body->AddSlider(u8"Strength", 0.0, 1.0, 0.05, [t]() { return static_cast<f64>(t->Strength()); },
                                [t](f64 v) { t->SetStrength(static_cast<f32>(v)); });
                body->AddSlider(u8"Spacing", 0.05, 1.0, 0.05, [t]() { return static_cast<f64>(t->Spacing()); },
                                [t](f64 v) { t->SetSpacing(static_cast<f32>(v)); });
                body->AddCheck(u8"Airbrush", [t]() { return t->IsAirbrush(); }, [t](bool on) { t->SetAirbrush(on); });
                body->AddHelp(u8"Drag to paint. 1 to 9 pick a plane, 0 erases, - smooths, Shift+wheel sizes the brush.");
                body->Sync();
                return body;
            }
        };

        class VegetationScatterPanelProvider final : public IViewportToolPanelProvider
        {
        public:
            [[nodiscard]] StringView ToolId() const override { return u8"vegetation.scatter"; }
            [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext& ctx) override
            {
                VegetationScatterTool* t = static_cast<VegetationScatterTool*>(&tool);
                const LayerNames layers = ctx.scene != nullptr ? ResolveLayerNames(*ctx.scene) : LayerNames{};
                auto body = MakeRef<app::ToolPanelBody>(EditorRootAllocator());
                Array<app::ToolPanelBody::Choice> choices;
                for (usize i = 0; i < layers.names.Size(); ++i)
                    choices.PushBack(Word(layers.names[i].AsView(),
                                          layers.hasMesh[i] ? Format(u8"Layer {}{}", i + 1, i < 9 ? Format(u8" ({})", i + 1) : String{}).AsView()
                                                            : StringView(u8"Its mesh does not resolve: props place, but nothing draws")));
                if (!choices.IsEmpty())
                    body->AddChoices(u8"Layer", Move(choices), [t]() { return static_cast<i32>(t->Layer()); },
                                     [t](i32 i) { t->SetLayer(static_cast<u32>(i)); });
                else
                    body->AddHelp(u8"No prop layers here: add one to the Terrain Vegetation component.");
                Array<app::ToolPanelBody::Choice> modes;
                modes.PushBack(Word(u8"Paint", u8"Place props"));
                modes.PushBack(Word(u8"Erase", u8"Remove the layer's props under the brush (0)"));
                body->AddChoices(u8"Mode", Move(modes), [t]() { return t->IsEraser() ? 1 : 0; }, [t](i32 i) { t->SetEraser(i == 1); });
                body->AddSlider(u8"Radius", 0.5, 128.0, 0.5, [t]() { return static_cast<f64>(t->Radius()); },
                                [t](f64 v) { t->SetRadius(static_cast<f32>(v)); });
                body->AddSlider(u8"Density", 0.0, 64.0, 0.05, [t]() { return static_cast<f64>(t->Density()); },
                                [t](f64 v) { t->SetDensity(static_cast<f32>(v)); });
                body->AddSlider(u8"Strength", 0.0, 1.0, 0.05, [t]() { return static_cast<f64>(t->Strength()); },
                                [t](f64 v) { t->SetStrength(static_cast<f32>(v)); });
                body->AddSlider(u8"Prop Spacing", 0.0, 8.0, 0.1, [t]() { return static_cast<f64>(t->Spacing()); },
                                [t](f64 v) { t->SetSpacing(static_cast<f32>(v)); });
                body->AddHelp(u8"Drag to place. Density is props per square metre; prop spacing keeps props apart, in their mesh's radii. "
                              u8"1 to 9 pick a layer, 0 erases, Shift+wheel sizes the brush.");
                body->Sync();
                return body;
            }
        };
    }

    void RegisterVegetationToolPanels()
    {
        static VegetationPaintPanelProvider paintPanel;
        static VegetationScatterPanelProvider scatterPanel;
        ViewportToolPanelRegistry::Get().Register(&paintPanel);
        ViewportToolPanelRegistry::Get().Register(&scatterPanel);
    }
}
