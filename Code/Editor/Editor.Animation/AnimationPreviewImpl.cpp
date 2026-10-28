// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:preview` partition (implementation).
//
// The heavy engine imports (the mesh component, the lights) live here, out of the interface.

module;
#include "Core/Prelude.h"

module editor.animation;

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.geometry;
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.gui;
import foundation.gui.toolkit; // ToolButton (Bones, Mesh)
import geometry.pipeline;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;

    void DrawSkeletonBones(foundation::render::debug::DebugDraw& draw, animation::Skeleton& skeleton,
                           Span<const animation::BoneTransform> localPoses, Array<Float4x4>& worldScratch, const Float4x4& base, i32 chosen)
    {
        const usize boneCount = static_cast<usize>(skeleton.BoneCount());
        if (boneCount == 0 || localPoses.Size() < boneCount)
            return;
        worldScratch.Resize(boneCount);
        skeleton.ComputeWorldPoses(localPoses, Span<Float4x4>{worldScratch.Data(), boneCount});
        for (usize b = 0; b < boneCount; ++b)
            worldScratch[b] = worldScratch[b] * base;
        const Color boneColor{0.35f, 0.85f, 1.0f, 1.0f};
        const Color chosenColor{1.0f, 0.78f, 0.25f, 1.0f};
        const auto at = [](const Float4x4& m) { return Float3{m.m[3][0], m.m[3][1], m.m[3][2]}; };
        for (usize b = 0; b < boneCount; ++b)
        {
            const animation::Bone* bone = skeleton.GetBone(static_cast<i32>(b));
            const bool isChosen = static_cast<i32>(b) == chosen;
            const Color color = isChosen ? chosenColor : boneColor;
            const Float3 joint = at(worldScratch[b]);
            if (bone != nullptr && bone->parentIndex >= 0 && static_cast<usize>(bone->parentIndex) < boneCount)
                draw.DrawLine(at(worldScratch[static_cast<usize>(bone->parentIndex)]), joint, color, isChosen);
            draw.DrawCross(joint, isChosen ? 0.05f : 0.02f, color);
        }
    }

    void DrawRootMotionPath(foundation::render::debug::DebugDraw& draw, const animation::AnimationClip& clip, f32 time)
    {
        const animation::RootMotionCurve& curve = clip.rootMotion;
        if (curve.IsEmpty())
            return;
        const Color path{1.0f, 0.75f, 0.2f, 1.0f};
        // From where it starts: along the ground where it extracts travel, up where it extracts height.
        const Float3 start = curve.positions[0];
        const auto onGround = [&](Float3 p)
        {
            return Float3{curve.horizontal ? p.x - start.x : 0.0f, curve.vertical ? p.y - start.y : 0.0f,
                          curve.horizontal ? p.z - start.z : 0.0f};
        };
        for (usize i = 0; i + 1 < curve.positions.Size(); ++i)
            draw.DrawLine(onGround(curve.positions[i]), onGround(curve.positions[i + 1]), path, true);
        const animation::RootMotionDelta now = animation::ClipRootMotion(clip, 0.0f, time, false);
        draw.DrawWireSphere(now.translation, 0.04f, path, 10, true);
    }

    bool FrameBones(app::PreviewPane& pane, Span<const Float4x4> world)
    {
        PreviewViewport* preview = pane.Viewport();
        if (preview == nullptr || world.IsEmpty())
            return false;
        Float3 low{world[0].m[3][0], world[0].m[3][1], world[0].m[3][2]};
        Float3 high = low;
        for (const Float4x4& bone : world)
        {
            const Float3 at{bone.m[3][0], bone.m[3][1], bone.m[3][2]};
            low = Float3{Min(low.x, at.x), Min(low.y, at.y), Min(low.z, at.z)};
            high = Float3{Max(high.x, at.x), Max(high.y, at.y), Max(high.z, at.z)};
        }
        // From the front and a little above, far enough that the tallest side fills about two thirds.
        const Float3 middle = (low + high) * 0.5f;
        const f32 size = Max(Max(high.x - low.x, high.y - low.y), Max(high.z - low.z, 0.01f));
        const f32 distance = size * 1.6f;
        preview->Camera().position = middle + Normalized(Float3{0.35f, 0.3f, 1.0f}) * distance;
        preview->Camera().LookAt(middle);
        preview->Camera().nearZ = Min(preview->Camera().nearZ, distance * 0.05f);
        preview->Camera().farZ = Max(preview->Camera().farZ, distance * 4.0f);
        return true;
    }

    Float4x4 RootMotionPlacement(const animation::AnimationClip& clip, f32 time)
    {
        if (clip.rootMotion.IsEmpty())
            return Float4x4::Identity();
        const animation::RootMotionDelta moved = animation::ClipRootMotion(clip, 0.0f, time, false);
        return RotationMatrix(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, moved.yaw)) * Float4x4::Translation(moved.translation);
    }

    // === The rig ===

    AnimationPreviewRig::AnimationPreviewRig(EditorContext& context, const app::AssetPageServices& services, app::PreviewPane& pane)
        : m_context(&context), m_services(&services), m_pane(&pane)
    {
        // The slots: what makes a skeleton, asked of the builders; a skinned mesh's asset, asked of its type.
        const app::ResourceRefServices* refs = services.refs;
        Array<String> skeletons = refs != nullptr && refs->AcceptedTypes ? refs->AcceptedTypes(animation::Skeleton::StaticType()) : Array<String>{};
        Array<String> meshes;
        meshes.PushBack(String(StringView(reinterpret_cast<const utf8char*>(pipeline::SkinnedMeshAsset::StaticType().name))));
        m_skeletonSlot = MakeSlot(u8"preview-skeleton", u8"The skeleton the preview plays on; kept for this asset", Move(skeletons), true);
        m_meshSlot = MakeSlot(u8"preview-mesh", u8"The skinned mesh drawn over the skeleton (none: the bones alone); kept for this asset", Move(meshes), false);
        const auto labelled = [&](StringView text, app::AssetPickerSlot& slot)
        {
            RefPtr<gui::Label> label = MakeRef<gui::Label>(EditorRootAllocator());
            label->AddClass(u8"preview-hint");
            label->SetText(text);
            pane.AddControl(*label);
            pane.AddControl(slot);
        };
        labelled(u8"Skeleton", *m_skeletonSlot);
        labelled(u8"Mesh", *m_meshSlot);
        // What shows of it: the bones, the mesh (hidden, its pick kept), each this page's alone.
        AnimationPreviewRig* self = this;
        const auto toggle = [&](StringView id, StringView text, StringView tooltip, Function<void(bool)> set) -> RefPtr<gui::ToolButton>
        {
            RefPtr<gui::ToolButton> button = MakeRef<gui::ToolButton>(EditorRootAllocator());
            button->SetId(id);
            button->SetToggle(true);
            button->SetChecked(true);
            button->SetText(text);
            button->SetShowsText(true);
            button->SetTooltip(tooltip);
            button->SetOnToggled(Move(set));
            pane.AddControl(*button);
            return button;
        };
        m_bonesButton = toggle(u8"preview-bones", u8"Bones", u8"Draw the skeleton's bones over the preview", [self](bool on) { self->SetBonesShown(on); });
        m_meshButton = toggle(u8"preview-mesh-shown", u8"Mesh", u8"Draw the skinned mesh (off: hidden, its pick kept)", [self](bool on) { self->SetMeshShown(on); });

        scene::Scene* scenePtr = pane.Viewport() != nullptr ? pane.Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr)
            return;
        // A sun so a skinned mesh is lit (the bones need none), and the mesh's entity, drawing
        // nothing until one is picked.
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) * Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
            lights->Add(sun).castsShadows = false;
        m_meshEntity = scenePtr->CreateEntity(u8"PreviewMesh");
        if (auto* components = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            components->Add(m_meshEntity);
        if (PreviewViewport* preview = pane.Viewport())
        {
            preview->Camera().position = Float3{0.0f, 1.4f, 3.2f};
            preview->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});
        }
    }

    AnimationPreviewRig::~AnimationPreviewRig() = default;

    RefPtr<app::AssetPickerSlot> AnimationPreviewRig::MakeSlot(StringView id, StringView tooltip, Array<String> types, bool skeleton)
    {
        RefPtr<app::AssetPickerSlot> slot = MakeRef<app::AssetPickerSlot>(EditorRootAllocator());
        slot->SetId(id);
        slot->AddClass(u8"preview-slot");
        slot->SetTooltip(tooltip);
        slot->SetAcceptedTypes(Move(types));
        AnimationPreviewRig* self = this;
        const auto assign = [self, skeleton](const Guid& id)
        {
            if (skeleton)
                self->SetSkeleton(id);
            else
                self->SetMesh(id);
            if (self->OnPicked)
                self->OnPicked();
        };
        app::AssetPickerSlot* raw = slot.Get();
        if (const app::ResourceRefServices* refs = m_services->refs; refs != nullptr && refs->Pick)
            slot->SetOnPick([self, refs, raw, skeleton, assign]()
                            { refs->Pick(raw->AcceptedTypes(), skeleton ? self->m_skeletonId : self->m_meshId, assign); });
        slot->SetOnClear([assign]() { assign(Guid{}); });
        slot->SetOnDropped(assign);
        return slot;
    }

    void AnimationPreviewRig::ShowSlot(app::AssetPickerSlot* slot, const Guid& id)
    {
        if (slot == nullptr)
            return;
        app::ResourceRefShown shown;
        const app::ResourceRefServices* refs = m_services->refs;
        if (!id.IsNil() && refs != nullptr && refs->Describe && refs->Describe(id, shown))
        {
            slot->SetValue(shown.Name.AsView());
            slot->SetGlyph(shown.Glyph.AsView(), shown.GlyphClass.AsView());
            return;
        }
        slot->SetValue(StringView{});
        slot->SetGlyph(StringView{});
    }

    void AnimationPreviewRig::SetSkeleton(const Guid& id)
    {
        m_skeletonId = id;
        m_skeleton = m_context->Resources() != nullptr && !id.IsNil() ? m_context->Resources()->Bind<animation::Skeleton>(id)
                                                                         : foundation::resource::Proxy<animation::Skeleton>{};
        ShowSlot(m_skeletonSlot.Get(), id);
    }

    void AnimationPreviewRig::SetMesh(const Guid& id)
    {
        m_meshId = id;
        m_mesh = m_context->Resources() != nullptr && !id.IsNil() ? m_context->Resources()->Bind<foundation::geometry::StaticMesh>(id)
                                                                     : foundation::resource::Proxy<foundation::geometry::StaticMesh>{};
        PointMeshAt(nullptr); // pointed at the product once it resolves (Show)
        ShowSlot(m_meshSlot.Get(), id);
    }

    animation::Skeleton* AnimationPreviewRig::Skeleton() const
    {
        return m_skeleton ? m_skeleton.Get() : nullptr;
    }

    void AnimationPreviewRig::PointMeshAt(foundation::geometry::StaticMesh* mesh)
    {
        m_shownMesh = mesh;
        scene::Scene* scenePtr = m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* components = scenePtr != nullptr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* component = components != nullptr ? components->Get(m_meshEntity) : nullptr;
        if (component == nullptr)
            return;
        if (mesh != nullptr)
            component->mesh = mesh;
        else
            component->mesh.SetDirect(RefPtr<foundation::geometry::StaticMesh>{});
        component->boneMatrices = nullptr;
        component->boneCount = 0;
    }

    bool AnimationPreviewRig::Frame()
    {
        return FrameBones(*m_pane, m_world.AsSpan());
    }

    void AnimationPreviewRig::Show(Span<const animation::BoneTransform> pose, const Float4x4& base, i32 chosen)
    {
        PreviewViewport* preview = m_pane->Viewport();
        if (preview == nullptr || !preview->IsValid())
            return;
        auto& draw = preview->SceneDebugDraw();
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});

        // The mesh follows its product (a cook or a reload makes a new one).
        foundation::geometry::StaticMesh* mesh = m_mesh ? m_mesh.Get() : nullptr;
        if (mesh != m_shownMesh)
            PointMeshAt(mesh);

        animation::Skeleton* skeleton = Skeleton();
        if (skeleton == nullptr || skeleton->BoneCount() <= 0)
            return;
        const usize boneCount = static_cast<usize>(skeleton->BoneCount());
        if (pose.Size() < boneCount)
        {
            m_bind.Resize(boneCount);
            for (usize b = 0; b < boneCount; ++b)
                m_bind[b] = skeleton->Bones()[b].localBindPose;
            pose = m_bind.AsSpan();
        }
        // Hidden bones are still placed (the framing and the mesh read them), drawn where nothing shows.
        if (m_bonesShown)
            DrawSkeletonBones(draw, *skeleton, pose, m_world, base, chosen);
        else
        {
            m_unshown.Clear();
            DrawSkeletonBones(m_unshown, *skeleton, pose, m_world, base, chosen);
        }
        if (skeleton != m_framedFor && !m_world.IsEmpty())
        {
            m_framedFor = skeleton;
            if (OnFirstShown)
                OnFirstShown();
        }

        scene::Scene* scenePtr = preview->Scene();
        auto* components = scenePtr != nullptr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* component = components != nullptr ? components->Get(m_meshEntity) : nullptr;
        if (component == nullptr || mesh == nullptr)
            return;
        component->visible = m_meshShown;
        // Skinned with the pose, placed where the bones are (borrowed for this frame's drawing).
        m_skinning.Resize(boneCount);
        skeleton->ComputeSkinningMatrices(pose, Span<Float4x4>{m_skinning.Data(), boneCount});
        component->boneMatrices = m_skinning.Data();
        component->boneCount = static_cast<u32>(boneCount);
        Transform placed;
        placed.position = Float3{base.m[3][0], base.m[3][1], base.m[3][2]};
        placed.rotation = QuaternionFromRotationMatrix(base);
        scenePtr->SetLocalTransform(m_meshEntity, placed);
    }

    void AnimationPreviewRig::SetBonesShown(bool shown)
    {
        m_bonesShown = shown;
        if (m_bonesButton && m_bonesButton->IsChecked() != shown)
            m_bonesButton->SetChecked(shown);
    }

    void AnimationPreviewRig::SetMeshShown(bool shown)
    {
        m_meshShown = shown;
        if (m_meshButton && m_meshButton->IsChecked() != shown)
            m_meshButton->SetChecked(shown);
        scene::Scene* scenePtr = m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* components = scenePtr != nullptr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        if (engine::render::MeshComponent* component = components != nullptr ? components->Get(m_meshEntity) : nullptr)
            component->visible = shown;
    }
}
