// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :skeleton_page partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.scene;

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
import foundation.resource;
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
import :animation_graph_page; // DrawSkeletonWireframe (shared preview helper, impl-only)

using namespace foundation::core;
namespace animation = foundation::animation;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace ui = foundation::ui;
namespace vg = foundation::vg;

namespace editor
{
    // ============================ Tree adapter ==============================================

    i32 SkeletonTreeAdapter::RootCount() const { return static_cast<i32>(m_owner->m_roots.Size()); }
    i32 SkeletonTreeAdapter::GetChildCount(i32 nodeId) const
    {
        if (nodeId == -1)
        {
            return RootCount();
        }
        if (nodeId < 0 || nodeId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return 0;
        }
        return static_cast<i32>(m_owner->m_nodes[static_cast<usize>(nodeId)].children.Size());
    }
    i32 SkeletonTreeAdapter::GetChildId(i32 parentId, i32 childIndex) const
    {
        if (parentId == -1)
        {
            return (childIndex >= 0 && childIndex < RootCount())
                       ? m_owner->m_roots[static_cast<usize>(childIndex)]
                       : -1;
        }
        if (parentId < 0 || parentId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return -1;
        }
        const Array<i32>& kids = m_owner->m_nodes[static_cast<usize>(parentId)].children;
        return (childIndex >= 0 && childIndex < static_cast<i32>(kids.Size()))
                   ? kids[static_cast<usize>(childIndex)]
                   : -1;
    }
    i32 SkeletonTreeAdapter::GetDepth(i32 nodeId) const
    {
        return (nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_nodes.Size()))
                   ? m_owner->m_nodes[static_cast<usize>(nodeId)].depth
                   : 0;
    }
    bool SkeletonTreeAdapter::HasChildren(i32 nodeId) const { return GetChildCount(nodeId) > 0; }
    RefPtr<ui::View> SkeletonTreeAdapter::CreateView(i32)
    {
        auto row = MakeRef<ui::EditableLabel>(editor::EditorRootAllocator());
        row->FontSize.SetValue(Optional<f32>{12.0f});
        row->Ellipsis.SetValue(true);
        row->DoubleClickToEdit.SetValue(false);
        row->SlowClickToEdit.SetValue(false);
        return RefPtr<ui::View>(row.Get());
    }
    void SkeletonTreeAdapter::BindView(ui::View* view, i32 nodeId, i32 depth, bool)
    {
        animation::Skeleton* skeleton = m_owner->m_skeleton.Get();
        if (skeleton == nullptr || nodeId < 0 ||
            nodeId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return;
        }
        const i32 boneIndex = m_owner->m_nodes[static_cast<usize>(nodeId)].boneIndex;
        const animation::Bone* bone = skeleton->GetBone(boneIndex);
        auto* row = static_cast<ui::EditableLabel*>(view);
        row->SetText(bone != nullptr ? bone->name.AsView() : StringView(u8"?"));
        row->TextOffsetX.SetValue(m_owner->m_tree->ContentInset(depth));
    }

    // ============================ Construction ==============================================

    SkeletonEditorPage::SkeletonEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                           ui::runtime::UIHost& uiHost,
                                           foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // Shared preview substrate (viewport + preview scene + orbit camera + render loop).
        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"skeleton.preview");
        m_preview->SetClearColor(Color{0.248f, 0.248f, 0.293f, 1.0f});
        m_preview->Camera().position = Float3{0.0f, 1.4f, 3.2f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});

        SetInstanceId(instance.Id());
        if (m_context->Resources() != nullptr)
        {
            m_skeleton = m_context->Resources()->Bind<animation::Skeleton>(InstanceId());
        }

        // Left: the bone tree under a stats line.
        m_adapter = MakeUnique<SkeletonTreeAdapter>(Allocator(), *this);
        m_tree = MakeRef<ui::toolkit::DraggableTreeView>(Allocator());
        m_tree->SetItemHeight(22.0f);
        m_tree->SetDragEnabled(false);
        m_tree->SetAdapter(m_adapter.Get());
        {
            SkeletonEditorPage* self = this;
            m_tree->InternalTreeView()->OnItemClick.Add(
                [self](ui::TreeView::ItemClickInfo info)
                {
                    if (info.NodeId >= 0 && info.NodeId < static_cast<i32>(self->m_nodes.Size()))
                    {
                        self->m_selectedBone =
                            self->m_nodes[static_cast<usize>(info.NodeId)].boneIndex;
                        ui::UIContext* ctx = self->Ctx();
                        if (ctx != nullptr)
                        {
                            ctx->MutationQueueRef().QueueAction(
                                Function<void()>{[self]() { self->RebuildInfoPane(); }});
                        }
                        else
                        {
                            self->RebuildInfoPane();
                        }
                    }
                });
        }
        m_statsLabel = MakeRef<ui::Label>(Allocator());
        m_statsLabel->FontSize.SetValue(Optional<f32>{12.0f});
        auto leftColumn = MakeRef<ui::FlexLayout>(Allocator());
        leftColumn->Direction = ui::Orientation::Vertical;
        leftColumn->Spacing = 4.0f;
        leftColumn->Padding = ui::Thickness{6, 4};
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            leftColumn->AddView(m_statsLabel.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            leftColumn->AddView(m_tree.Get(), grow);
        }

        // Right: read-only info for the selected bone.
        m_grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());

        auto centerSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        centerSplit->SetSplitRatio(0.26f);
        centerSplit->SetPanes(leftColumn.Get(), m_preview->View());
        auto outerSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        outerSplit->SetSplitRatio(0.78f);
        outerSplit->SetPanes(centerSplit.Get(), m_grid.Get());
        m_content = outerSplit;

        RebuildTree();
        RebuildInfoPane();
    }

    // ============================ Model / panes =============================================

    void SkeletonEditorPage::RebuildTree()
    {
        m_nodes.Clear();
        m_roots.Clear();
        animation::Skeleton* skeleton = m_skeleton.Get();
        if (skeleton != nullptr)
        {
            const i32 boneCount = skeleton->BoneCount();
            m_nodes.Resize(static_cast<usize>(boneCount));
            // Node id == bone index; children + depth from the parent links (parents are
            // guaranteed to precede children in import order, so one pass suffices).
            for (i32 i = 0; i < boneCount; ++i)
            {
                BoneNode& node = m_nodes[static_cast<usize>(i)];
                node.boneIndex = i;
                const animation::Bone* bone = skeleton->GetBone(i);
                const i32 parent = (bone != nullptr) ? bone->parentIndex : -1;
                if (parent >= 0 && parent < boneCount)
                {
                    node.depth = m_nodes[static_cast<usize>(parent)].depth + 1;
                    m_nodes[static_cast<usize>(parent)].children.PushBack(i);
                }
                else
                {
                    node.depth = 0;
                    m_roots.PushBack(i);
                }
            }
            const Array<String> stats = SkeletonStatLines(*skeleton);
            String text;
            for (usize i = 0; i < stats.Size(); ++i)
            {
                if (i > 0)
                {
                    text.Append(u8"  |  ");
                }
                text.Append(stats[i].AsView());
            }
            m_statsLabel->SetText(text.AsView());
        }
        else
        {
            m_statsLabel->SetText(u8"(skeleton not cooked yet)");
        }
        m_tree->SetAdapter(m_adapter.Get());
        if (ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter())
        {
            HashSet<i32> expanded;
            for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); ++i)
            {
                expanded.Insert(i);
            }
            flat->SetExpandedNodes(expanded);
        }
        m_tree->InternalTreeView()->InternalListView()->NotifyDataChanged();
    }

    void SkeletonEditorPage::RebuildInfoPane()
    {
        m_grid->Clear();
        animation::Skeleton* skeleton = m_skeleton.Get();
        const animation::Bone* bone =
            (skeleton != nullptr) ? skeleton->GetBone(m_selectedBone) : nullptr;
        if (bone == nullptr)
        {
            return;
        }
        const StringView cat = u8"Bone";
        auto stat = [&](StringView name, String value)
        {
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::StringEditor>(Allocator(), name, value.AsView(),
                                                   Function<void(StringView)>{}, cat)
                    .Get()));
        };
        stat(u8"Name", String(bone->name.AsView()));
        stat(u8"Index", Format(u8"{}", bone->index));
        const animation::Bone* parent =
            (bone->parentIndex >= 0) ? skeleton->GetBone(bone->parentIndex) : nullptr;
        stat(u8"Parent", parent != nullptr ? String(parent->name.AsView()) : String(u8"(root)"));
        stat(u8"Children", Format(u8"{}", bone->children.Size()));
        const animation::BoneTransform& bind = bone->localBindPose;
        stat(u8"Bind Position",
             Format(u8"{}, {}, {}", bind.position.x, bind.position.y, bind.position.z));
        stat(u8"Bind Rotation", Format(u8"{}, {}, {}, {}", bind.rotation.x, bind.rotation.y,
                                       bind.rotation.z, bind.rotation.w));
        stat(u8"Bind Scale", Format(u8"{}, {}, {}", bind.scale.x, bind.scale.y, bind.scale.z));
    }

    // ============================ Preview ===================================================

    void SkeletonEditorPage::UpdatePreview()
    {
        animation::Skeleton* skeleton = m_skeleton.Get();
        if (skeleton == nullptr || skeleton->BoneCount() <= 0 || m_preview.Get() == nullptr ||
            !m_preview->IsValid())
        {
            return;
        }
        const usize boneCount = static_cast<usize>(skeleton->BoneCount());
        // Local poses = the BIND pose (this page shows the skeleton at rest).
        m_poseScratch.Resize(boneCount);
        for (usize b = 0; b < boneCount; ++b)
        {
            const animation::Bone* bone = skeleton->GetBone(static_cast<i32>(b));
            m_poseScratch[b] = (bone != nullptr) ? bone->localBindPose : animation::BoneTransform{};
        }
        auto& draw = m_preview->SceneDebugDraw();
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
        DrawSkeletonWireframe(draw, *skeleton,
                              Span<const animation::BoneTransform>{m_poseScratch.Data(), boneCount},
                              m_worldScratch);

        // Selected-bone emphasis: the wireframe filled m_worldScratch, so read its position back.
        if (m_selectedBone >= 0 && static_cast<usize>(m_selectedBone) < m_worldScratch.Size())
        {
            const Float4x4& world = m_worldScratch[static_cast<usize>(m_selectedBone)];
            const Float3 pos{world.m[3][0], world.m[3][1], world.m[3][2]};
            draw.DrawCross(pos, 0.06f, Color{1.0f, 0.65f, 0.2f, 1.0f});
            draw.DrawWireSphere(pos, 0.03f, Color{1.0f, 0.65f, 0.2f, 1.0f}, 12);
        }
    }

    // ============================ Frame / close =============================================

    ui::UIContext* SkeletonEditorPage::Ctx() const
    {
        return (m_tree.Get() != nullptr && m_tree->InternalTreeView() != nullptr)
                   ? m_tree->InternalTreeView()->Context
                   : nullptr;
    }

    void SkeletonEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        // Product resolve / hot-reload watchdog (pointer identity, like the graph page).
        if (m_skeleton.Get() != m_lastSkeleton)
        {
            m_lastSkeleton = m_skeleton.Get();
            m_selectedBone = -1;
            RebuildTree();
            RebuildInfoPane();
        }
        UpdatePreview();

        if (m_preview)
        {
            m_preview->Update(dt);
        }
    }

    void SkeletonEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                            foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    void SkeletonEditorPage::OnClose()
    {
        if (m_tree.Get() != nullptr)
        {
            m_tree->SetAdapter(nullptr);
        }
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    // ============================ Factory / stats ===========================================

    const TypeInfo* SkeletonPageFactory::PrimaryType() const
    {
        return &pipeline::SkeletonAsset::StaticType();
    }

    UniquePtr<EditorPage> SkeletonPageFactory::CreatePage(EditorContext& context,
                                                          foundation::content::Instance& instance)
    {
        auto* page =
            editor::EditorRootAllocator().New<SkeletonEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    Array<String> SkeletonStatLines(const animation::Skeleton& skeleton)
    {
        Array<String> lines;
        if (!skeleton.Name().IsEmpty())
        {
            lines.PushBack(String(skeleton.Name().AsView()));
        }
        lines.PushBack(Format(u8"{} bones", skeleton.BoneCount()));
        i32 roots = 0;
        i32 maxDepth = 0;
        for (i32 i = 0; i < skeleton.BoneCount(); ++i)
        {
            const animation::Bone* bone = skeleton.GetBone(i);
            if (bone == nullptr)
            {
                continue;
            }
            if (bone->parentIndex < 0)
            {
                ++roots;
            }
            i32 depth = 0;
            for (const animation::Bone* walk = bone;
                 walk != nullptr && walk->parentIndex >= 0 && depth < skeleton.BoneCount();
                 walk = skeleton.GetBone(walk->parentIndex))
            {
                ++depth;
            }
            maxDepth = Max(maxDepth, depth);
        }
        lines.PushBack(Format(u8"{} roots", roots));
        lines.PushBack(Format(u8"depth {}", maxDepth));
        return lines;
    }

    void RegisterSkeletonEditor(EditorContext& context, runtime::IApplicationHost& host,
                                ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<SkeletonPageFactory>(host, uiHost), editor::EditorRootAllocator()));
    }
}
