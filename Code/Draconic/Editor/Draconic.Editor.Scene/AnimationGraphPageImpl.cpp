// Draconic::EditorScene - :animation_graph_page partition (implementation).
//
// The state-machine / blend-tree authoring tool (see AnimationGraphPage.cppm for the overview).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.scene;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {

        using Page = AnimationGraphEditorPage*;

        // Node palette: node 0 = "Any State", node 1+i = state i.
        constexpr i32 kAnyStateNode = 0;
        [[nodiscard]] constexpr i32 StateToNode(i32 state) noexcept { return state + 1; }
        [[nodiscard]] constexpr i32 NodeToState(i32 node) noexcept { return node - 1; }

        [[nodiscard]] foundation::Color HeaderPlain() { return foundation::Color{0.27f, 0.40f, 0.60f, 1.0f}; }
        [[nodiscard]] foundation::Color HeaderDefault() { return foundation::Color{0.80f, 0.52f, 0.18f, 1.0f}; }
        [[nodiscard]] foundation::Color HeaderAny() { return foundation::Color{0.30f, 0.55f, 0.50f, 1.0f}; }

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
                           DefaultAllocator(), name, static_cast<f64>(*field), mn, mx, step, 3,
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
                           DefaultAllocator(), name, static_cast<i64>(*field), mn, mx,
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
                           DefaultAllocator(), name, *field,
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
                       MakeRef<ui::toolkit::EnumEditor>(DefaultAllocator(), name, value, items,
                                                        Move(setter), cat)
                           .Get()));
        }
        void RowButton(ui::toolkit::PropertyGrid& g, StringView name, StringView cat,
                       Function<void()> action)
        {
            Add(g,
                RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::ButtonEditor>(DefaultAllocator(), name, Move(action), cat)
                        .Get()));
        }
        void RowString(ui::toolkit::PropertyGrid& g, StringView name, String* field, StringView cat,
                       Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::StringEditor>(
                           DefaultAllocator(), name, field->AsView(),
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

        // The display name of a cooked/source asset for a picker button ("(none)" for nil).
        [[nodiscard]] String AssetLabel(EditorContext& context, const Guid& id)
        {
            if (id.IsNil())
            {
                return String(u8"(none)");
            }
            if (context.Project() != nullptr)
            {
                if (draconic::content::Instance* inst =
                        context.Project()->SourceDb().GetInstance(id))
                {
                    return String(inst->Name());
                }
            }
            return String(u8"(missing)");
        }
    } // namespace

    // ============================ Construction ==============================================

    AnimationGraphEditorPage::AnimationGraphEditorPage(EditorContext& context,
                                                       runtime::IApplicationHost& host,
                                                       ui::runtime::UIHost& uiHost,
                                                       draconic::content::Instance& instance)
        : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        m_router =
            MakeUnique<draconic::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());
        m_camera.position = Float3{0.0f, 1.4f, 3.2f};
        m_camera.LookAt(Float3{0.0f, 0.9f, 0.0f});
        m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
        m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();

        SetInstanceId(instance.Id());

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<animation::AnimationGraphAsset>(
            Cast<animation::AnimationGraphAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor",
                               u8"animation graph '{}' failed to read - page opens empty", m_title);
        }
        else
        {
            SyncLayoutArrays();
        }
        m_undoBaseline = SnapshotAsset();

        // ---- center: the state-machine canvas ----
        m_canvas = MakeRef<ui::toolkit::NodeGraphCanvas>(DefaultAllocator());
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
        m_leftRows = MakeRef<ui::FlexLayout>(DefaultAllocator());
        m_leftRows->Direction = ui::Orientation::Vertical;
        m_leftRows->Spacing = 2.0f;
        m_leftRows->Padding = ui::Thickness{6, 6};
        auto leftScroll = MakeRef<ui::ScrollView>(DefaultAllocator());
        leftScroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        leftScroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            auto lp = MakeRef<ui::LayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            leftScroll->AddView(m_leftRows.Get(), lp);
        }

        // ---- right: inspector ----
        m_grid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
        m_inspectorTitle = MakeRef<ui::Label>(DefaultAllocator());
        m_inspectorTitle->FontSize.SetValue(Optional<f32>{12.0f});
        auto inspectorColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        inspectorColumn->Direction = ui::Orientation::Vertical;
        inspectorColumn->Spacing = 4.0f;
        inspectorColumn->Padding = ui::Thickness{6, 4};
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_inspectorTitle.Get(), lp);
            auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            grow->Grow = 1.0f;
            grow->Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_grid.Get(), grow);
        }

        // ---- center-bottom: the live preview strip (transport + wireframe viewport) ----
        BuildPreviewScene();
        m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
        m_viewport->ClearColor = rhi::ClearColor{0.05f, 0.05f, 0.07f, 1.0f};

        auto transport = MakeRef<ui::FlexLayout>(DefaultAllocator());
        transport->Direction = ui::Orientation::Horizontal;
        transport->Spacing = 6.0f;
        transport->Padding = ui::Thickness{6, 4};
        {
            AnimationGraphEditorPage* self = this;
            m_skeletonButton =
                MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Skeleton: (none)"));
            m_skeletonButton->OnClick.Add([self](ui::ButtonBase*) { self->PickPreviewSkeleton(); });
            transport->AddView(m_skeletonButton.Get());
            m_playButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pause"));
            m_playButton->OnClick.Add(
                [self](ui::ButtonBase*)
                {
                    self->m_previewPlaying = !self->m_previewPlaying;
                    self->m_playButton->SetText(self->m_previewPlaying ? StringView(u8"Pause")
                                                                       : StringView(u8"Play"));
                });
            transport->AddView(m_playButton.Get());
            auto restart = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Restart"));
            restart->OnClick.Add([self](ui::ButtonBase*) { self->RebuildPreviewGraph(); });
            transport->AddView(restart.Get());

            m_previewStatus = MakeRef<ui::Label>(DefaultAllocator());
            m_previewStatus->FontSize.SetValue(Optional<f32>{12.0f});
            m_previewStatus->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            grow->Grow = 1.0f;
            transport->AddView(m_previewStatus.Get(), grow);
        }
        auto previewColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        previewColumn->Direction = ui::Orientation::Vertical;
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            previewColumn->AddView(transport.Get(), lp);
            auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            grow->Grow = 1.0f;
            grow->Width = ui::SizeSpec::Match();
            previewColumn->AddView(m_viewport.Get(), grow);
        }
        auto centerSplit = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        centerSplit->Orientation = ui::Orientation::Vertical;
        centerSplit->SetSplitRatio(0.62f);
        centerSplit->SetPanes(m_canvas.Get(), previewColumn.Get());

        auto leftSplit = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        leftSplit->SetSplitRatio(0.18f);
        leftSplit->SetPanes(leftScroll.Get(), centerSplit.Get());
        auto rightSplit = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        rightSplit->SetSplitRatio(0.74f);
        rightSplit->SetPanes(leftSplit.Get(), inspectorColumn.Get());
        m_content = rightSplit;

        RebuildLeftPanel();
        RebuildCanvas();
        RebuildPreviewGraph();
        Select(GraphSel{GraphSelKind::Layer, 0, 0});
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
                auto node = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
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
                auto node = MakeUnique<ui::toolkit::NodeGraphNode>(DefaultAllocator());
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
        auto addRow = [&](StringView text, Function<void()> onClick, bool emphasized)
        {
            auto button = MakeRef<ui::Button>(DefaultAllocator(), text);
            if (emphasized)
            {
                button->AddClass(u8"accent");
            }
            ui::Button* raw = button.Get();
            (void)raw;
            button->OnClick.Add(
                [onClick = Move(onClick)](ui::ButtonBase*) mutable
                {
                    if (onClick)
                    {
                        onClick();
                    }
                });
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(24.0f));
            m_leftRows->AddView(button.Get(), lp);
        };
        auto addHeader = [&](StringView text)
        {
            auto label = MakeRef<ui::Label>(DefaultAllocator());
            label->FontSize.SetValue(Optional<f32>{12.0f});
            label->SetText(text);
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
            m_leftRows->AddView(label.Get(), lp);
        };

        addHeader(u8"Layers");
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
                   layerIndex == m_selectedLayer);
        }
        addRow(u8"+ Add Layer",
               Function<void()>{
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
                   }},
               false);

        addHeader(u8"Parameters");
        for (usize p = 0; p < source.paramNames.Size(); ++p)
        {
            const i32 paramIndex = static_cast<i32>(p);
            addRow(source.paramNames[p].AsView(),
                   Function<void()>{
                       [self, paramIndex]()
                       { self->Select(GraphSel{GraphSelKind::Parameter, 0, paramIndex}); }},
                   m_selected.kind == GraphSelKind::Parameter && m_selected.index == paramIndex);
        }
        addRow(u8"+ Add Parameter",
               Function<void()>{[self]()
                                {
                                    self->QueueStructural(
                                        u8"add-param",
                                        Function<void()>{[self]()
                                                         {
                                                             animation::AnimationGraphSource& s =
                                                                 self->m_asset->source;
                                                             s.paramNames.PushBack(Format(
                                                                 u8"Param{}", s.paramNames.Size()));
                                                             s.paramTypes.PushBack(0); // Float
                                                             s.paramFloats.PushBack(0.0f);
                                                             s.paramInts.PushBack(0);
                                                             s.paramBools.PushBack(0);
                                                         }},
                                        GraphSel{GraphSelKind::Parameter, 0, -2 /*last*/});
                                }},
               false);
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
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
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
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
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
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
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
                UniquePtr<IEditorCommand>(DefaultAllocator().New<EditGraphCommand>(
                                              *self, key.AsView(), self->m_undoBaseline, after),
                                          DefaultAllocator()));
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
            RowButton(g, u8"Delete Layer", cat,
                      [self, layerIdx]()
                      {
                          self->QueueStructural(
                              u8"del-layer",
                              Function<void()>{
                                  [self, layerIdx]()
                                  {
                                      animation::AnimationGraphSource& s = self->m_asset->source;
                                      if (static_cast<usize>(layerIdx) < s.layers.Size() &&
                                          s.layers.Size() > 1)
                                      {
                                          s.layers.RemoveAt(static_cast<usize>(layerIdx));
                                          self->m_asset->layerStatePositions.RemoveAt(
                                              static_cast<usize>(layerIdx));
                                          self->m_asset->layerAnyStatePositions.RemoveAt(
                                              static_cast<usize>(layerIdx));
                                      }
                                  }},
                              GraphSel{GraphSelKind::Layer, 0, 0});
                      });
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
                           DefaultAllocator(), u8"Default", current,
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
                               DefaultAllocator(), u8"Value",
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
                               DefaultAllocator(), u8"Value",
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
                               DefaultAllocator(), u8"Value", m_player->GetBool(pi),
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
        RowButton(g, u8"Delete Parameter", cat,
                  [self, paramIdx]()
                  {
                      self->QueueStructural(
                          u8"del-param",
                          Function<void()>{[self, paramIdx]()
                                           { self->DeleteParameterInternal(paramIdx); }},
                          GraphSel{});
                  });
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
                        DefaultAllocator(), u8"Name", state.name.AsView(),
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
            // Clip picker.
            String label(u8"Clip: ");
            label.Append(AssetLabel(*m_context, node.clipRef).AsView());
            const i32 li = layerIndex, si = stateIndex;
            RowButton(
                g, label.AsView(), kindCat.AsView(),
                [self, li, si]()
                {
                    ui::UIContext* ctx = self->Ctx();
                    if (ctx == nullptr || self->m_context->Project() == nullptr)
                    {
                        return;
                    }
                    Array<String> types;
                    types.PushBack(String(u8"AnimationClipAsset"));
                    auto dialog = MakeRef<app::AssetPickerDialog>(DefaultAllocator(),
                                                                  *self->m_context, Move(types));
                    dialog->OnPicked = [self, li, si](const Guid& picked)
                    {
                        animation::AnimationGraphSource& s = self->m_asset->source;
                        if (static_cast<usize>(li) < s.layers.Size() &&
                            static_cast<usize>(si) < s.layers[static_cast<usize>(li)].states.Size())
                        {
                            s.layers[static_cast<usize>(li)]
                                .states[static_cast<usize>(si)]
                                .node.clipRef = picked;
                            self->CommitEdit(u8"state-clip");
                            self->Select(GraphSel{GraphSelKind::State, li, si});
                        }
                    };
                    dialog->Show(ctx);
                });
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

            // Entries: threshold/position + clip pick + remove; add appends.
            for (usize e = 0; e < node.entryClips.Size(); ++e)
            {
                const String entryCat = Format(u8"Entry {}", e);
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
                                   DefaultAllocator(), u8"Position", node.entryPositions[e],
                                   -1000.0f, 1000.0f, 0.05f,
                                   Function<void(Float2)>{[&node, e, page](Float2 v)
                                                          {
                                                              node.entryPositions[e] = v;
                                                              page->CommitEdit(u8"entry-pos");
                                                          }},
                                   entryCat.AsView())
                                   .Get()));
                }
                String clipLabel(u8"Clip: ");
                clipLabel.Append(AssetLabel(*m_context, node.entryClips[e]).AsView());
                const i32 li = layerIndex, si = stateIndex;
                const usize entryIdx = e;
                RowButton(g, clipLabel.AsView(), entryCat.AsView(),
                          [self, li, si, entryIdx]()
                          {
                              ui::UIContext* ctx = self->Ctx();
                              if (ctx == nullptr || self->m_context->Project() == nullptr)
                              {
                                  return;
                              }
                              Array<String> types;
                              types.PushBack(String(u8"AnimationClipAsset"));
                              auto dialog = MakeRef<app::AssetPickerDialog>(
                                  DefaultAllocator(), *self->m_context, Move(types));
                              dialog->OnPicked = [self, li, si, entryIdx](const Guid& picked)
                              {
                                  animation::AnimationGraphSource& s = self->m_asset->source;
                                  if (static_cast<usize>(li) < s.layers.Size() &&
                                      static_cast<usize>(si) <
                                          s.layers[static_cast<usize>(li)].states.Size())
                                  {
                                      animation::GraphNodeData& n =
                                          s.layers[static_cast<usize>(li)]
                                              .states[static_cast<usize>(si)]
                                              .node;
                                      if (entryIdx < n.entryClips.Size())
                                      {
                                          n.entryClips[entryIdx] = picked;
                                          self->CommitEdit(u8"entry-clip");
                                          self->Select(GraphSel{GraphSelKind::State, li, si});
                                      }
                                  }
                              };
                              dialog->Show(ctx);
                          });
                RowButton(g, u8"Remove Entry", entryCat.AsView(),
                          [self, li, si, entryIdx]()
                          {
                              self->QueueStructural(
                                  u8"del-entry",
                                  Function<void()>{
                                      [self, li, si, entryIdx]()
                                      {
                                          animation::AnimationGraphSource& s =
                                              self->m_asset->source;
                                          if (static_cast<usize>(li) >= s.layers.Size() ||
                                              static_cast<usize>(si) >=
                                                  s.layers[static_cast<usize>(li)].states.Size())
                                          {
                                              return;
                                          }
                                          animation::GraphNodeData& n =
                                              s.layers[static_cast<usize>(li)]
                                                  .states[static_cast<usize>(si)]
                                                  .node;
                                          if (entryIdx < n.entryClips.Size())
                                          {
                                              n.entryClips.RemoveAt(entryIdx);
                                          }
                                          if (entryIdx < n.entryThresholds.Size())
                                          {
                                              n.entryThresholds.RemoveAt(entryIdx);
                                          }
                                          if (entryIdx < n.entryPositions.Size())
                                          {
                                              n.entryPositions.RemoveAt(entryIdx);
                                          }
                                      }},
                                  GraphSel{GraphSelKind::State, li, si});
                          });
            }
            {
                const i32 li = layerIndex, si = stateIndex;
                RowButton(g, u8"+ Add Entry", kindCat.AsView(),
                          [self, li, si]()
                          {
                              self->QueueStructural(
                                  u8"add-entry",
                                  Function<void()>{
                                      [self, li, si]()
                                      {
                                          animation::AnimationGraphSource& s =
                                              self->m_asset->source;
                                          if (static_cast<usize>(li) >= s.layers.Size() ||
                                              static_cast<usize>(si) >=
                                                  s.layers[static_cast<usize>(li)].states.Size())
                                          {
                                              return;
                                          }
                                          animation::GraphNodeData& n =
                                              s.layers[static_cast<usize>(li)]
                                                  .states[static_cast<usize>(si)]
                                                  .node;
                                          n.entryClips.PushBack(Guid{});
                                          n.entryThresholds.PushBack(0.0f);
                                          n.entryPositions.PushBack(Float2{0.0f, 0.0f});
                                      }},
                                  GraphSel{GraphSelKind::State, li, si});
                          });
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

        // Conditions.
        for (usize c = 0; c < transition.conditions.Size(); ++c)
        {
            animation::GraphConditionData& condition = transition.conditions[c];
            const String condCat = Format(u8"Condition {}", c);
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
            const i32 li = layerIndex, ti = transitionIndex;
            const usize condIdx = c;
            RowButton(g, u8"Remove Condition", condCat.AsView(),
                      [self, li, ti, condIdx]()
                      {
                          self->QueueStructural(
                              u8"del-cond",
                              Function<void()>{
                                  [self, li, ti, condIdx]()
                                  {
                                      animation::AnimationGraphSource& s = self->m_asset->source;
                                      if (static_cast<usize>(li) >= s.layers.Size())
                                      {
                                          return;
                                      }
                                      Array<animation::GraphTransitionData>& transitions =
                                          s.layers[static_cast<usize>(li)].transitions;
                                      if (static_cast<usize>(ti) < transitions.Size() &&
                                          condIdx <
                                              transitions[static_cast<usize>(ti)].conditions.Size())
                                      {
                                          transitions[static_cast<usize>(ti)].conditions.RemoveAt(
                                              condIdx);
                                      }
                                  }},
                              GraphSel{GraphSelKind::Transition, li, ti});
                      });
        }
        {
            const i32 li = layerIndex, ti = transitionIndex;
            RowButton(g, u8"+ Add Condition", cat,
                      [self, li, ti]()
                      {
                          self->QueueStructural(
                              u8"add-cond",
                              Function<void()>{
                                  [self, li, ti]()
                                  {
                                      animation::AnimationGraphSource& s = self->m_asset->source;
                                      if (static_cast<usize>(li) >= s.layers.Size())
                                      {
                                          return;
                                      }
                                      Array<animation::GraphTransitionData>& transitions =
                                          s.layers[static_cast<usize>(li)].transitions;
                                      if (static_cast<usize>(ti) < transitions.Size())
                                      {
                                          transitions[static_cast<usize>(ti)].conditions.PushBack(
                                              animation::GraphConditionData{});
                                      }
                                  }},
                              GraphSel{GraphSelKind::Transition, li, ti});
                      });
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
            DefaultAllocator().New<EditGraphCommand>(*this, mergeKey, m_undoBaseline, after),
            DefaultAllocator()));
        m_undoBaseline = Move(after);
        RebuildPreviewGraph();
        MarkDirty();
    }

    // ============================ Live preview ==============================================

    void AnimationGraphEditorPage::BuildPreviewScene()
    {
        if (m_scenes == nullptr)
        {
            return;
        }
        m_sceneManager.SetAwareRegistry(&m_scenes->AwareRegistry());
        m_scenes->RegisterManager(&m_sceneManager);
        m_scene = m_sceneManager.CreateScene(u8"animgraph.preview");
    }

    void AnimationGraphEditorPage::PickPreviewSkeleton()
    {
        ui::UIContext* ctx = Ctx();
        if (ctx == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        AnimationGraphEditorPage* self = this;
        Array<String> types;
        types.PushBack(String(u8"SkeletonAsset"));
        auto dialog = MakeRef<app::AssetPickerDialog>(DefaultAllocator(), *m_context, Move(types));
        dialog->OnPicked = [self](const Guid& picked)
        {
            self->m_skeletonGuid = picked;
            if (self->m_context->Resources() != nullptr && !picked.IsNil())
            {
                self->m_skeleton = self->m_context->Resources()->Bind<animation::Skeleton>(picked);
            }
            else
            {
                self->m_skeleton = draconic::resource::Proxy<animation::Skeleton>{};
            }
            String label(u8"Skeleton: ");
            label.Append(AssetLabel(*self->m_context, picked).AsView());
            self->m_skeletonButton->SetText(label.AsView());
            self->RebuildPreviewGraph();
        };
        dialog->Show(ctx);
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
        m_previewGraph = MakeRef<animation::AnimationGraph>(DefaultAllocator());
        m_asset->source.BuildInto(*m_context->Resources(), *m_previewGraph);
        if (m_previewGraph->Layers().IsEmpty())
        {
            return;
        }
        m_player = MakeUnique<animation::AnimationGraphPlayer>(DefaultAllocator(), *m_previewGraph,
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

        // Bone wireframe + ground grid into the preview scene's debug lane.
        if (m_render == nullptr || !m_render->IsReady() || m_scene == nullptr)
        {
            return;
        }
        auto& draw = m_render->DebugScene(*m_scene);
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
        DrawSkeletonWireframe(draw, *m_playerSkeleton, m_player->GetLocalPoses(), m_worldScratch);
    }

    void DrawSkeletonWireframe(draconic::render::debug::DebugDraw& draw,
                               animation::Skeleton& skeleton,
                               Span<const animation::BoneTransform> localPoses,
                               Array<Float4x4>& worldScratch)
    {
        const usize boneCount = static_cast<usize>(skeleton.BoneCount());
        if (boneCount == 0 || localPoses.Size() < boneCount)
        {
            return;
        }
        worldScratch.Resize(boneCount);
        skeleton.ComputeWorldPoses(localPoses, Span<Float4x4>{worldScratch.Data(), boneCount});
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

    void AnimationGraphEditorPage::EnsureViewportBound()
    {
        draconic::ui::RootView* root = m_viewport->Root();
        if (root == nullptr)
        {
            return;
        }
        draconic::graphics::RenderWindow* window = m_uiHost->WindowForRoot(root);
        if (window == nullptr || window == m_hostWindow)
        {
            return;
        }
        vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(window);
        if (renderer == nullptr)
        {
            return;
        }
        if (m_hostWindow == nullptr)
        {
            m_viewport->Initialize(m_host->Graphics()->Raw(), renderer, m_host->Shell()->Input(),
                                   window->Window().Id());
            if (m_viewport->Surface() != nullptr)
            {
                m_router->AddSurface(m_viewport->Surface());
            }
        }
        else
        {
            m_viewport->AttachToWindow(renderer, window->Window().Id());
        }
        m_hostWindow = window;
    }

    // ============================ Frame / save / close ======================================

    void AnimationGraphEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        UpdatePreview(dt);
        EnsureViewportBound();
        if (m_hostWindow == nullptr)
        {
            return;
        }
        m_viewport->SyncInputRegion();
        if (m_router)
        {
            m_router->Update();
        }
        if (m_viewport->IsHovered() || m_viewport->IsFocused())
        {
            m_camera.Update(m_viewport->Keyboard(), m_viewport->Mouse(), dt);
        }
    }

    void AnimationGraphEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                                  draconic::graphics::FrameContext& frame)
    {
        if (!m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        if (m_render == nullptr || !m_render->IsReady() || m_scene == nullptr)
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0 || !m_viewport->IsEffectivelyVisible())
        {
            return;
        }

        render::ViewCamera camera;
        camera.view = Float4x4::LookAtRH(m_camera.position, m_camera.position + m_camera.Forward(),
                                         m_camera.Up());
        camera.projection = Float4x4::PerspectiveFovRH(
            1.0472f, static_cast<f32>(w) / static_cast<f32>(h), 0.05f, 200.0f);
        camera.position = m_camera.position;
        camera.farZ = 200.0f;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        cameraOverride.clearColor = Color{m_viewport->ClearColor.r, m_viewport->ClearColor.g,
                                          m_viewport->ClearColor.b, m_viewport->ClearColor.a};

        render::TargetState targetState;
        targetState.texture = m_viewport->ColorTexture();
        targetState.currentState = m_viewport->ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;

        m_render->RenderScene(*m_scene, m_viewport->ColorTargetView(), m_viewport->ColorFormat(), w,
                              h, render::ViewportRect{0, 0, w, h}, &cameraOverride, targetState);
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
    }

    Status AnimationGraphEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        draconic::content::Instance* instance =
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
            DRACONIC_LOG_INFO(u8"Editor", u8"saved animation graph '{}'", m_title);
        }
        return saved;
    }

    void AnimationGraphEditorPage::OnClose()
    {
        m_player.Reset();
        m_previewGraph = RefPtr<animation::AnimationGraph>{};
        if (m_viewport.Get() != nullptr)
        {
            m_viewport->Shutdown();
        }
        if (m_scene != nullptr)
        {
            m_sceneManager.DestroyScene(m_scene);
            m_scene = nullptr;
        }
        if (m_scenes != nullptr)
        {
            m_scenes->UnregisterManager(&m_sceneManager);
        }
    }

    // ============================ Factory / creator =========================================

    const TypeInfo* AnimationGraphPageFactory::PrimaryType() const
    {
        return &animation::AnimationGraphAsset::StaticType();
    }

    UniquePtr<EditorPage>
    AnimationGraphPageFactory::CreatePage(EditorContext& context,
                                          draconic::content::Instance& instance)
    {
        auto* page =
            DefaultAllocator().New<AnimationGraphEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }

    void SeedDefaultAnimationGraph(animation::AnimationGraphAsset& asset)
    {
        animation::AnimationGraphSource& source = asset.source;
        source.paramNames.PushBack(String(u8"Speed"));
        source.paramTypes.PushBack(0); // Float
        source.paramFloats.PushBack(0.0f);
        source.paramInts.PushBack(0);
        source.paramBools.PushBack(0);

        animation::GraphLayerData layer;
        layer.name = String(u8"Base");
        animation::GraphStateData idle;
        idle.name = String(u8"Idle");
        idle.node.kind = 0; // clip (unassigned - pick in the inspector)
        layer.states.PushBack(Move(idle));
        layer.defaultState = 0;
        source.layers.PushBack(Move(layer));

        Array<Float2> positions;
        positions.PushBack(Float2{280.0f, 120.0f});
        asset.layerStatePositions.PushBack(Move(positions));
        asset.layerAnyStatePositions.PushBack(Float2{60.0f, 40.0f});
    }

    inline draconic::content::Instance*
    CreateAnimationGraphInstance(EditorContext& context, draconic::content::Group* group)
    {
        if (context.Project() == nullptr)
        {
            return nullptr;
        }
        draconic::content::Group* target = group;
        if (target == nullptr)
        {
            draconic::content::Group* root = context.Project()->SourceDb().RootGroup();
            target = root->GetGroup(u8"Animation");
            if (target == nullptr)
            {
                target = root->CreateGroup(u8"Animation");
            }
        }
        if (target == nullptr)
        {
            return nullptr;
        }

        const String name = target->UniqueInstanceName(u8"AnimationGraph");

        draconic::content::Instance* instance =
            target->CreateInstance(name.AsView(), animation::AnimationGraphAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        animation::AnimationGraphAsset asset;
        SeedDefaultAnimationGraph(asset);
        if (!instance->WriteObject(asset).IsOk())
        {
            return nullptr;
        }
        DRACONIC_LOG_INFO(u8"Editor", u8"created animation graph '{}'", instance->Path());
        context.RequestCook(false);
        return instance;
    }

    void RegisterAnimationGraphEditor(EditorContext& context, runtime::IApplicationHost& host,
                                      ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<AnimationGraphPageFactory>(host, uiHost), DefaultAllocator()));

        EditorContext::AssetCreator creator;
        creator.label = String(u8"Animation Graph");
        creator.create = [](EditorContext& ctx, draconic::content::Group* group)
        { return CreateAnimationGraphInstance(ctx, group); };
        context.RegisterCreator(Move(creator));
    }
}
