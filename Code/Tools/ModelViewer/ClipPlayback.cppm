// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:playback`: the animation bar's half that is not the window
// (Documentation/Specs/model-viewer.md, P2). A model put into a scene by InstantiateModel has a
// skeletal animation component on its root; this drives it: which clip plays (any of the model's),
// play and pause, stop (back to the start, paused), stepping a frame while paused, scrubbing to a
// time, looping and speed. The component's manager builds the player on its first tick and starts
// a clip it is handed, so a choice takes effect on the next scene update; the rest acts on the
// player at once.

module;
#include "Core/Prelude.h"

export module modelviewer:playback;

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.model.resource;
import engine.animation;

using namespace foundation::core;

export namespace modelviewer
{
    namespace scene = foundation::scene;
    namespace animation = foundation::animation;

    class ClipPlayback
    {
    public:
        /// Steps move a frame at this rate (the rate the importer samples clips at).
        static constexpr f32 kStepFps = 30.0f;

        /// Drives the animator on `root` (InstantiateModel's), playing `model`'s clips. Without an
        /// animator (a model without clips, or a scene without the animation managers) it has
        /// nothing to drive and says so (HasClips).
        ClipPlayback(scene::Scene& scene, scene::EntityHandle root, foundation::model::ModelResource& model)
            : m_scene(&scene), m_root(root), m_model(&model)
        {
            if (const engine::animation::SkeletalAnimationComponent* animator = Animator())
                for (usize i = 0; i < m_model->animations.Size(); ++i)
                    if (m_model->animations[i].Get() == animator->clip.Get() && animator->clip.Get() != nullptr)
                        m_selected = static_cast<i32>(i);
            if (animation::AnimationClip* clip = Clip(m_selected))
                clip->isLooping = m_looping; // a viewer loops what it shows, whatever the file says
        }

        [[nodiscard]] bool HasClips() const { return Animator() != nullptr && !m_model->animations.IsEmpty(); }
        [[nodiscard]] i32 ClipCount() const { return static_cast<i32>(m_model->animations.Size()); }
        [[nodiscard]] i32 Selected() const noexcept { return m_selected; }
        /// The clip's name as the cook made it, its model prefix ("model.") left off.
        [[nodiscard]] String ClipName(i32 index) const
        {
            const animation::AnimationClip* clip = Clip(index);
            if (clip == nullptr)
                return {};
            const StringView name = clip->Name().AsView();
            usize dot = 0;
            while (dot < name.Size() && name[dot] != u8'.')
                ++dot;
            return String(dot < name.Size() ? name.SubStr(dot + 1, name.Size() - dot - 1) : name);
        }

        /// Plays clip `index` from its start (on the next scene update).
        void Select(i32 index)
        {
            engine::animation::SkeletalAnimationComponent* animator = Animator();
            animation::AnimationClip* clip = Clip(index);
            if (animator == nullptr || clip == nullptr)
                return;
            m_selected = index;
            clip->isLooping = m_looping;
            animator->clip = clip;
            animator->autoPlay = true;
            animator->startTime = 0.0f;
        }

        [[nodiscard]] bool Playing() const
        {
            const animation::AnimationPlayer* player = Player();
            return player != nullptr && player->State() == animation::PlaybackState::Playing;
        }

        /// Play from where it stands (from the start again once a clip that does not loop ended), or pause.
        void SetPlaying(bool playing)
        {
            animation::AnimationPlayer* player = Player();
            if (player == nullptr)
                return;
            if (!playing)
            {
                player->Pause();
                return;
            }
            if (player->State() == animation::PlaybackState::Paused)
                player->Resume();
            else if (player->State() == animation::PlaybackState::Stopped && player->CurrentClip() != nullptr)
                player->Play(player->CurrentClip(), /*restart*/ Time() >= Duration());
        }

        /// Back to the clip's start, paused.
        void Stop()
        {
            animation::AnimationPlayer* player = Player();
            if (player == nullptr)
                return;
            player->Pause();
            player->SetCurrentTime(0.0f);
        }

        /// A frame forward (or back, negative), paused there.
        void Step(i32 frames)
        {
            if (Player() == nullptr)
                return;
            SetPlaying(false);
            Scrub(Time() + static_cast<f32>(frames) / kStepFps);
        }

        /// The pose at `seconds` into the clip (clamped to it); playing goes on from there.
        void Scrub(f32 seconds)
        {
            animation::AnimationPlayer* player = Player();
            if (player == nullptr)
                return;
            player->SetCurrentTime(Clamp(seconds, 0.0f, Duration()));
        }

        [[nodiscard]] f32 Time() const
        {
            const animation::AnimationPlayer* player = Player();
            return player != nullptr ? player->CurrentTime() : 0.0f;
        }
        [[nodiscard]] f32 Duration() const
        {
            const animation::AnimationPlayer* player = Player();
            const animation::AnimationClip* clip = player != nullptr ? player->CurrentClip() : Clip(m_selected);
            return clip != nullptr ? clip->duration : 0.0f;
        }

        [[nodiscard]] bool Looping() const noexcept { return m_looping; }
        /// Whether the clip starts over at its end (else it stops there). The viewer's own copy of
        /// the clip, so the file is untouched.
        void SetLooping(bool looping)
        {
            m_looping = looping;
            if (animation::AnimationClip* clip = Clip(m_selected))
                clip->isLooping = looping;
        }

        [[nodiscard]] f32 Speed() const
        {
            const engine::animation::SkeletalAnimationComponent* animator = Animator();
            return animator != nullptr ? animator->speed : 1.0f;
        }
        void SetSpeed(f32 speed)
        {
            if (engine::animation::SkeletalAnimationComponent* animator = Animator())
                animator->speed = speed;
        }

        /// The player, once the animator's manager built it (its first tick).
        [[nodiscard]] animation::AnimationPlayer* Player() const
        {
            const engine::animation::SkeletalAnimationComponent* animator = Animator();
            return animator != nullptr ? animator->player.Get() : nullptr;
        }

    private:
        [[nodiscard]] engine::animation::SkeletalAnimationComponent* Animator() const
        {
            auto* animators = m_scene->GetSystem<engine::animation::SkeletalAnimationComponentManager>();
            return animators != nullptr && m_scene->IsValid(m_root) ? animators->Get(m_root) : nullptr;
        }
        [[nodiscard]] animation::AnimationClip* Clip(i32 index) const
        {
            return index >= 0 && static_cast<usize>(index) < m_model->animations.Size() ? m_model->animations[static_cast<usize>(index)].Get()
                                                                                         : nullptr;
        }

        scene::Scene* m_scene;
        scene::EntityHandle m_root;
        foundation::model::ModelResource* m_model;
        i32 m_selected = -1;
        bool m_looping = true;
    };
}
