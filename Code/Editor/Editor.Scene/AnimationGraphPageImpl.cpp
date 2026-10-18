// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :animation_graph_page partition (implementation).
//
// The state-machine / blend-tree authoring tool (see AnimationGraphPage.cppm for the overview).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.settings; // per-project editor-settings store (preview skeleton/mesh prefs)

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.geometry;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace animation = foundation::animation;
namespace core = foundation::core;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene; // :actions declares it too; MSVC does not see a partition's
namespace ui = foundation::ui;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

namespace editor
{
    namespace
    {

        using Page = AnimationGraphEditorPage*;

        // Node palette: node 0 = "Any State", node 1+i = state i.
        constexpr i32 kAnyStateNode = 0;
        [[nodiscard]] constexpr i32 StateToNode(i32 state) noexcept { return state + 1; }
        [[nodiscard]] constexpr i32 NodeToState(i32 node) noexcept { return node - 1; }

        [[nodiscard]] core::Color HeaderPlain() { return core::Color{0.27f, 0.40f, 0.60f, 1.0f}; }
        [[nodiscard]] core::Color HeaderDefault() { return core::Color{0.80f, 0.52f, 0.18f, 1.0f}; }
        [[nodiscard]] core::Color HeaderAny() { return core::Color{0.30f, 0.55f, 0.50f, 1.0f}; }

        [[nodiscard]] StringView NodeKindLabel(u8 kind)
        {
            switch (kind)
            {
            case 1:
                return u8"Blend Tree 1D";
            case 2:
                return u8"Blend Tree 2D";
            default:
                return u8"Clip";
            }
        }

        void Add(ui::toolkit::PropertyGrid& g, RefPtr<ui::toolkit::PropertyEditor> e)
        {
            g.AddProperty(Move(e));
        }

        void RowFloat(ui::toolkit::PropertyGrid& g, StringView name, f32* field, StringView cat,
                      Page page, f64 mn = -1e9, f64 mx = 1e9, f64 step = 0.05)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::FloatEditor>(
                           editor::EditorRootAllocator(), name, static_cast<f64>(*field), mn, mx, step, 3,
                           Function<void(f64)>{[field, page, key](f64 v)
                                               {
                                                   *field = static_cast<f32>(v);
                                                   page->CommitEdit(key.AsView());
                                               }},
                           cat)
                           .Get()));
        }
        void RowInt(ui::toolkit::PropertyGrid& g, StringView name, i32* field, StringView cat,
                    Page page, i64 mn = -1000000, i64 mx = 1000000)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::IntEditor>(
                           editor::EditorRootAllocator(), name, static_cast<i64>(*field), mn, mx,
                           Function<void(i64)>{[field, page, key](i64 v)
                                               {
                                                   *field = static_cast<i32>(v);
                                                   page->CommitEdit(key.AsView());
                                               }},
                           cat)
                           .Get()));
        }
        void RowBool(ui::toolkit::PropertyGrid& g, StringView name, bool* field, StringView cat,
                     Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::BoolEditor>(
                           editor::EditorRootAllocator(), name, *field,
                           Function<void(bool)>{[field, page, key](bool v)
                                                {
                                                    *field = v;
                                                    page->CommitEdit(key.AsView());
                                                }},
                           cat)
                           .Get()));
        }
        void RowEnum(ui::toolkit::PropertyGrid& g, StringView name, i32 value,
                     Span<const StringView> items, Function<void(i32)> setter, StringView cat)
        {
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::EnumEditor>(editor::EditorRootAllocator(), name, value, items,
                                                        Move(setter), cat)
                           .Get()));
        }
        void RowButton(ui::toolkit::PropertyGrid& g, StringView name, StringView cat,
                       Function<void()> action)
        {
            Add(g,
                RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::ButtonEditor>(editor::EditorRootAllocator(), name, Move(action), cat)
                        .Get()));
        }
        void RowString(ui::toolkit::PropertyGrid& g, StringView name, String* field, StringView cat,
                       Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::StringEditor>(
                           editor::EditorRootAllocator(), name, field->AsView(),
                           Function<void(StringView)>{[field, page, key](StringView v)
                                                      {
                                                          *field = String(v);
                                                          page->CommitEdit(key.AsView());
                                                      }},
                           cat)
                           .Get()));
        }

        // Parameter dropdown: "(none)" + every graph parameter name. Selected value = index + 1.
        void RowParamPick(ui::toolkit::PropertyGrid& g, StringView name,
                          const animation::AnimationGraphSource& source, i32 current,
                          Function<void(i32)> setIndex, StringView cat)
        {
            Array<String> owned;
            owned.PushBack(String(u8"(none)"));
            for (const String& p : source.paramNames)
            {
                owned.PushBack(String(p.AsView()));
            }
            Array<StringView> items;
            for (const String& s : owned)
            {
                items.PushBack(s.AsView());
            }
            RowEnum(g, name, current + 1, Span<const StringView>{items.Data(), items.Size()},
                    Function<void(i32)>{[setIndex = Move(setIndex)](i32 v) { setIndex(v - 1); }},
                    cat);
        }

        // A section header's remove icon.
        RefPtr<ui::View> RemoveIcon(IAllocator& allocator, StringView tooltip,
                                    Function<void()> action)
        {
            auto remove =
                MakeRef<ui::IconButton>(allocator, app::EditorIcons::Get().remove.Get());
            remove->TooltipText = String(tooltip);
            remove->OnClick.Add([action = Move(action)](ui::ButtonBase*) { action(); });
            return RefPtr<ui::View>(remove.Get());
        }
    } // namespace

    // A left panel row whose right click opens its context menu (Delete, when it has one).
    class GraphListRow final : public ui::Button
    {
    public:
        GraphListRow(StringView text, Function<void()> onDelete)
            : ui::Button(text), m_onDelete(Move(onDelete))
        {
        }

        void OnMouseDown(ui::MouseEventArgs& e) override
        {
            if (e.Button != ui::MouseButton::Right || !m_onDelete || Context == nullptr)
            {
                ui::Button::OnMouseDown(e);
                return;
            }
            auto menu = MakeRef<ui::ContextMenu>(MemoryAllocator());
            GraphListRow* self = this;
            menu->AddItem(u8"Delete", [self]() { self->m_onDelete(); });
            const Float2 at = LocalToScreen(Float2{e.X, e.Y});
            menu->Show(Context, at.x, at.y);
            e.Handled = true;
        }

    private:
        Function<void()> m_onDelete;
    };

    // ============================ Construction ==============================================

    // Per-asset graph-preview prefs: {graphGuid -> (skeleton guid, skinned-mesh guid)} - a section
    // in the per-project editor-settings store, so a reopened graph editor restores its preview rig.
    struct GraphPreviewPref
    {
        Guid asset;
        Guid skeleton;
        Guid mesh;
        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            ar.Key("skeleton");
            ar.GuidValue(skeleton);
            ar.Key("mesh");
            ar.GuidValue(mesh);
        }
    };
    inline void Serialize(ISerializer& ar, GraphPreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }
    class GraphPreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(GraphPreviewSettings, ISerializable)
    public:
        Array<GraphPreviewPref> prefs;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "prefs", prefs);
        }
    };

    AnimationGraphEditorPage::AnimationGraphEditorPage(EditorContext& context,
                                                       runtime::IApplicationHost& host,
                                                       ui::runtime::UIHost& uiHost,
                                                       foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // Shared preview substrate (viewport + preview scene + orbit camera + render loop).
        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"animgraph.preview");
        m_preview->SetClearColor(Color{0.248f, 0.248f, 0.293f, 1.0f});
        m_preview->Camera().position = Float3{0.0f, 1.4f, 3.2f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});

        // Preview content: a sun so a picked skinned mesh is lit (the wireframe needs none) + the
        // optional mesh entity (its MeshComponent gets skinning matrices fed each frame; no mesh
        // bound until the user picks one).
        if (scene::Scene* scenePtr = m_preview->Scene())
        {
            const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
            Transform st;
            st.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                          Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
            scenePtr->SetLocalTransform(sun, st);
            if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
            {
                lights->Add(sun).castsShadows = false;
            }
            m_meshEntity = scenePtr->CreateEntity(u8"PreviewMesh");
            if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            {
                meshes->Add(m_meshEntity);
            }
        }

        SetInstanceId(instance.Id());
        Provide<IPlaybackPage>(*this);

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<pipeline::AnimationGraphAsset>(
            Cast<pipeline::AnimationGraphAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            LOG_ERROR(u8"Editor",
                               u8"animation graph '{}' failed to read - page opens empty", m_title);
        }
        else
        {
            SyncLayoutArrays();
        }
        m_undoBaseline = SnapshotAsset();

        // ---- center: the state-machine canvas ----
        m_canvas = MakeRef<ui::toolkit::NodeGraphCanvas>(Allocator());
        m_canvas->EdgeStyle = ui::toolkit::ConnectionStyle::StraightNodeToNode;
        {
            AnimationGraphEditorPage* self = this;
            m_canvas->OnNodeMoved.Add(
                [self](i32 node)
                {
                    if (self->m_syncingCanvas || self->m_asset.Get() == nullptr)
                    {
                        return;
                    }
                    ui::toolkit::NodeGraphNode* n = self->m_canvas->GetNode(node);
                    if (n == nullptr)
                    {
                        return;
                    }
                    self->SyncLayoutArrays();
                    const usize layer = static_cast<usize>(self->m_selectedLayer);
                    if (node == kAnyStateNode)
                    {
                        self->m_asset->layerAnyStatePositions[layer] = n->Position;
                    }
                    else
                    {
                        Array<Float2>& positions = self->m_asset->layerStatePositions[layer];
                        const i32 state = NodeToState(node);
                        if (state >= 0 && static_cast<usize>(state) < positions.Size())
                        {
                            positions[static_cast<usize>(state)] = n->Position;
                        }
                    }
                    self->CommitEdit(u8"move-node");
                });
            m_canvas->OnCanvasContextMenu.Add([self](f32 x, f32 y) { self->ShowCanvasMenu(x, y); });
            m_canvas->OnNodeContextMenu.Add([self](i32 node) { self->ShowNodeMenu(node); });
            m_canvas->OnConnectionContextMenu.Add([self](i32 conn)
                                                  { self->ShowConnectionMenu(conn); });
            m_canvas->OnNodeDoubleClicked.Add(
                [self](i32 node)
                {
                    if (node != kAnyStateNode)
                    {
                        self->Select(GraphSel{GraphSelKind::State, self->m_selectedLayer,
                                              NodeToState(node)});
                    }
                });
            m_canvas->OnNodeLinkRequested.Add(
                [self](i32 sourceNode, i32 targetNode)
                {
                    // Transitions cannot LAND on Any State.
                    if (targetNode == kAnyStateNode || self->m_asset.Get() == nullptr)
                    {
                        return;
                    }
                    const i32 layerIndex = self->m_selectedLayer;
                    const i32 src = (sourceNode == kAnyStateNode) ? -1 : NodeToState(sourceNode);
                    const i32 dst = NodeToState(targetNode);
                    self->QueueStructural(
                        u8"add-transition",
                        Function<void()>{
                            [self, layerIndex, src, dst]()
                            {
                                animation::AnimationGraphSource& source = self->m_asset->source;
                                if (layerIndex < 0 ||
                                    static_cast<usize>(layerIndex) >= source.layers.Size())
                                {
                                    return;
                                }
                                animation::GraphTransitionData t;
                                t.src = src;
                                t.dst = dst;
                                source.layers[static_cast<usize>(layerIndex)]
                                    .transitions.PushBack(Move(t));
                            }},
                        GraphSel{GraphSelKind::Transition, layerIndex, -2 /*"last" - resolved
                                                                            in the apply*/});
                });
            m_canvas->OnConnectionDeleting.Add(
                [self](i32 connectionIndex)
                {
                    if (self->m_syncingCanvas)
                    {
                        return;
                    }
                    const i32 layerIndex = self->m_selectedLayer;
                    self->QueueStructural(
                        u8"del-transition",
                        Function<void()>{
                            [self, layerIndex, connectionIndex]()
                            {
                                animation::AnimationGraphSource& source = self->m_asset->source;
                                if (layerIndex < 0 ||
                                    static_cast<usize>(layerIndex) >= source.layers.Size())
                                {
                                    return;
                                }
                                Array<animation::GraphTransitionData>& transitions =
                                    source.layers[static_cast<usize>(layerIndex)].transitions;
                                if (connectionIndex >= 0 &&
                                    static_cast<usize>(connectionIndex) < transitions.Size())
                                {
                                    transitions.RemoveAt(static_cast<usize>(connectionIndex));
                                }
                            }},
                        GraphSel{});
                });
            m_canvas->OnSelectionChanged.Add(
                [self]()
                {
                    if (self->m_syncingCanvas)
                    {
                        return;
                    }
                    Array<i32> nodes;
                    self->m_canvas->GetSelectedNodes(nodes);
                    GraphSel derived{}; // None: empty-space click / cleared box select
                    if (!nodes.IsEmpty())
                    {
                        if (nodes[0] != kAnyStateNode)
                        {
                            derived = GraphSel{GraphSelKind::State, self->m_selectedLayer,
                                               NodeToState(nodes[0])};
                        }
                        // Any State stays None - the pseudo-node has nothing to inspect.
                    }
                    else
                    {
                        for (i32 i = 0; i < self->m_canvas->ConnectionCount(); ++i)
                        {
                            if (self->m_canvas->GetConnection(i).IsSelected)
                            {
                                derived =
                                    GraphSel{GraphSelKind::Transition, self->m_selectedLayer, i};
                                break;
                            }
                        }
                    }
                    // Deselect must CLEAR the inspector (user report): always push the derived
                    // selection, including the None case.
                    if (!(derived == self->m_selected))
                    {
                        self->Select(derived);
                    }
                });
        }

        // ---- left: layers + parameters ----
        m_leftRows = MakeRef<ui::FlexLayout>(Allocator());
        m_leftRows->Direction = ui::Orientation::Vertical;
        m_leftRows->Spacing = 2.0f;
        m_leftRows->Padding = ui::Thickness{6, 6};
        auto leftScroll = MakeRef<ui::ScrollView>(Allocator());
        leftScroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        leftScroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            leftScroll->AddView(m_leftRows.Get(), lp);
        }

        // ---- right: inspector ----
        m_grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        m_inspectorTitle = MakeRef<ui::Label>(Allocator());
        m_inspectorTitle->FontSize.SetValue(Optional<f32>{12.0f});
        auto inspectorColumn = MakeRef<ui::FlexLayout>(Allocator());
        inspectorColumn->Direction = ui::Orientation::Vertical;
        inspectorColumn->Spacing = 4.0f;
        inspectorColumn->Padding = ui::Thickness{6, 4};
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_inspectorTitle.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_grid.Get(), grow);
        }

        // ---- center-bottom: the live preview strip (transport + wireframe viewport) ----
        auto transport = MakeRef<ui::FlexLayout>(Allocator());
        transport->Direction = ui::Orientation::Horizontal;
        transport->Spacing = 6.0f;
        transport->Padding = ui::Thickness{6, 4};
        {
            AnimationGraphEditorPage* self = this;
            // The preview rig: compact asset slots (pick, drop and clear are one assignment).
            const StringView skeletonTypes[] = {u8"SkeletonAsset"};
            m_skeletonSlot = MakeRef<app::CompactAssetSlot>(
                Allocator(), StringView(u8"Skeleton"), Span<const StringView>{skeletonTypes, 1});
            m_skeletonSlot->Editor().BindAsset(*m_context,
                                               [self]() { return self->m_skeletonGuid; },
                                               [self](const Guid& picked)
                                               {
                                                   self->SetPreviewSkeleton(picked);
                                                   self->RebuildPreviewGraph();
                                                   self->SavePreviewPref();
                                               });
            m_skeletonSlot->Build();
            const StringView meshTypes[] = {u8"SkinnedMeshAsset"};
            m_meshSlot = MakeRef<app::CompactAssetSlot>(Allocator(), StringView(u8"Mesh"),
                                                        Span<const StringView>{meshTypes, 1});
            m_meshSlot->Editor().BindAsset(*m_context, [self]() { return self->m_previewMeshId; },
                                           [self](const Guid& picked)
                                           {
                                               self->SetPreviewMesh(picked);
                                               self->SavePreviewPref();
                                           });
            m_meshSlot->Build();
            {
                ui::LayoutStyle slot;
                slot.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(200.0f));
                transport->AddView(m_skeletonSlot.Get(), slot);
                transport->AddView(m_meshSlot.Get(), slot);
            }
            m_previewStatus = MakeRef<ui::Label>(Allocator());
            m_previewStatus->FontSize.SetValue(Optional<f32>{12.0f});
            m_previewStatus->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            transport->AddView(m_previewStatus.Get(), grow);
        }
        auto previewColumn = MakeRef<ui::FlexLayout>(Allocator());
        previewColumn->Direction = ui::Orientation::Vertical;
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            previewColumn->AddView(transport.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            previewColumn->AddView(m_preview->View(), grow);
        }
        auto centerSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        centerSplit->Orientation = ui::Orientation::Vertical;
        centerSplit->SetSplitRatio(0.62f);
        centerSplit->SetPanes(m_canvas.Get(), previewColumn.Get());

        auto leftSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        leftSplit->SetSplitRatio(0.18f);
        leftSplit->SetPanes(leftScroll.Get(), centerSplit.Get());
        auto rightSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        rightSplit->SetSplitRatio(0.74f);
        rightSplit->SetPanes(leftSplit.Get(), inspectorColumn.Get());
        // The page toolbar: the standard set, playback, then the preview's bones and mesh toggles.
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_toolbar->AddPlayback();
        m_toolbar->AddSeparator();
        {
            AnimationGraphEditorPage* self = this;
            ui::toolkit::ToolbarToggle* bones = m_toolbar->AddToggle(u8"Bones");
            bones->SetIsChecked(m_showSkeleton);
            bones->OnCheckedChanged.Add([self](ui::toolkit::ToolbarToggle*, bool on)
                                        { self->m_showSkeleton = on; });
            ui::toolkit::ToolbarToggle* mesh = m_toolbar->AddToggle(u8"Mesh");
            mesh->SetIsChecked(m_showMesh);
            mesh->OnCheckedChanged.Add([self](ui::toolkit::ToolbarToggle*, bool on)
                                       { self->m_showMesh = on; });
            ui::toolkit::ToolbarToggle* travel = m_toolbar->AddToggle(u8"Travel");
            travel->SetIsChecked(m_showTravel);
            travel->OnCheckedChanged.Add([self](ui::toolkit::ToolbarToggle*, bool on)
                                         {
                                             self->m_showTravel = on;
                                             self->m_travelPosition = Float3{};
                                             self->m_travelYaw = 0.0f;
                                         });
        }
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *rightSplit);

        RebuildLeftPanel();
        RebuildCanvas();
        LoadPreviewPref(); // restore skeleton + mesh before the player builds below
        RebuildPreviewGraph();
        Select(GraphSel{GraphSelKind::Layer, 0, 0});
    }

    void AnimationGraphEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        const GraphPreviewSettings* section = store->Find<GraphPreviewSettings>();
        if (section == nullptr)
        {
            return;
        }
        for (const GraphPreviewPref& p : section->prefs)
        {
            if (p.asset != InstanceId())
            {
                continue;
            }
            SetPreviewSkeleton(p.skeleton);
            SetPreviewMesh(p.mesh);
            return;
        }
    }

    void AnimationGraphEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        GraphPreviewSettings& section = store->Section<GraphPreviewSettings>();
        for (GraphPreviewPref& p : section.prefs)
        {
            if (p.asset == InstanceId())
            {
                p.skeleton = m_skeletonGuid;
                p.mesh = m_previewMeshId;
                return;
            }
        }
        section.prefs.PushBack(GraphPreviewPref{InstanceId(), m_skeletonGuid, m_previewMeshId});
    }

    // ============================ Model helpers =============================================

    animation::GraphLayerData* AnimationGraphEditorPage::SelectedLayer()
    {
        if (m_asset.Get() == nullptr)
        {
            return nullptr;
        }
        Array<animation::GraphLayerData>& layers = m_asset->source.layers;
        if (m_selectedLayer < 0 || static_cast<usize>(m_selectedLayer) >= layers.Size())
        {
            return nullptr;
        }
        return &layers[static_cast<usize>(m_selectedLayer)];
    }

    void AnimationGraphEditorPage::SyncLayoutArrays()
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        const Array<animation::GraphLayerData>& layers = m_asset->source.layers;
        while (m_asset->layerStatePositions.Size() < layers.Size())
        {
            m_asset->layerStatePositions.PushBack(Array<Float2>{});
        }
        while (m_asset->layerStatePositions.Size() > layers.Size())
        {
            m_asset->layerStatePositions.RemoveAt(m_asset->layerStatePositions.Size() - 1);
        }
        while (m_asset->layerAnyStatePositions.Size() < layers.Size())
        {
            m_asset->layerAnyStatePositions.PushBack(Float2{40.0f, 40.0f});
        }
        while (m_asset->layerAnyStatePositions.Size() > layers.Size())
        {
            m_asset->layerAnyStatePositions.RemoveAt(m_asset->layerAnyStatePositions.Size() - 1);
        }
        for (usize l = 0; l < layers.Size(); ++l)
        {
            Array<Float2>& positions = m_asset->layerStatePositions[l];
            // New states fan out to the right of the last known position.
            while (positions.Size() < layers[l].states.Size())
            {
                const f32 x = 240.0f + 60.0f * static_cast<f32>(positions.Size() % 5);
                const f32 y = 90.0f + 70.0f * static_cast<f32>(positions.Size() / 5);
                positions.PushBack(Float2{x, y});
            }
            while (positions.Size() > layers[l].states.Size())
            {
                positions.RemoveAt(positions.Size() - 1);
            }
        }
    }

    // ============================ Canvas sync ===============================================

    void AnimationGraphEditorPage::RebuildCanvas()
    {
        m_syncingCanvas = true;
        m_canvas->Clear();
        animation::GraphLayerData* layer = SelectedLayer();
        if (layer != nullptr)
        {
            SyncLayoutArrays();
            const usize layerIdx = static_cast<usize>(m_selectedLayer);

            // Node 0: the Any State pseudo-node.
            {
                auto node = MakeUnique<ui::toolkit::NodeGraphNode>(Allocator());
                node->Title = String(u8"Any State");
                node->Position = m_asset->layerAnyStatePositions[layerIdx];
                node->Size = Float2{140.0f, 44.0f};
                node->HeaderColor = HeaderAny();
                node->IsDeletable = false;
                (void)m_canvas->AddNode(Move(node));
            }
            // Node 1+i: the states.
            for (usize i = 0; i < layer->states.Size(); ++i)
            {
                const animation::GraphStateData& s = layer->states[i];
                auto node = MakeUnique<ui::toolkit::NodeGraphNode>(Allocator());
                node->Title = String(s.name.AsView());
                node->Subtitle = String(NodeKindLabel(s.node.kind));
                node->Position = m_asset->layerStatePositions[layerIdx][i];
                node->Size = Float2{150.0f, 46.0f};
                node->HeaderColor =
                    (static_cast<i32>(i) == layer->defaultState) ? HeaderDefault() : HeaderPlain();
                node->IsDeletable = false; // deletion goes through the context menu (source-first)
                (void)m_canvas->AddNode(Move(node));
            }
            // Connections in TRANSITION ORDER: connection index == transition index.
            for (usize t = 0; t < layer->transitions.Size(); ++t)
            {
                const animation::GraphTransitionData& tr = layer->transitions[t];
                ui::toolkit::NodeGraphConnection conn;
                conn.SourceNodeIndex = (tr.src < 0) ? kAnyStateNode : StateToNode(tr.src);
                conn.DestNodeIndex = StateToNode(tr.dst);
                (void)m_canvas->AddConnection(conn);
            }
        }
        m_syncingCanvas = false;
    }

    void AnimationGraphEditorPage::RebuildLeftPanel()
    {
        m_leftRows->RemoveAllViews();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        AnimationGraphEditorPage* self = this;
        // A list row: a click selects it, and a right click offers Delete when `onDelete` is set.
        auto addRow = [&](StringView text, Function<void()> onClick, bool emphasized,
                          Function<void()> onDelete)
        {
            auto button = MakeRef<GraphListRow>(Allocator(), text, Move(onDelete));
            if (emphasized)
            {
                button->AddClass(u8"accent");
            }
            button->OnClick.Add(
                [onClick = Move(onClick)](ui::ButtonBase*) mutable
                {
                    if (onClick)
                    {
                        onClick();
                    }
                });
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(24.0f));
            m_leftRows->AddView(button.Get(), lp);
        };
        // A list's header: its title, and its add icon on the right.
        auto addHeader = [&](StringView text, StringView addTooltip, Function<void()> onAdd)
        {
            auto header = MakeRef<app::ListHeader>(Allocator(), text, addTooltip, Move(onAdd));
            m_leftRows->AddView(header.Get(), app::ListHeader::RowStyle());
        };

        addHeader(u8"Layers", u8"Add layer",
                  [self]()
                  {
                      self->QueueStructural(
                          u8"add-layer",
                          Function<void()>{
                              [self]()
                              {
                                  animation::GraphLayerData layer;
                                  layer.name =
                                      Format(u8"Layer {}", self->m_asset->source.layers.Size());
                                  self->m_asset->source.layers.PushBack(Move(layer));
                                  self->m_selectedLayer =
                                      static_cast<i32>(self->m_asset->source.layers.Size()) - 1;
                              }},
                          GraphSel{GraphSelKind::Layer, 0, 0});
                  });
        const animation::AnimationGraphSource& source = m_asset->source;
        for (usize l = 0; l < source.layers.Size(); ++l)
        {
            const i32 layerIndex = static_cast<i32>(l);
            String text(source.layers[l].name.AsView());
            if (layerIndex == m_selectedLayer)
            {
                text.Append(u8"  <");
            }
            addRow(text.AsView(),
                   Function<void()>{[self, layerIndex]()
                                    {
                                        self->m_selectedLayer = layerIndex;
                                        self->Select(
                                            GraphSel{GraphSelKind::Layer, layerIndex, layerIndex});
                                        ui::UIContext* ctx = self->Ctx();
                                        if (ctx != nullptr)
                                        {
                                            AnimationGraphEditorPage* page = self;
                                            ctx->MutationQueueRef().QueueAction(
                                                Function<void()>{[page]()
                                                                 {
                                                                     page->RebuildLeftPanel();
                                                                     page->RebuildCanvas();
                                                                 }});
                                        }
                                    }},
                   layerIndex == m_selectedLayer,
                   source.layers.Size() > 1
                       ? Function<void()>{[self, layerIndex]() { self->DeleteLayer(layerIndex); }}
                       : Function<void()>{});
        }

        addHeader(u8"Parameters", u8"Add parameter",
                  [self]()
                  {
                      self->QueueStructural(
                          u8"add-param",
                          Function<void()>{[self]()
                                           {
                                               animation::AnimationGraphSource& s =
                                                   self->m_asset->source;
                                               s.paramNames.PushBack(
                                                   Format(u8"Param{}", s.paramNames.Size()));
                                               s.paramTypes.PushBack(0); // Float
                                               s.paramFloats.PushBack(0.0f);
                                               s.paramInts.PushBack(0);
                                               s.paramBools.PushBack(0);
                                           }},
                          GraphSel{GraphSelKind::Parameter, 0, -2 /*last*/});
                  });
        for (usize p = 0; p < source.paramNames.Size(); ++p)
        {
            const i32 paramIndex = static_cast<i32>(p);
            addRow(source.paramNames[p].AsView(),
                   Function<void()>{
                       [self, paramIndex]()
                       { self->Select(GraphSel{GraphSelKind::Parameter, 0, paramIndex}); }},
                   m_selected.kind == GraphSelKind::Parameter && m_selected.index == paramIndex,
                   Function<void()>{[self, paramIndex]() { self->DeleteParam(paramIndex); }});
        }
    }

    void AnimationGraphEditorPage::DeleteLayer(i32 layerIndex)
    {
        AnimationGraphEditorPage* self = this;
        QueueStructural(u8"del-layer",
                        Function<void()>{[self, layerIndex]()
                                         {
                                             animation::AnimationGraphSource& s =
                                                 self->m_asset->source;
                                             const usize at = static_cast<usize>(layerIndex);
                                             if (layerIndex >= 0 && at < s.layers.Size() &&
                                                 s.layers.Size() > 1)
                                             {
                                                 s.layers.RemoveAt(at);
                                                 self->m_asset->layerStatePositions.RemoveAt(at);
                                                 self->m_asset->layerAnyStatePositions.RemoveAt(at);
                                             }
                                         }},
                        GraphSel{GraphSelKind::Layer, 0, 0});
    }

    void AnimationGraphEditorPage::DeleteParam(i32 paramIndex)
    {
        AnimationGraphEditorPage* self = this;
        QueueStructural(u8"del-param",
                        Function<void()>{[self, paramIndex]()
                                         { self->DeleteParameterInternal(paramIndex); }},
                        GraphSel{});
    }

    animation::GraphTransitionData* AnimationGraphEditorPage::TransitionAt(i32 layer,
                                                                           i32 transition) const
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layer < 0 || transition < 0 || static_cast<usize>(layer) >= source.layers.Size())
        {
            return nullptr;
        }
        Array<animation::GraphTransitionData>& transitions =
            source.layers[static_cast<usize>(layer)].transitions;
        return static_cast<usize>(transition) < transitions.Size()
                   ? &transitions[static_cast<usize>(transition)]
                   : nullptr;
    }

    // ============================ Context menus =============================================

    void AnimationGraphEditorPage::ShowCanvasMenu(f32 canvasX, f32 canvasY)
    {
        ui::UIContext* ctx = Ctx();
        if (ctx == nullptr || m_asset.Get() == nullptr)
        {
            return;
        }
        AnimationGraphEditorPage* self = this;
        const i32 layerIndex = m_selectedLayer;
        const Float2 at{canvasX, canvasY};
        auto menu = MakeRef<ui::ContextMenu>(Allocator());
        auto addState = [self, layerIndex, at, &menu](StringView label, u8 kind)
        {
            menu->AddItem(
                label,
                [self, layerIndex, at, kind]()
                {
                    self->QueueStructural(
                        u8"add-state",
                        Function<void()>{
                            [self, layerIndex, at, kind]()
                            {
                                animation::AnimationGraphSource& source = self->m_asset->source;
                                if (layerIndex < 0 ||
                                    static_cast<usize>(layerIndex) >= source.layers.Size())
                                {
                                    return;
                                }
                                animation::GraphLayerData& layer =
                                    source.layers[static_cast<usize>(layerIndex)];
                                animation::GraphStateData state;
                                state.name = Format(u8"State {}", layer.states.Size());
                                state.node.kind = kind;
                                layer.states.PushBack(Move(state));
                                self->SyncLayoutArrays();
                                self->m_asset->layerStatePositions[static_cast<usize>(layerIndex)]
                                    .Back() = at;
                            }},
                        GraphSel{GraphSelKind::State, layerIndex, -2 /*last*/});
                });
        };
        addState(u8"Add Clip State", 0);
        addState(u8"Add Blend Tree 1D", 1);
        addState(u8"Add Blend Tree 2D", 2);
        // The canvas hands CANVAS coords; local = canvas * zoom + pan, then to screen.
        const Float2 local{canvasX * m_canvas->Zoom() + m_canvas->PanOffset().x,
                           canvasY * m_canvas->Zoom() + m_canvas->PanOffset().y};
        const Float2 screen = m_canvas->LocalToScreen(local);
        menu->Show(ctx, screen.x, screen.y);
    }

    void AnimationGraphEditorPage::ShowNodeMenu(i32 nodeIndex)
    {
        ui::UIContext* ctx = Ctx();
        animation::GraphLayerData* layer = SelectedLayer();
        if (ctx == nullptr || layer == nullptr)
        {
            return;
        }
        AnimationGraphEditorPage* self = this;
        const i32 layerIndex = m_selectedLayer;
        auto menu = MakeRef<ui::ContextMenu>(Allocator());
        menu->AddItem(u8"Make Transition",
                      [self, nodeIndex]() { self->m_canvas->StartLinkFrom(nodeIndex); });
        if (nodeIndex != kAnyStateNode)
        {
            const i32 stateIndex = NodeToState(nodeIndex);
            menu->AddItem(
                u8"Set as Default State",
                [self, layerIndex, stateIndex]()
                {
                    self->QueueStructural(
                        u8"set-default",
                        Function<void()>{
                            [self, layerIndex, stateIndex]()
                            {
                                animation::AnimationGraphSource& source = self->m_asset->source;
                                if (layerIndex >= 0 &&
                                    static_cast<usize>(layerIndex) < source.layers.Size())
                                {
                                    source.layers[static_cast<usize>(layerIndex)].defaultState =
                                        stateIndex;
                                }
                            }},
                        GraphSel{GraphSelKind::State, layerIndex, stateIndex});
                });
            menu->AddSeparator();
            menu->AddItem(
                u8"Delete State",
                [self, layerIndex, stateIndex]()
                {
                    self->QueueStructural(
                        u8"del-state",
                        Function<void()>{[self, layerIndex, stateIndex]()
                                         { self->DeleteStateInternal(layerIndex, stateIndex); }},
                        GraphSel{GraphSelKind::Layer, layerIndex, layerIndex});
                });
        }
        const Float2 screen = m_canvas->LocalToScreen(Float2{0.0f, 0.0f});
        // Anchor near the node header.
        ui::toolkit::NodeGraphNode* node = m_canvas->GetNode(nodeIndex);
        Float2 at = screen;
        if (node != nullptr)
        {
            const Float2 nodeScreen{(node->Position.x * m_canvas->Zoom()) + m_canvas->PanOffset().x,
                                    (node->Position.y * m_canvas->Zoom()) +
                                        m_canvas->PanOffset().y};
            at = m_canvas->LocalToScreen(nodeScreen);
        }
        menu->Show(ctx, at.x + 16.0f, at.y + 16.0f);
    }

    void AnimationGraphEditorPage::ShowConnectionMenu(i32 connectionIndex)
    {
        ui::UIContext* ctx = Ctx();
        if (ctx == nullptr)
        {
            return;
        }
        AnimationGraphEditorPage* self = this;
        const i32 layerIndex = m_selectedLayer;
        auto menu = MakeRef<ui::ContextMenu>(Allocator());
        menu->AddItem(
            u8"Edit Transition", [self, layerIndex, connectionIndex]()
            { self->Select(GraphSel{GraphSelKind::Transition, layerIndex, connectionIndex}); });
        menu->AddSeparator();
        menu->AddItem(u8"Delete Transition",
                      [self, layerIndex, connectionIndex]()
                      {
                          self->QueueStructural(
                              u8"del-transition",
                              Function<void()>{
                                  [self, layerIndex, connectionIndex]()
                                  {
                                      animation::AnimationGraphSource& source =
                                          self->m_asset->source;
                                      if (layerIndex < 0 ||
                                          static_cast<usize>(layerIndex) >= source.layers.Size())
                                      {
                                          return;
                                      }
                                      Array<animation::GraphTransitionData>& transitions =
                                          source.layers[static_cast<usize>(layerIndex)].transitions;
                                      if (connectionIndex >= 0 &&
                                          static_cast<usize>(connectionIndex) < transitions.Size())
                                      {
                                          transitions.RemoveAt(static_cast<usize>(connectionIndex));
                                      }
                                  }},
                              GraphSel{});
                      });
        const Float2 screen = m_canvas->LocalToScreen(Float2{20.0f, 20.0f});
        menu->Show(ctx, screen.x, screen.y);
    }

    // ============================ Structural apply ==========================================

    void AnimationGraphEditorPage::QueueStructural(StringView undoKey, Function<void()> mutate,
                                                   GraphSel reselect)
    {
        AnimationGraphEditorPage* self = this;
        String key(undoKey);
        auto run = [self, mutate = Move(mutate), reselect, key = Move(key)]() mutable
        {
            mutate();
            self->SyncLayoutArrays();
            // Clamp the layer selection (layer deletes shift it).
            const i32 layerCount = static_cast<i32>(self->m_asset->source.layers.Size());
            self->m_selectedLayer = Clamp(self->m_selectedLayer, 0, Max(layerCount - 1, 0));
            // "-2" = select the LAST element of the reselect kind (new additions).
            GraphSel sel = reselect;
            if (sel.index == -2)
            {
                animation::AnimationGraphSource& source = self->m_asset->source;
                if (sel.kind == GraphSelKind::Parameter)
                {
                    sel.index = static_cast<i32>(source.paramNames.Size()) - 1;
                }
                else if (sel.kind == GraphSelKind::State &&
                         static_cast<usize>(sel.layer) < source.layers.Size())
                {
                    sel.index = static_cast<i32>(
                                    source.layers[static_cast<usize>(sel.layer)].states.Size()) -
                                1;
                }
                else if (sel.kind == GraphSelKind::Transition &&
                         static_cast<usize>(sel.layer) < source.layers.Size())
                {
                    sel.index =
                        static_cast<i32>(
                            source.layers[static_cast<usize>(sel.layer)].transitions.Size()) -
                        1;
                }
            }
            self->m_selected = sel;

            Array<byte> after = self->SnapshotAsset();
            (void)self->Commands().Execute(
                UniquePtr<IEditorCommand>(self->Allocator().New<EditGraphCommand>(
                                              *self, key.AsView(), self->m_undoBaseline, after),
                                          self->Allocator()));
            self->m_undoBaseline = Move(after);
            self->RebuildLeftPanel();
            self->RebuildCanvas();
            self->RebuildInspector();
            self->RebuildPreviewGraph();
            self->MarkDirty();
        };
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{Move(run)});
        }
        else
        {
            run();
        }
    }

    void AnimationGraphEditorPage::Select(const GraphSel& sel)
    {
        m_selected = sel;
        AnimationGraphEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(
                Function<void()>{[self]() { self->RebuildInspector(); }});
        }
        else
        {
            RebuildInspector();
        }
    }

    ui::UIContext* AnimationGraphEditorPage::Ctx() const
    {
        return (m_canvas.Get() != nullptr) ? m_canvas->Context : nullptr;
    }

    // ============================ Inspectors ================================================

    void AnimationGraphEditorPage::RebuildInspector()
    {
        m_grid->Clear();
        if (m_asset.Get() == nullptr)
        {
            m_inspectorTitle->SetText(u8"(no asset)");
            return;
        }
        switch (m_selected.kind)
        {
        case GraphSelKind::Layer:
            m_inspectorTitle->SetText(u8"Layer");
            BuildLayerInspector(m_selected.index);
            break;
        case GraphSelKind::Parameter:
            m_inspectorTitle->SetText(u8"Parameter");
            BuildParameterInspector(m_selected.index);
            break;
        case GraphSelKind::State:
            m_inspectorTitle->SetText(u8"State");
            BuildStateInspector(m_selected.layer, m_selected.index);
            break;
        case GraphSelKind::Transition:
            m_inspectorTitle->SetText(u8"Transition");
            BuildTransitionInspector(m_selected.layer, m_selected.index);
            break;
        case GraphSelKind::None:
        default:
            m_inspectorTitle->SetText(u8"");
            break;
        }
    }

    void AnimationGraphEditorPage::BuildLayerInspector(i32 layerIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layerIndex < 0 || static_cast<usize>(layerIndex) >= source.layers.Size())
        {
            return;
        }
        animation::GraphLayerData& layer = source.layers[static_cast<usize>(layerIndex)];
        Page page = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = u8"Layer";
        AnimationGraphEditorPage* self = this;

        RowString(g, u8"Name", &layer.name, cat, page);
        static constexpr StringView kBlend[] = {u8"Override", u8"Additive"};
        RowEnum(g, u8"Blend Mode", static_cast<i32>(layer.blendMode),
                Span<const StringView>{kBlend, 2},
                Function<void(i32)>{[&layer, page](i32 v)
                                    {
                                        layer.blendMode = static_cast<u8>(v);
                                        page->CommitEdit(u8"layer-blend");
                                    }},
                cat);
        RowFloat(g, u8"Weight", &layer.weight, cat, page, 0.0, 1.0, 0.01);
        // Default state dropdown by name.
        {
            Array<String> owned;
            for (const animation::GraphStateData& s : layer.states)
            {
                owned.PushBack(String(s.name.AsView()));
            }
            Array<StringView> items;
            for (const String& s : owned)
            {
                items.PushBack(s.AsView());
            }
            if (!items.IsEmpty())
            {
                const i32 layerIdx = layerIndex;
                RowEnum(
                    g, u8"Default State",
                    Clamp(layer.defaultState, 0, static_cast<i32>(items.Size()) - 1),
                    Span<const StringView>{items.Data(), items.Size()},
                    Function<void(i32)>{
                        [self, layerIdx](i32 v)
                        {
                            self->QueueStructural(
                                u8"layer-default",
                                Function<void()>{
                                    [self, layerIdx, v]()
                                    {
                                        animation::AnimationGraphSource& s = self->m_asset->source;
                                        if (static_cast<usize>(layerIdx) < s.layers.Size())
                                        {
                                            s.layers[static_cast<usize>(layerIdx)].defaultState = v;
                                        }
                                    }},
                                GraphSel{GraphSelKind::Layer, layerIdx, layerIdx});
                        }},
                    cat);
            }
        }
        if (source.layers.Size() > 1)
        {
            const i32 layerIdx = layerIndex;
            g.SetCategoryHeaderActions(
                cat, RemoveIcon(Allocator(), u8"Delete layer",
                                [self, layerIdx]() { self->DeleteLayer(layerIdx); }));
        }
    }

    void AnimationGraphEditorPage::BuildParameterInspector(i32 paramIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (paramIndex < 0 || static_cast<usize>(paramIndex) >= source.paramNames.Size())
        {
            return;
        }
        Page page = this;
        AnimationGraphEditorPage* self = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = u8"Parameter";
        const usize p = static_cast<usize>(paramIndex);

        RowString(g, u8"Name", &source.paramNames[p], cat, page);
        static constexpr StringView kTypes[] = {u8"Float", u8"Int", u8"Bool", u8"Trigger"};
        RowEnum(g, u8"Type",
                static_cast<i32>(p < source.paramTypes.Size() ? source.paramTypes[p] : 0),
                Span<const StringView>{kTypes, 4},
                Function<void(i32)>{[&source, p, page](i32 v)
                                    {
                                        if (p < source.paramTypes.Size())
                                        {
                                            source.paramTypes[p] = static_cast<u8>(v);
                                            page->CommitEdit(u8"param-type");
                                        }
                                    }},
                cat);
        const u8 type = p < source.paramTypes.Size() ? source.paramTypes[p] : 0;
        if (type == 0 && p < source.paramFloats.Size())
        {
            RowFloat(g, u8"Default", &source.paramFloats[p], cat, page);
        }
        else if (type == 1 && p < source.paramInts.Size())
        {
            RowInt(g, u8"Default", &source.paramInts[p], cat, page);
        }
        else if ((type == 2 || type == 3) && p < source.paramBools.Size())
        {
            // Bool stored as u8; edit through a temp.
            const bool current = source.paramBools[p] != 0;
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::BoolEditor>(
                           Allocator(), u8"Default", current,
                           Function<void(bool)>{[&source, p, page](bool v)
                                                {
                                                    source.paramBools[p] = v ? 1 : 0;
                                                    page->CommitEdit(u8"param-default");
                                                }},
                           cat)
                           .Get()));
        }
        // Live scrub: when the preview player is running, drive ITS runtime copy of the
        // parameter directly (no undo - runtime state, not asset data).
        if (m_player.Get() != nullptr)
        {
            const StringView liveCat = u8"Live (preview)";
            const i32 pi = paramIndex;
            if (type == 0)
            {
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                           MakeRef<ui::toolkit::FloatEditor>(
                               Allocator(), u8"Value",
                               static_cast<f64>(m_player->GetFloat(pi)), -1e6, 1e6, 0.02, 3,
                               Function<void(f64)>{[self, pi](f64 v)
                                                   {
                                                       if (self->m_player.Get() != nullptr)
                                                       {
                                                           self->m_player->SetFloat(
                                                               pi, static_cast<f32>(v));
                                                       }
                                                   }},
                               liveCat)
                               .Get()));
            }
            else if (type == 1)
            {
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                           MakeRef<ui::toolkit::IntEditor>(
                               Allocator(), u8"Value",
                               static_cast<i64>(m_player->GetInt(pi)), -1000000, 1000000,
                               Function<void(i64)>{[self, pi](i64 v)
                                                   {
                                                       if (self->m_player.Get() != nullptr)
                                                       {
                                                           self->m_player->SetInt(
                                                               pi, static_cast<i32>(v));
                                                       }
                                                   }},
                               liveCat)
                               .Get()));
            }
            else if (type == 2)
            {
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                           MakeRef<ui::toolkit::BoolEditor>(
                               Allocator(), u8"Value", m_player->GetBool(pi),
                               Function<void(bool)>{[self, pi](bool v)
                                                    {
                                                        if (self->m_player.Get() != nullptr)
                                                        {
                                                            self->m_player->SetBool(pi, v);
                                                        }
                                                    }},
                               liveCat)
                               .Get()));
            }
            else
            {
                RowButton(g, u8"Fire Trigger", liveCat,
                          [self, pi]()
                          {
                              if (self->m_player.Get() != nullptr)
                              {
                                  self->m_player->SetTrigger(pi);
                              }
                          });
            }
        }

        const i32 paramIdx = paramIndex;
        g.SetCategoryHeaderActions(cat,
                                   RemoveIcon(Allocator(), u8"Delete parameter",
                                              [self, paramIdx]() { self->DeleteParam(paramIdx); }));
    }

    void AnimationGraphEditorPage::BuildStateInspector(i32 layerIndex, i32 stateIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layerIndex < 0 || static_cast<usize>(layerIndex) >= source.layers.Size())
        {
            return;
        }
        animation::GraphLayerData& layer = source.layers[static_cast<usize>(layerIndex)];
        if (stateIndex < 0 || static_cast<usize>(stateIndex) >= layer.states.Size())
        {
            return;
        }
        animation::GraphStateData& state = layer.states[static_cast<usize>(stateIndex)];
        Page page = this;
        AnimationGraphEditorPage* self = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = u8"State";

        // Name edits relabel the canvas node - structural (rebuild) so the title updates.
        {
            const i32 li = layerIndex, si = stateIndex;
            Add(g,
                RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::StringEditor>(
                        Allocator(), u8"Name", state.name.AsView(),
                        Function<void(StringView)>{
                            [self, li, si](StringView v)
                            {
                                String name(v);
                                self->QueueStructural(
                                    u8"state-name",
                                    Function<void()>{
                                        [self, li, si, name]()
                                        {
                                            animation::AnimationGraphSource& s =
                                                self->m_asset->source;
                                            if (static_cast<usize>(li) < s.layers.Size() &&
                                                static_cast<usize>(si) <
                                                    s.layers[static_cast<usize>(li)].states.Size())
                                            {
                                                s.layers[static_cast<usize>(li)]
                                                    .states[static_cast<usize>(si)]
                                                    .name = String(name.AsView());
                                            }
                                        }},
                                    GraphSel{GraphSelKind::State, li, si});
                            }},
                        cat)
                        .Get()));
        }
        RowFloat(g, u8"Speed", &state.speed, cat, page, -10.0, 10.0, 0.05);
        RowBool(g, u8"Loop", &state.loop, cat, page);

        // --- node config (kind-specific) ---
        animation::GraphNodeData& node = state.node;
        const String kindCat = Format(u8"{}", NodeKindLabel(node.kind));
        if (node.kind == 0)
        {
            // The state's clip: the shared asset row (pick, drop and clear are one assignment).
            const i32 li = layerIndex, si = stateIndex;
            const StringView clipTypes[] = {u8"AnimationClipAsset"};
            auto clip = MakeRef<app::ResourceRefEditor>(Allocator(), StringView(u8"Clip"),
                                                        StringView(u8"(none)"), kindCat.AsView(),
                                                        Span<const StringView>{clipTypes, 1});
            clip->BindAsset(
                *m_context,
                [self, li, si]()
                {
                    animation::GraphNodeData* n = self->StateNode(li, si);
                    return n != nullptr ? n->clipRef : Guid{};
                },
                [self, li, si](const Guid& picked)
                {
                    if (animation::GraphNodeData* n = self->StateNode(li, si))
                    {
                        n->clipRef = picked;
                        self->CommitEdit(u8"state-clip");
                        self->Select(GraphSel{GraphSelKind::State, li, si});
                    }
                });
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(clip.Get()));
        }
        else
        {
            // Blend tree drivers.
            if (node.kind == 1)
            {
                RowParamPick(g, u8"Parameter", source, node.paramIndex,
                             Function<void(i32)>{[&node, page](i32 v)
                                                 {
                                                     node.paramIndex = v;
                                                     page->CommitEdit(u8"blend-param");
                                                 }},
                             kindCat.AsView());
            }
            else
            {
                RowParamPick(g, u8"Parameter X", source, node.paramIndexX,
                             Function<void(i32)>{[&node, page](i32 v)
                                                 {
                                                     node.paramIndexX = v;
                                                     page->CommitEdit(u8"blend-param-x");
                                                 }},
                             kindCat.AsView());
                RowParamPick(g, u8"Parameter Y", source, node.paramIndexY,
                             Function<void(i32)>{[&node, page](i32 v)
                                                 {
                                                     node.paramIndexY = v;
                                                     page->CommitEdit(u8"blend-param-y");
                                                 }},
                             kindCat.AsView());
            }

            // The entries are a section list: the header's add icon, and each entry a section
            // inside the state's kind with its remove icon (an entry's threshold or position places it, so its order
            // means nothing).
            {
                const i32 li = layerIndex, si = stateIndex;
                auto entries = MakeRef<app::ContainerListEditor>(
                    Allocator(), StringView(u8"Entries"), kindCat.AsView());
                entries->ElementsAsSections = true;
                for (const Guid& clipId : node.entryClips)
                {
                    entries->slotNames.PushBack(String(m_context->AssetNameFor(clipId)));
                }
                entries->OnAdd = [self, li, si]()
                {
                    self->QueueStructural(u8"add-entry",
                                          Function<void()>{[self, li, si]()
                                                           {
                                                               animation::GraphNodeData* n =
                                                                   self->StateNode(li, si);
                                                               if (n != nullptr)
                                                               {
                                                                   n->entryClips.PushBack(Guid{});
                                                                   n->entryThresholds.PushBack(0.0f);
                                                                   n->entryPositions.PushBack(
                                                                       Float2{0.0f, 0.0f});
                                                               }
                                                           }},
                                          GraphSel{GraphSelKind::State, li, si});
                };
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(entries.Get()));
            }
            const usize entryCount = node.entryClips.Size();
            for (usize e = 0; e < entryCount; ++e)
            {
                const String entryCat = Format(u8"Entry {}", e + 1);
                g.SetCategoryParent(entryCat.AsView(), kindCat.AsView());
                {
                    const i32 li = layerIndex, si = stateIndex;
                    g.SetCategoryHeaderActions(
                        entryCat.AsView(),
                        app::ContainerListEditor::ElementActions(
                            Allocator(), e, entryCount, Function<void(usize, bool)>{},
                            [self, li, si](usize i)
                            {
                                self->QueueStructural(
                                    u8"del-entry",
                                    Function<void()>{[self, li, si, i]()
                                                     {
                                                         animation::GraphNodeData* n =
                                                             self->StateNode(li, si);
                                                         if (n == nullptr)
                                                         {
                                                             return;
                                                         }
                                                         if (i < n->entryClips.Size())
                                                         {
                                                             n->entryClips.RemoveAt(i);
                                                         }
                                                         if (i < n->entryThresholds.Size())
                                                         {
                                                             n->entryThresholds.RemoveAt(i);
                                                         }
                                                         if (i < n->entryPositions.Size())
                                                         {
                                                             n->entryPositions.RemoveAt(i);
                                                         }
                                                     }},
                                    GraphSel{GraphSelKind::State, li, si});
                            }));
                }
                if (node.kind == 1)
                {
                    while (node.entryThresholds.Size() < node.entryClips.Size())
                    {
                        node.entryThresholds.PushBack(0.0f);
                    }
                    RowFloat(g, u8"Threshold", &node.entryThresholds[e], entryCat.AsView(), page);
                }
                else
                {
                    while (node.entryPositions.Size() < node.entryClips.Size())
                    {
                        node.entryPositions.PushBack(Float2{0.0f, 0.0f});
                    }
                    Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                               MakeRef<ui::toolkit::Float2Editor>(
                                   Allocator(), u8"Position", node.entryPositions[e],
                                   -1000.0f, 1000.0f, 0.05f,
                                   Function<void(Float2)>{[&node, e, page](Float2 v)
                                                          {
                                                              node.entryPositions[e] = v;
                                                              page->CommitEdit(u8"entry-pos");
                                                          }},
                                   entryCat.AsView())
                                   .Get()));
                }
                const i32 li = layerIndex, si = stateIndex;
                const usize entryIdx = e;
                const StringView entryTypes[] = {u8"AnimationClipAsset"};
                auto entryClip = MakeRef<app::ResourceRefEditor>(
                    Allocator(), StringView(u8"Clip"), StringView(u8"(none)"), entryCat.AsView(),
                    Span<const StringView>{entryTypes, 1});
                entryClip->BindAsset(
                    *m_context,
                    [self, li, si, entryIdx]()
                    {
                        animation::GraphNodeData* n = self->StateNode(li, si);
                        return n != nullptr && entryIdx < n->entryClips.Size()
                                   ? n->entryClips[entryIdx]
                                   : Guid{};
                    },
                    [self, li, si, entryIdx](const Guid& picked)
                    {
                        animation::GraphNodeData* n = self->StateNode(li, si);
                        if (n != nullptr && entryIdx < n->entryClips.Size())
                        {
                            n->entryClips[entryIdx] = picked;
                            self->CommitEdit(u8"entry-clip");
                            self->Select(GraphSel{GraphSelKind::State, li, si});
                        }
                    });
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(entryClip.Get()));
            }
        }
    }

    void AnimationGraphEditorPage::BuildTransitionInspector(i32 layerIndex, i32 transitionIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layerIndex < 0 || static_cast<usize>(layerIndex) >= source.layers.Size())
        {
            return;
        }
        animation::GraphLayerData& layer = source.layers[static_cast<usize>(layerIndex)];
        if (transitionIndex < 0 || static_cast<usize>(transitionIndex) >= layer.transitions.Size())
        {
            return;
        }
        animation::GraphTransitionData& transition =
            layer.transitions[static_cast<usize>(transitionIndex)];
        Page page = this;
        AnimationGraphEditorPage* self = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = u8"Transition";

        // Readout: src -> dst by state name.
        {
            auto nameOf = [&layer](i32 idx) -> String
            {
                if (idx < 0)
                {
                    return String(u8"Any State");
                }
                if (static_cast<usize>(idx) < layer.states.Size())
                {
                    return String(layer.states[static_cast<usize>(idx)].name.AsView());
                }
                return String(u8"?");
            };
            String route = nameOf(transition.src);
            route.Append(u8"  ->  ");
            route.Append(nameOf(transition.dst).AsView());
            m_inspectorTitle->SetText(route.AsView());
        }

        RowFloat(g, u8"Duration (s)", &transition.duration, cat, page, 0.0, 10.0, 0.01);
        RowBool(g, u8"Has Exit Time", &transition.hasExitTime, cat, page);
        RowFloat(g, u8"Exit Time", &transition.exitTime, cat, page, 0.0, 1.0, 0.01);
        RowInt(g, u8"Priority", &transition.priority, cat, page, -100, 100);

        // The conditions are a section list, all of them required, so their order means
        // nothing: the header's add icon, and each condition a section inside the transition's
        // with its remove icon.
        const i32 li = layerIndex, ti = transitionIndex;
        auto conditions =
            MakeRef<app::ContainerListEditor>(Allocator(), StringView(u8"Conditions"), cat);
        conditions->ElementsAsSections = true;
        for (const animation::GraphConditionData& condition : transition.conditions)
        {
            const bool named = condition.paramIndex >= 0 &&
                               static_cast<usize>(condition.paramIndex) < source.paramNames.Size();
            conditions->slotNames.PushBack(
                named ? source.paramNames[static_cast<usize>(condition.paramIndex)]
                      : String(u8"(none)"));
        }
        conditions->OnAdd = [self, li, ti]()
        {
            self->QueueStructural(u8"add-cond",
                                  Function<void()>{[self, li, ti]()
                                                   {
                                                       if (animation::GraphTransitionData* t =
                                                               self->TransitionAt(li, ti))
                                                       {
                                                           t->conditions.PushBack(
                                                               animation::GraphConditionData{});
                                                       }
                                                   }},
                                  GraphSel{GraphSelKind::Transition, li, ti});
        };
        Add(g, RefPtr<ui::toolkit::PropertyEditor>(conditions.Get()));
        const usize conditionCount = transition.conditions.Size();
        for (usize c = 0; c < conditionCount; ++c)
        {
            animation::GraphConditionData& condition = transition.conditions[c];
            const String condCat = Format(u8"Condition {}", c + 1);
            g.SetCategoryParent(condCat.AsView(), cat);
            g.SetCategoryHeaderActions(
                condCat.AsView(),
                app::ContainerListEditor::ElementActions(
                    Allocator(), c, conditionCount, Function<void(usize, bool)>{},
                    [self, li, ti](usize i)
                    {
                        self->QueueStructural(
                            u8"del-cond",
                            Function<void()>{[self, li, ti, i]()
                                             {
                                                 animation::GraphTransitionData* t =
                                                     self->TransitionAt(li, ti);
                                                 if (t != nullptr && i < t->conditions.Size())
                                                 {
                                                     t->conditions.RemoveAt(i);
                                                 }
                                             }},
                            GraphSel{GraphSelKind::Transition, li, ti});
                    }));
            RowParamPick(g, u8"Parameter", source, condition.paramIndex,
                         Function<void(i32)>{[&condition, page](i32 v)
                                             {
                                                 condition.paramIndex = v;
                                                 page->CommitEdit(u8"cond-param");
                                             }},
                         condCat.AsView());
            static constexpr StringView kOps[] = {u8"==", u8"!=", u8">", u8"<", u8">=", u8"<="};
            RowEnum(g, u8"Compare", static_cast<i32>(condition.op), Span<const StringView>{kOps, 6},
                    Function<void(i32)>{[&condition, page](i32 v)
                                        {
                                            condition.op = static_cast<u8>(v);
                                            page->CommitEdit(u8"cond-op");
                                        }},
                    condCat.AsView());
            RowFloat(g, u8"Threshold", &condition.threshold, condCat.AsView(), page);
        }
    }

    // ============================ Structural internals ======================================

    void AnimationGraphEditorPage::DeleteStateInternal(i32 layerIndex, i32 stateIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layerIndex < 0 || static_cast<usize>(layerIndex) >= source.layers.Size())
        {
            return;
        }
        animation::GraphLayerData& layer = source.layers[static_cast<usize>(layerIndex)];
        if (stateIndex < 0 || static_cast<usize>(stateIndex) >= layer.states.Size())
        {
            return;
        }
        layer.states.RemoveAt(static_cast<usize>(stateIndex));
        // Transitions: drop the ones touching the state, remap indices above it.
        for (i32 t = static_cast<i32>(layer.transitions.Size()) - 1; t >= 0; --t)
        {
            animation::GraphTransitionData& tr = layer.transitions[static_cast<usize>(t)];
            if (tr.src == stateIndex || tr.dst == stateIndex)
            {
                layer.transitions.RemoveAt(static_cast<usize>(t));
                continue;
            }
            if (tr.src > stateIndex)
            {
                tr.src--;
            }
            if (tr.dst > stateIndex)
            {
                tr.dst--;
            }
        }
        if (layer.defaultState == stateIndex)
        {
            layer.defaultState = 0;
        }
        else if (layer.defaultState > stateIndex)
        {
            layer.defaultState--;
        }
        // Layout array stays parallel.
        Array<Float2>& positions = m_asset->layerStatePositions[static_cast<usize>(layerIndex)];
        if (static_cast<usize>(stateIndex) < positions.Size())
        {
            positions.RemoveAt(static_cast<usize>(stateIndex));
        }
    }

    void AnimationGraphEditorPage::DeleteParameterInternal(i32 paramIndex)
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (paramIndex < 0 || static_cast<usize>(paramIndex) >= source.paramNames.Size())
        {
            return;
        }
        const usize p = static_cast<usize>(paramIndex);
        source.paramNames.RemoveAt(p);
        if (p < source.paramTypes.Size())
        {
            source.paramTypes.RemoveAt(p);
        }
        if (p < source.paramFloats.Size())
        {
            source.paramFloats.RemoveAt(p);
        }
        if (p < source.paramInts.Size())
        {
            source.paramInts.RemoveAt(p);
        }
        if (p < source.paramBools.Size())
        {
            source.paramBools.RemoveAt(p);
        }
        // Remap references: conditions on it DROP; blend drivers on it clear to -1; higher
        // indices decrement.
        auto remap = [paramIndex](i32& index)
        {
            if (index == paramIndex)
            {
                index = -1;
            }
            else if (index > paramIndex)
            {
                index--;
            }
        };
        for (animation::GraphLayerData& layer : source.layers)
        {
            for (animation::GraphStateData& state : layer.states)
            {
                remap(state.node.paramIndex);
                remap(state.node.paramIndexX);
                remap(state.node.paramIndexY);
            }
            for (animation::GraphTransitionData& transition : layer.transitions)
            {
                for (i32 c = static_cast<i32>(transition.conditions.Size()) - 1; c >= 0; --c)
                {
                    animation::GraphConditionData& condition =
                        transition.conditions[static_cast<usize>(c)];
                    if (condition.paramIndex == paramIndex)
                    {
                        transition.conditions.RemoveAt(static_cast<usize>(c));
                    }
                    else if (condition.paramIndex > paramIndex)
                    {
                        condition.paramIndex--;
                    }
                }
            }
        }
    }

    // ============================ Undo ======================================================

    Array<byte> AnimationGraphEditorPage::SnapshotAsset() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        m_asset->Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void AnimationGraphEditorPage::ApplyAssetBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        // If-changed guard: Execute() at push time is a no-op (state already applied live).
        Array<byte> current = SnapshotAsset();
        if (current.Size() == blob.Size())
        {
            bool same = true;
            for (usize i = 0; i < blob.Size(); ++i)
            {
                if (current[i] != blob[i])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return;
            }
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        // Reset the asset to defaults, then deserialize (arrays are appended by Serialize;
        // the source is an ISerializable - no copy assign - so clear its arrays in place).
        m_asset->source.paramNames.Clear();
        m_asset->source.paramTypes.Clear();
        m_asset->source.paramFloats.Clear();
        m_asset->source.paramInts.Clear();
        m_asset->source.paramBools.Clear();
        m_asset->source.layers.Clear();
        m_asset->layerStatePositions.Clear();
        m_asset->layerAnyStatePositions.Clear();
        m_asset->Serialize(ar);
        m_undoBaseline = blob;
        SyncLayoutArrays();
        const i32 layerCount = static_cast<i32>(m_asset->source.layers.Size());
        m_selectedLayer = Clamp(m_selectedLayer, 0, Max(layerCount - 1, 0));
        AnimationGraphEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{[self]()
                                                                 {
                                                                     self->RebuildLeftPanel();
                                                                     self->RebuildCanvas();
                                                                     self->RebuildInspector();
                                                                     self->RebuildPreviewGraph();
                                                                 }});
        }
        else
        {
            RebuildLeftPanel();
            RebuildCanvas();
            RebuildInspector();
            RebuildPreviewGraph();
        }
        MarkDirty();
    }

    void AnimationGraphEditorPage::CommitEdit(StringView mergeKey)
    {
        Array<byte> after = SnapshotAsset();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditGraphCommand>(*this, mergeKey, m_undoBaseline, after),
            Allocator()));
        m_undoBaseline = Move(after);
        RebuildPreviewGraph();
        MarkDirty();
    }

    // ============================ Live preview ==============================================

    animation::GraphNodeData* AnimationGraphEditorPage::StateNode(i32 layer, i32 state) const
    {
        animation::AnimationGraphSource& source = m_asset->source;
        if (layer < 0 || state < 0 || static_cast<usize>(layer) >= source.layers.Size() ||
            static_cast<usize>(state) >= source.layers[static_cast<usize>(layer)].states.Size())
        {
            return nullptr;
        }
        return &source.layers[static_cast<usize>(layer)].states[static_cast<usize>(state)].node;
    }

    void AnimationGraphEditorPage::SetPreviewSkeleton(const Guid& id)
    {
        m_skeletonGuid = id;
        if (m_context->Resources() != nullptr && !id.IsNil())
        {
            m_skeleton = m_context->Resources()->Bind<animation::Skeleton>(id);
        }
        else
        {
            m_skeleton = foundation::resource::Proxy<animation::Skeleton>{};
        }
        if (m_skeletonSlot.Get() != nullptr)
        {
            m_skeletonSlot->Editor().Refresh();
        }
    }

    void AnimationGraphEditorPage::SetPreviewMesh(const Guid& id)
    {
        m_previewMeshId = id;
        if (m_context->Resources() != nullptr && !id.IsNil())
        {
            m_previewMesh = m_context->Resources()->Bind<foundation::geometry::StaticMesh>(id);
        }
        else
        {
            m_previewMesh = foundation::resource::Proxy<foundation::geometry::StaticMesh>{};
        }
        // Point the preview MeshComponent at the mesh (skinning matrices feed per frame).
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr != nullptr)
        {
            if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            {
                if (auto* mc = meshes->Get(m_meshEntity))
                {
                    if (foundation::geometry::StaticMesh* pm = m_previewMesh.Get())
                    {
                        mc->mesh = pm;
                    }
                    else
                    {
                        mc->mesh.SetDirect(RefPtr<foundation::geometry::StaticMesh>{});
                    }
                }
            }
        }
        if (m_meshSlot.Get() != nullptr)
        {
            m_meshSlot->Editor().Refresh();
        }
    }

    void AnimationGraphEditorPage::RebuildPreviewGraph()
    {
        // Tear down in dependency order: the player borrows graph + skeleton.
        m_player.Reset();
        m_previewGraph = RefPtr<animation::AnimationGraph>{};
        m_playerSkeleton = nullptr;
        m_lastHighlightedNode = -1;
        if (m_asset.Get() == nullptr || m_context->Resources() == nullptr)
        {
            return;
        }
        animation::Skeleton* skeleton = m_skeleton.Get();
        if (skeleton == nullptr || skeleton->BoneCount() <= 0)
        {
            return;
        }
        m_previewGraph = MakeRef<animation::AnimationGraph>(Allocator());
        m_asset->source.BuildInto(*m_context->Resources(), *m_previewGraph);
        if (m_previewGraph->Layers().IsEmpty())
        {
            return;
        }
        m_player = MakeUnique<animation::AnimationGraphPlayer>(Allocator(), *m_previewGraph,
                                                               *skeleton);
        m_playerSkeleton = skeleton;
    }

    void AnimationGraphEditorPage::UpdatePreview(f32 dt)
    {
        // Skeleton hot-reload (or first resolve): the proxy's object changed identity.
        if (m_skeleton.Get() != m_playerSkeleton)
        {
            RebuildPreviewGraph();
        }
        if (m_player.Get() == nullptr)
        {
            if (m_previewStatus.Get() != nullptr)
            {
                m_previewStatus->SetText(m_skeletonGuid.IsNil() ? u8"pick a skeleton to preview"
                                                                : u8"(skeleton not cooked yet)");
            }
            return;
        }
        if (m_previewPlaying)
        {
            m_player->Update(dt);
        }
        // Root motion: read every frame (so it never piles up), applied while Travel is on.
        const animation::RootMotionDelta moved = m_player->ConsumeRootMotion();
        if (m_showTravel)
        {
            m_travelPosition = m_travelPosition +
                               RotateVector(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_travelYaw),
                                            moved.translation);
            m_travelYaw += moved.yaw;
            if (Length(Float3{m_travelPosition.x, 0.0f, m_travelPosition.z}) > 6.0f)
            {
                m_travelPosition = Float3{}; // back to the middle of the grid
            }
        }
        const Float4x4 travel =
            RotationMatrix(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_travelYaw)) *
            Float4x4::Translation(m_travelPosition);

        // Status readout + ACTIVE-state ring on the canvas.
        const i32 current = m_player->GetCurrentStateIndex(
            Min(m_selectedLayer, static_cast<i32>(m_previewGraph->Layers().Size()) - 1));
        if (m_previewStatus.Get() != nullptr)
        {
            String status;
            animation::GraphLayerData* layer = SelectedLayer();
            if (layer != nullptr && current >= 0 &&
                static_cast<usize>(current) < layer->states.Size())
            {
                status.Append(layer->states[static_cast<usize>(current)].name.AsView());
            }
            if (m_player->IsTransitioning(m_selectedLayer))
            {
                status.Append(u8"  (transitioning)");
            }
            m_previewStatus->SetText(status.AsView());
        }
        const i32 highlightNode = (current >= 0) ? StateToNode(current) : -1;
        if (highlightNode != m_lastHighlightedNode)
        {
            for (i32 n = 0; n < m_canvas->NodeCount(); ++n)
            {
                if (ui::toolkit::NodeGraphNode* node = m_canvas->GetNode(n))
                {
                    node->IsHighlighted = (n == highlightNode);
                }
            }
            m_lastHighlightedNode = highlightNode;
            m_canvas->Invalidate();
        }

        if (m_preview.Get() == nullptr || !m_preview->IsValid())
        {
            return;
        }
        scene::Scene* scenePtr = m_preview->Scene();

        // Skinned preview mesh: feed the graph player's skinning matrices to the MeshComponent
        // (borrowed for this frame's render); the toggle shows/hides the whole mesh entity.
        if (scenePtr != nullptr && m_meshEntity.IsAssigned())
        {
            const bool meshVisible = m_showMesh && m_previewMesh.Get() != nullptr;
            scenePtr->SetActive(m_meshEntity, meshVisible);
            Transform at;
            at.position = m_travelPosition;
            at.rotation = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_travelYaw);
            scenePtr->SetLocalTransform(m_meshEntity, at);
            if (meshVisible)
            {
                const Span<const Float4x4> mats = m_player->GetSkinningMatrices();
                if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
                {
                    if (auto* mc = meshes->Get(m_meshEntity))
                    {
                        mc->boneMatrices = mats.Data();
                        mc->boneCount = static_cast<u32>(mats.Size());
                    }
                }
            }
        }

        // Bone wireframe + ground grid into the preview scene's debug lane.
        auto& draw = m_preview->SceneDebugDraw();
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
        if (m_showSkeleton)
        {
            DrawSkeletonWireframe(draw, *m_playerSkeleton, m_player->GetLocalPoses(), m_worldScratch, travel);
        }
    }

    void DrawSkeletonWireframe(foundation::render::debug::DebugDraw& draw,
                               animation::Skeleton& skeleton,
                               Span<const animation::BoneTransform> localPoses,
                               Array<Float4x4>& worldScratch, const Float4x4& base)
    {
        const usize boneCount = static_cast<usize>(skeleton.BoneCount());
        if (boneCount == 0 || localPoses.Size() < boneCount)
        {
            return;
        }
        worldScratch.Resize(boneCount);
        skeleton.ComputeWorldPoses(localPoses, Span<Float4x4>{worldScratch.Data(), boneCount});
        for (usize b = 0; b < boneCount; ++b)
        {
            worldScratch[b] = worldScratch[b] * base;
        }
        const Color boneColor{0.35f, 0.85f, 1.0f, 1.0f};
        for (usize b = 0; b < boneCount; ++b)
        {
            const animation::Bone* bone = skeleton.GetBone(static_cast<i32>(b));
            const Float4x4& world = worldScratch[b];
            const Float3 pos{world.m[3][0], world.m[3][1], world.m[3][2]};
            if (bone != nullptr && bone->parentIndex >= 0 &&
                static_cast<usize>(bone->parentIndex) < boneCount)
            {
                const Float4x4& parent = worldScratch[static_cast<usize>(bone->parentIndex)];
                draw.DrawLine(Float3{parent.m[3][0], parent.m[3][1], parent.m[3][2]}, pos,
                              boneColor);
            }
            draw.DrawCross(pos, 0.02f, boneColor);
        }
    }


    // ============================ Frame / save / close ======================================

    void AnimationGraphEditorPage::Stop()
    {
        m_previewPlaying = false;
        RebuildPreviewGraph(); // a rebuild is the rewind
    }

    void AnimationGraphEditorPage::Restart()
    {
        RebuildPreviewGraph();
        m_previewPlaying = true;
    }

    void AnimationGraphEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // the page's actions and the transport follow it each frame
        }
        UpdatePreview(dt);
        if (m_preview)
        {
            m_preview->Update(dt);
        }
    }

    void AnimationGraphEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                                  foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    Status AnimationGraphEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false);
            LOG_INFO(u8"Editor", u8"saved animation graph '{}'", m_title);
        }
        return saved;
    }

    void AnimationGraphEditorPage::OnClose()
    {
        m_player.Reset();
        m_previewGraph = RefPtr<animation::AnimationGraph>{};
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    // ============================ Factory / creator =========================================

    const TypeInfo* AnimationGraphPageFactory::PrimaryType() const
    {
        return &pipeline::AnimationGraphAsset::StaticType();
    }

    UniquePtr<EditorPage>
    AnimationGraphPageFactory::CreatePage(EditorContext& context,
                                          foundation::content::Instance& instance)
    {
        auto* page =
            editor::EditorRootAllocator().New<AnimationGraphEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterAnimationGraphEditor(EditorContext& context, runtime::IApplicationHost& host,
                                      ui::runtime::UIHost& uiHost)
    {
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(GraphPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<GraphPreviewSettings>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<AnimationGraphPageFactory>(host, uiHost), editor::EditorRootAllocator()));

    }

    RTTI_DEFINE_OBJECT_VERSIONED(GraphPreviewSettings, "rtti::editor::editor.graph", 1)
}
