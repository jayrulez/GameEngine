// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:preview` partition.
//
// What the animation pages' previews share (Documentation/Specs/editor-gui.md, 5.4, layer 10): a
// skeleton drawn as its bones (a line from each to its parent, a cross at each joint, one bone drawn
// apart from the rest when chosen), the path a clip's root motion travels drawn on the ground, and
// the rig a pose is shown on (AnimationPreviewRig): the skeleton and the skinned mesh the preview
// plays on, picked on the preview's bar, each optional (no mesh: the bones alone), the mesh skinned
// with the pose each frame.

module;
#include "Core/Prelude.h"

export module editor.animation:preview;

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.geometry;
import foundation.resource;
import foundation.render;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace animation = foundation::animation;

    /// `skeleton` in the pose `localPoses` as its bones, placed by `base`; `chosen` (-1: none)
    /// drawn apart. `worldScratch` is the caller's, kept between frames.
    void DrawSkeletonBones(foundation::render::debug::DebugDraw& draw, animation::Skeleton& skeleton,
                           Span<const animation::BoneTransform> localPoses, Array<Float4x4>& worldScratch,
                           const Float4x4& base = Float4x4::Identity(), i32 chosen = -1);

    /// The path `clip`'s root motion travels from its start (on the ground, and up where it takes
    /// its height), and where the clip is along it at `time`. Nothing for a clip without one.
    void DrawRootMotionPath(foundation::render::debug::DebugDraw& draw, const animation::AnimationClip& clip, f32 time);

    /// Where the rig stands at `time` when it travels by `clip`'s root motion from its start.
    [[nodiscard]] Float4x4 RootMotionPlacement(const animation::AnimationClip& clip, f32 time);

    class AnimationPreviewRig
    {
    public:
        /// Over `pane`'s preview (its scene given a sun and the mesh's entity), its two slots
        /// placed among the preview's own choices. Without a preview (a test), it still keeps
        /// what was picked.
        AnimationPreviewRig(EditorContext& context, const app::AssetPageServices& services, app::PreviewPane& pane);
        ~AnimationPreviewRig();
        AnimationPreviewRig(const AnimationPreviewRig&) = delete;
        AnimationPreviewRig& operator=(const AnimationPreviewRig&) = delete;

        /// The skeleton it plays on (nil: none) and the skinned mesh drawn over it (nil: the bones
        /// alone), each bound by its asset's id.
        void SetSkeleton(const Guid& id);
        void SetMesh(const Guid& id);
        [[nodiscard]] const Guid& SkeletonId() const noexcept { return m_skeletonId; }
        [[nodiscard]] const Guid& MeshId() const noexcept { return m_meshId; }
        /// The cooked skeleton (null: none picked, or not cooked yet).
        [[nodiscard]] animation::Skeleton* Skeleton() const;
        /// Told when the user picks either: the page keeps them with the project.
        Function<void()> OnPicked;

        /// Once a frame: `pose` (the skeleton's local poses; empty or short: its bind pose) shown,
        /// placed by `base`, its bones drawn (`chosen` apart) and the mesh skinned with it.
        void Show(Span<const animation::BoneTransform> pose, const Float4x4& base = Float4x4::Identity(), i32 chosen = -1);
        /// The preview's camera put where the bones last shown fill the view (a skeleton may be a
        /// few centimetres tall or a hundred metres); false when none have shown yet.
        bool Frame();
        /// Told once, the first time a skeleton shows (the page frames it).
        Function<void()> OnFirstShown;

        // === For tests ===
        [[nodiscard]] app::AssetPickerSlot* SkeletonSlot() const noexcept { return m_skeletonSlot.Get(); }
        [[nodiscard]] app::AssetPickerSlot* MeshSlot() const noexcept { return m_meshSlot.Get(); }

    private:
        [[nodiscard]] RefPtr<app::AssetPickerSlot> MakeSlot(StringView id, StringView tooltip, Array<String> types, bool skeleton);
        void ShowSlot(app::AssetPickerSlot* slot, const Guid& id);
        void PointMeshAt(foundation::geometry::StaticMesh* mesh);

        EditorContext* m_context;
        const app::AssetPageServices* m_services; // the page's, which outlives the rig
        app::PreviewPane* m_pane;
        RefPtr<app::AssetPickerSlot> m_skeletonSlot;
        RefPtr<app::AssetPickerSlot> m_meshSlot;
        Guid m_skeletonId;
        Guid m_meshId;
        foundation::resource::Proxy<animation::Skeleton> m_skeleton;
        foundation::resource::Proxy<foundation::geometry::StaticMesh> m_mesh;
        foundation::geometry::StaticMesh* m_shownMesh = nullptr; // the product the entity draws, as last pointed
        foundation::scene::EntityHandle m_meshEntity;
        Array<animation::BoneTransform> m_bind;
        Array<Float4x4> m_world;
        Array<Float4x4> m_skinning;
        animation::Skeleton* m_framedFor = nullptr; // the skeleton the first frame was told for
    };
}
