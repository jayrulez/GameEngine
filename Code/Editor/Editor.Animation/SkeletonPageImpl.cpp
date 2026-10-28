// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:skeleton_page` partition (implementation).

module;
#include <cmath> // std::acos (a rotation's angle)
#include "Core/Prelude.h"

module editor.animation;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace animation = foundation::animation;

    namespace
    {
        // Three decimals, a trailing zero dropped: what a bind pose's numbers need, and no more.
        [[nodiscard]] String Short(f32 value)
        {
            const f64 rounded = static_cast<f64>(static_cast<i64>(static_cast<f64>(value) * 1000.0 + (value < 0.0f ? -0.5 : 0.5))) / 1000.0;
            return Format(u8"{}", rounded == 0.0 ? 0.0 : rounded);
        }
        [[nodiscard]] String Describe(Float3 v)
        {
            return Format(u8"{}, {}, {}", Short(v.x), Short(v.y), Short(v.z));
        }

        void AddFact(gui::PropertyBag& rows, StringView path, StringView shown)
        {
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::String, gui::Variant(StringView{}));
            info.DisplayName = String(shown);
            info.ReadOnly = true;
        }
    }

    SkeletonFacts FactsOf(const animation::Skeleton& skeleton)
    {
        SkeletonFacts facts;
        facts.bones = skeleton.BoneCount();
        for (i32 i = 0; i < skeleton.BoneCount(); ++i)
        {
            const animation::Bone* bone = skeleton.GetBone(i);
            if (bone == nullptr)
                continue;
            if (bone->parentIndex < 0)
                ++facts.roots;
            i32 depth = 0;
            for (const animation::Bone* walk = bone; walk != nullptr && walk->parentIndex >= 0 && depth < skeleton.BoneCount();
                 walk = skeleton.GetBone(walk->parentIndex))
                ++depth;
            facts.depth = Max(facts.depth, depth);
        }
        return facts;
    }

    String DescribeRotation(const Quaternion& rotation)
    {
        Quaternion q = rotation;
        const f32 length = Sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (length < 1e-6f)
            return String(u8"None");
        q = Quaternion{q.x / length, q.y / length, q.z / length, q.w / length};
        if (q.w < -1e-5f) // the shorter way round (a half turn, w about 0, either way)
            q = Quaternion{-q.x, -q.y, -q.z, -q.w};
        const f32 angle = 2.0f * static_cast<f32>(std::acos(static_cast<f64>(Clamp(q.w, -1.0f, 1.0f))));
        const f32 degrees = angle * 180.0f / kPi;
        const f32 s = Sqrt(Max(1.0f - q.w * q.w, 0.0f));
        if (degrees < 0.05f || s < 1e-5f)
            return String(u8"None");
        return Format(u8"{}° about {}", Short(degrees), Describe(Float3{q.x / s, q.y / s, q.z / s}));
    }

    SkeletonEditorPage::SkeletonEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::SkeletonAsset::StaticType(), kPath)
    {
        HideAssetRows(); // its source is its import's: the rows are what it is, read only
        SkeletonEditorPage* self = this;
        m_model = MakeUnique<gui::TreeModel>(Allocator());
        m_tree = Find<gui::TreeView>(u8"skeleton-tree");
        if (m_tree != nullptr)
        {
            m_tree->SetModel(m_model.Get());
            m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                          {
                                              if (self->m_showing || !index.IsValid())
                                                  return;
                                              const usize node = static_cast<usize>(index.InternalId);
                                              if (node < self->m_rows.Size())
                                                  self->ChooseBone(self->m_rows[node]);
                                          });
        }
        if (auto* search = Find<gui::TextField>(u8"skeleton-search"))
            search->SetOnTextChanging([self](StringView text) { self->Search(text); });

        m_boneRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        AddFact(*m_boneRows, u8"name", u8"Name");
        AddFact(*m_boneRows, u8"index", u8"Index");
        AddFact(*m_boneRows, u8"parent", u8"Parent");
        AddFact(*m_boneRows, u8"children", u8"Children");
        AddFact(*m_boneRows, u8"position", u8"Bind position");
        AddFact(*m_boneRows, u8"rotation", u8"Bind rotation");
        AddFact(*m_boneRows, u8"scale", u8"Bind scale");
        AddSection(u8"bone", u8"Bone", *m_boneRows);

        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"skeleton.preview");
            m_pane->OnFrame = [self]() { self->FramePreview(); };
        }
        BuildSkeleton();
        ChooseBone(m_skeleton && m_skeleton->BoneCount() > 0 ? 0 : -1);
    }

    SkeletonEditorPage::~SkeletonEditorPage()
    {
        if (m_tree != nullptr)
            m_tree->SetModel(nullptr);
    }

    void SkeletonEditorPage::BuildSkeleton()
    {
        const auto* asset = ObjectAs<pipeline::SkeletonAsset>();
        m_skeleton = MakeRef<animation::Skeleton>(Allocator());
        if (asset != nullptr)
            asset->source.FillSkeleton(*m_skeleton);
        m_framed = false;
        ShowTree();
        ShowFacts();
    }

    void SkeletonEditorPage::OnObjectChanged()
    {
        // Read again from its source (a page_reload, a re-import): its bones may be others now.
        BuildSkeleton();
        ChooseBone(m_chosen < (m_skeleton ? m_skeleton->BoneCount() : 0) ? m_chosen : -1);
    }

    void SkeletonEditorPage::ShowFacts()
    {
        if (!m_skeleton)
            return;
        const SkeletonFacts facts = FactsOf(*m_skeleton);
        SetFact(u8"Bones", Format(u8"{}", facts.bones).AsView());
        SetFact(u8"Roots", Format(u8"{}", facts.roots).AsView());
        SetFact(u8"Deepest chain", Format(u8"{} below its root", facts.depth).AsView());
    }

    void SkeletonEditorPage::Search(StringView text)
    {
        m_search = String(Trim(text));
        ShowTree();
    }

    void SkeletonEditorPage::ShowTree()
    {
        if (!m_skeleton)
            return;
        m_showing = true;
        const i32 count = m_skeleton->BoneCount();
        // A bone the user folded stays folded through the rebuild (a search, a reload).
        Array<i32> folded(Allocator());
        if (m_tree != nullptr)
            for (usize node = 0; node < m_rows.Size(); ++node)
                if (!m_tree->IsIdExpanded(static_cast<gui::ItemId>(node)))
                    folded.PushBack(m_rows[node]);
        // A search keeps the bones whose names hold it, and every bone above one (where it hangs).
        Array<bool> kept;
        kept.Resize(static_cast<usize>(count), m_search.IsEmpty());
        if (!m_search.IsEmpty())
            for (i32 b = 0; b < count; ++b)
                if (m_skeleton->GetBone(b)->name.AsView().ContainsIgnoreCase(m_search.AsView()))
                    for (i32 walk = b; walk >= 0 && walk < count && !kept[static_cast<usize>(walk)]; walk = m_skeleton->GetBone(walk)->parentIndex)
                        kept[static_cast<usize>(walk)] = true;
        m_model->Clear();
        m_rows.Clear();
        i32 chosen = -1;
        // Each bone under its parent, in the skeleton's order among its siblings.
        const Function<void(i32, i32)> add = [&](i32 bone, i32 parentNode)
        {
            const animation::Bone* data = m_skeleton->GetBone(bone);
            const i32 node = m_model->AddNode(parentNode, data->name.IsEmpty() ? Format(u8"Bone {}", bone).AsView() : data->name.AsView());
            m_model->SetIcon(node, u8"Editor/Icons/Types/SkeletonAsset.svg", u8"cat-animation");
            m_rows.PushBack(bone);
            if (bone == m_chosen)
                chosen = node;
            for (i32 child = 0; child < count; ++child)
                if (m_skeleton->GetBone(child)->parentIndex == bone && kept[static_cast<usize>(child)])
                    add(child, node);
        };
        for (i32 b = 0; b < count; ++b)
        {
            const i32 parent = m_skeleton->GetBone(b)->parentIndex;
            if ((parent < 0 || parent >= count) && kept[static_cast<usize>(b)])
                add(b, gui::kTreeModelRoot);
        }
        if (m_tree != nullptr)
        {
            for (usize node = 0; node < m_rows.Size(); ++node)
            {
                bool wasFolded = false;
                for (const i32 bone : folded)
                    wasFolded = wasFolded || bone == m_rows[node];
                m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), !wasFolded || !m_search.IsEmpty()); // a search opens what it found
            }
            if (chosen >= 0)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(chosen);
                m_tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
        }
        m_showing = false;
    }

    void SkeletonEditorPage::ChooseBone(i32 bone)
    {
        m_chosen = m_skeleton && bone >= 0 && bone < m_skeleton->BoneCount() ? bone : -1;
        ShowBone();
        if (m_tree == nullptr)
            return;
        m_showing = true;
        for (usize node = 0; node < m_rows.Size(); ++node)
            if (m_rows[node] == m_chosen)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(node);
                m_tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
        m_showing = false;
    }

    void SkeletonEditorPage::ShowBone()
    {
        const animation::Bone* bone = m_chosen >= 0 ? m_skeleton->GetBone(m_chosen) : nullptr;
        const auto set = [this](StringView path, StringView value)
        {
            if (m_boneRows->GetValue(path, 0).AsString() != value)
                m_boneRows->SetValue(path, 0, gui::Variant(value));
        };
        if (bone == nullptr)
        {
            constexpr StringView kRows[] = {u8"index", u8"parent", u8"children", u8"position", u8"rotation", u8"scale"};
            set(u8"name", u8"None chosen");
            for (const StringView path : kRows)
                set(path, StringView{});
            return;
        }
        const animation::Bone* parent = bone->parentIndex >= 0 ? m_skeleton->GetBone(bone->parentIndex) : nullptr;
        i32 children = 0;
        for (i32 b = 0; b < m_skeleton->BoneCount(); ++b)
            children += m_skeleton->GetBone(b)->parentIndex == m_chosen ? 1 : 0;
        set(u8"name", bone->name.AsView());
        set(u8"index", Format(u8"{}", m_chosen).AsView());
        set(u8"parent", parent != nullptr ? parent->name.AsView() : StringView(u8"None: a root"));
        set(u8"children", Format(u8"{}", children).AsView());
        set(u8"position", Describe(bone->localBindPose.position).AsView());
        set(u8"rotation", DescribeRotation(bone->localBindPose.rotation).AsView());
        set(u8"scale", Describe(bone->localBindPose.scale).AsView());
    }

    String SkeletonEditorPage::Tree() const
    {
        String out;
        // The rows in the model's order, each indented by its depth among the rows shown.
        for (const i32 bone : m_rows)
        {
            i32 depth = 0;
            for (i32 walk = m_skeleton->GetBone(bone)->parentIndex; walk >= 0; walk = m_skeleton->GetBone(walk)->parentIndex)
                ++depth;
            if (!out.IsEmpty())
                out += u8"\n";
            for (i32 i = 0; i < depth; ++i)
                out += u8"  ";
            out += m_skeleton->GetBone(bone)->name.AsView();
        }
        return out;
    }

    // === The preview ===

    void SkeletonEditorPage::FramePreview()
    {
        if (m_pane)
            (void)FrameBones(*m_pane, m_world.AsSpan());
    }

    void SkeletonEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (!m_pane)
            return;
        PreviewViewport* preview = m_pane->Viewport();
        const i32 count = m_skeleton ? m_skeleton->BoneCount() : 0;
        if (count <= 0)
            m_pane->ShowEmpty(u8"No bones", u8"This skeleton has none to show.");
        else
            m_pane->Show();
        if (preview != nullptr && preview->IsValid() && count > 0)
        {
            auto& draw = preview->SceneDebugDraw();
            draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
            m_bind.Resize(static_cast<usize>(count));
            for (i32 b = 0; b < count; ++b)
                m_bind[static_cast<usize>(b)] = m_skeleton->GetBone(b)->localBindPose;
            DrawSkeletonBones(draw, *m_skeleton, m_bind.AsSpan(), m_world, Float4x4::Identity(), m_chosen);
            if (!m_framed)
                m_framed = FrameBones(*m_pane, m_world.AsSpan());
        }
        m_pane->Update(dt);
    }

    void SkeletonEditorPage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void SkeletonEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    // === The factory ===

    const TypeInfo* SkeletonPageFactory::PrimaryType() const
    {
        return &pipeline::SkeletonAsset::StaticType();
    }

    UniquePtr<EditorPage> SkeletonPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<SkeletonEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterSkeletonEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<SkeletonPageFactory>(services),
                                                               editor::EditorRootAllocator()));
    }
}
