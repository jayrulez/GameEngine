// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:skeleton_page` partition.
//
// SkeletonEditorPage: a skeleton on the GUI from its markup (Data/Editor/SkeletonPage.xml). It is
// a viewer: a skeleton comes from its model's import and nothing here edits it. The bones are a
// tree, each under its parent. A search keeps the bones whose names contain the text and every
// bone above them, so their place in the hierarchy still shows; a bone the user folds stays
// folded. Beside the tree the bind pose is drawn from the asset itself, so it shows before a cook,
// with the chosen bone drawn apart. The grid shows the chosen bone's facts (its name, index,
// parent, children and bind position, rotation and scale) and the skeleton's (its bones, roots
// and deepest chain), all read-only.

module;
#include "Core/Prelude.h"

export module editor.animation:skeleton_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :preview;

using namespace foundation::core;

export namespace editor
{
    /// The skeleton facts the page's read-only rows show: its bones, roots and deepest chain.
    struct SkeletonFacts
    {
        i32 bones = 0;
        i32 roots = 0;
        i32 depth = 0; // bones below its deepest root, along the longest chain
    };
    [[nodiscard]] SkeletonFacts FactsOf(const foundation::animation::Skeleton& skeleton);
    /// Describes a rotation for its row as degrees about an axis ("90° about 0, 1, 0"; "None").
    [[nodiscard]] String DescribeRotation(const Quaternion& rotation);

    class SkeletonEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/SkeletonPage.xml";

        SkeletonEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~SkeletonEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// Chooses `bone` (-1: none): it is drawn apart, its facts fill the grid, its row is selected.
        void ChooseBone(i32 bone);
        [[nodiscard]] i32 ChosenBone() const noexcept { return m_chosen; }
        /// Filters the tree to the bones whose names contain `text` and those above them (empty: all).
        void Search(StringView text);

        // === For tests ===
        /// The tree's rows, indented ("Hips\n  Spine\n    Chest").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::PropertyBag& BoneRows() noexcept { return *m_boneRows; }
        [[nodiscard]] foundation::animation::Skeleton* Skeleton() const noexcept { return m_skeleton.Get(); }

    protected:
        void OnObjectChanged() override;

    private:
        void BuildSkeleton();
        void ShowTree();
        void ShowBone();
        void ShowFacts();
        void FramePreview();

        RefPtr<foundation::animation::Skeleton> m_skeleton; // filled from the asset's source
        UniquePtr<foundation::gui::TreeModel> m_model;
        foundation::gui::TreeView* m_tree = nullptr;
        Array<i32> m_rows; // the bone each tree node stands for, by node id
        String m_search;
        i32 m_chosen = -1;
        bool m_showing = false; // the code is making the choice, not the user
        UniquePtr<foundation::gui::PropertyBag> m_boneRows;
        UniquePtr<app::PreviewPane> m_pane;
        Array<foundation::animation::BoneTransform> m_bind;
        Array<Float4x4> m_world;
        bool m_framed = false;
    };

    class SkeletonPageFactory final : public IEditorPageFactory
    {
    public:
        explicit SkeletonPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the skeleton page, which asks for its services when it is made. Skeletons come
    /// from import, so there is no creator.
    void RegisterSkeletonEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
