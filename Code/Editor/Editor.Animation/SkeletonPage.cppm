// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:skeleton_page` partition.
//
// SkeletonEditorPage: a skeleton on the GUI from its markup (Data/Editor/SkeletonPage.gml;
// Documentation/Specs/editor-gui.md, 5.4, layer 10, part 3). A viewer: a skeleton comes from its
// model's import and nothing here edits it. Its bones are a tree (each under its parent), with a
// search that keeps a bone whose name holds it and the bones above it, so where it hangs still
// shows; a bone the user folds stays folded. Beside it the bind pose, drawn from the asset itself
// (it shows before a cook), the chosen bone drawn apart. The grid has the chosen bone's facts (its
// name, index, parent, children and bind position, rotation and scale) and the skeleton's (its
// bones, roots and deepest chain), all read only.

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
    /// What a skeleton is, as the page's read-only rows say it: its bones, roots and deepest chain.
    struct SkeletonFacts
    {
        i32 bones = 0;
        i32 roots = 0;
        i32 depth = 0; // bones below its deepest root, along the longest chain
    };
    [[nodiscard]] SkeletonFacts FactsOf(const foundation::animation::Skeleton& skeleton);
    /// A rotation as its row says it: degrees about an axis ("90° about 0, 1, 0"; "None").
    [[nodiscard]] String DescribeRotation(const Quaternion& rotation);

    class SkeletonEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/SkeletonPage.gml";

        SkeletonEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~SkeletonEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// The bone chosen (-1: none): drawn apart, its facts in the grid, its row selected.
        void ChooseBone(i32 bone);
        [[nodiscard]] i32 ChosenBone() const noexcept { return m_chosen; }
        /// The tree keeps the bones whose names hold `text` and those above them (empty: all).
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
        bool m_showing = false; // choosing from code: not the user's
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

    /// The skeleton page (asking for its services when it is made). Skeletons come from import:
    /// there is no creator.
    void RegisterSkeletonEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
