// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:preview` partition.
//
// What the animation pages' previews share: a skeleton drawn as its bones (a line from each to its
// parent, a cross at each joint, the chosen bone drawn apart from the rest), the path a clip's root
// motion travels drawn on the ground, and the rig a pose is shown on (AnimationPreviewRig). The rig
// is the skeleton and the skinned mesh the preview plays on, each picked on the preview's bar and
// each optional (with no mesh, the bones alone show); the mesh is skinned with the pose each frame.
// Bones and Mesh on the bar hide either one, and a hidden mesh keeps its pick.

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
import foundation.gui.toolkit; // ToolButton, for the Bones and Mesh buttons
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace animation = foundation::animation;

    /// Draws `skeleton` in the pose `localPoses` as its bones, placed by `base`, with bone `chosen`
    /// (-1: none) drawn apart. `worldScratch` belongs to the caller and is kept between frames.
    void DrawSkeletonBones(foundation::render::debug::DebugDraw& draw, animation::Skeleton& skeleton,
                           Span<const animation::BoneTransform> localPoses, Array<Float4x4>& worldScratch,
                           const Float4x4& base = Float4x4::Identity(), i32 chosen = -1);

    /// Draws the path `clip`'s root motion travels from its start (on the ground, and upwards where
    /// it extracts height), and where the clip is along it at `time`. Draws nothing for a clip
    /// without root motion.
    void DrawRootMotionPath(foundation::render::debug::DebugDraw& draw, const animation::AnimationClip& clip, f32 time);

    /// Moves `pane`'s camera so the bones at `world` (their model-space matrices, as
    /// DrawSkeletonBones leaves them) fill the view, since a skeleton may be a few centimetres tall
    /// or a hundred metres. Returns false without a preview or bones.
    bool FrameBones(app::PreviewPane& pane, Span<const Float4x4> world);

    /// Where the rig stands at `time` when it travels by `clip`'s root motion from its start.
    [[nodiscard]] Float4x4 RootMotionPlacement(const animation::AnimationClip& clip, f32 time);

    class AnimationPreviewRig
    {
    public:
        /// A rig over `pane`'s preview. It adds a sun and the mesh's entity to the preview's scene
        /// and places its two picker slots among the preview's other controls. Without a preview
        /// (in a test) it still keeps what was picked.
        AnimationPreviewRig(EditorContext& context, const app::AssetPageServices& services, app::PreviewPane& pane);
        ~AnimationPreviewRig();
        AnimationPreviewRig(const AnimationPreviewRig&) = delete;
        AnimationPreviewRig& operator=(const AnimationPreviewRig&) = delete;

        /// Sets the skeleton it plays on (nil: none) and the skinned mesh drawn over it (nil: only
        /// the bones show), each by its asset's id.
        void SetSkeleton(const Guid& id);
        void SetMesh(const Guid& id);
        [[nodiscard]] const Guid& SkeletonId() const noexcept { return m_skeletonId; }
        [[nodiscard]] const Guid& MeshId() const noexcept { return m_meshId; }
        /// The cooked skeleton (null: none picked, or not cooked yet).
        [[nodiscard]] animation::Skeleton* Skeleton() const;
        /// Called when the user picks either one, so the page can keep the picks with the project.
        Function<void()> OnPicked;

        /// Called once a frame: shows `pose` (the skeleton's local poses; the bind pose is used when
        /// it is empty or shorter than the skeleton) placed by `base`, draws its bones (`chosen`
        /// apart) and skins the mesh with it.
        void Show(Span<const animation::BoneTransform> pose, const Float4x4& base = Float4x4::Identity(), i32 chosen = -1);
        /// Moves the preview's camera so the bones last shown fill the view (a skeleton may be a
        /// few centimetres tall or a hundred metres). Returns false when none have been shown yet.
        bool Frame();
        /// Called once, the first time a skeleton shows, so the page can frame it.
        Function<void()> OnFirstShown;
        /// Whether its bones and its mesh are drawn (a hidden mesh keeps its pick), as set by the
        /// bar's Bones and Mesh buttons. These belong to the page and do not change the asset.
        void SetBonesShown(bool shown);
        void SetMeshShown(bool shown);
        [[nodiscard]] bool BonesShown() const noexcept { return m_bonesShown; }
        [[nodiscard]] bool MeshShown() const noexcept { return m_meshShown; }

        // === For tests ===
        [[nodiscard]] app::AssetPickerSlot* SkeletonSlot() const noexcept { return m_skeletonSlot.Get(); }
        [[nodiscard]] app::AssetPickerSlot* MeshSlot() const noexcept { return m_meshSlot.Get(); }
        [[nodiscard]] foundation::gui::ToolButton* BonesButton() const noexcept { return m_bonesButton.Get(); }
        [[nodiscard]] foundation::gui::ToolButton* MeshButton() const noexcept { return m_meshButton.Get(); }

    private:
        [[nodiscard]] RefPtr<app::AssetPickerSlot> MakeSlot(StringView id, StringView tooltip, Array<String> types, bool skeleton);
        void ShowSlot(app::AssetPickerSlot* slot, const Guid& id);
        void PointMeshAt(foundation::geometry::StaticMesh* mesh);

        EditorContext* m_context;
        const app::AssetPageServices* m_services; // owned by the page, which outlives the rig
        app::PreviewPane* m_pane;
        RefPtr<app::AssetPickerSlot> m_skeletonSlot;
        RefPtr<app::AssetPickerSlot> m_meshSlot;
        RefPtr<foundation::gui::ToolButton> m_bonesButton;
        RefPtr<foundation::gui::ToolButton> m_meshButton;
        bool m_bonesShown = true;
        bool m_meshShown = true;
        foundation::render::debug::DebugDraw m_unshown; // hidden bones are placed into this; it is never drawn
        Guid m_skeletonId;
        Guid m_meshId;
        foundation::resource::Proxy<animation::Skeleton> m_skeleton;
        foundation::resource::Proxy<foundation::geometry::StaticMesh> m_mesh;
        foundation::geometry::StaticMesh* m_shownMesh = nullptr; // the cooked mesh the entity was last pointed at
        foundation::scene::EntityHandle m_meshEntity;
        Array<animation::BoneTransform> m_bind;
        Array<Float4x4> m_world;
        Array<Float4x4> m_skinning;
        animation::Skeleton* m_framedFor = nullptr; // the skeleton OnFirstShown was last called for
    };
}
