// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:playback`: the animation bar's state, kept apart from the window.
// InstantiateModel puts a skeletal animation component on the root of the model it places, and
// ClipPlayback drives that component: which of the model's clips plays, play and pause, stop
// (rewind and pause), stepping a frame while paused, scrubbing to a time, looping and speed. The
// component's manager builds the player on its first tick and starts the clip it was handed, so
// selecting a clip takes effect on the next scene update; the other controls act on the player
// at once.

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
        /// The frame rate Step moves by: the rate the importer samples clips at.
        static constexpr f32 kStepFps = 30.0f;

        /// Drives the animator on `root` (the entity InstantiateModel made), playing the clips of
        /// `model`. When there is no animator (the model has no clips, or the scene has no animation
        /// managers) there is nothing to drive, and HasClips returns false.
        ClipPlayback(scene::Scene& scene, scene::EntityHandle root, foundation::model::ModelResource& model)
            : m_scene(&scene), m_root(root), m_model(&model)
        {
            if (const engine::animation::SkeletalAnimationComponent* animator = Animator())
                for (usize i = 0; i < m_model->animations.Size(); ++i)
                    if (m_model->animations[i].Get() == animator->clip.Get() && animator->clip.Get() != nullptr)
                        m_selected = static_cast<i32>(i);
            if (animation::AnimationClip* clip = Clip(m_selected))
                clip->isLooping = m_looping; // the viewer loops clips whatever the file says
        }

        [[nodiscard]] bool HasClips() const { return Animator() != nullptr && !m_model->animations.IsEmpty(); }
        [[nodiscard]] i32 ClipCount() const { return static_cast<i32>(m_model->animations.Size()); }
        [[nodiscard]] i32 Selected() const noexcept { return m_selected; }
        /// The clip's name as the cook made it, without the model prefix ("model.").
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

        /// Plays clip `index` from its start, beginning on the next scene update.
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

        /// Plays from the current time, or pauses. A clip that does not loop and has reached its end
        /// starts again from the beginning.
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

        /// Rewinds to the clip's start and pauses.
        void Stop()
        {
            animation::AnimationPlayer* player = Player();
            if (player == nullptr)
                return;
            player->Pause();
            player->SetCurrentTime(0.0f);
        }

        /// Moves `frames` frames forward (backward when negative) and pauses there.
        void Step(i32 frames)
        {
            if (Player() == nullptr)
                return;
            SetPlaying(false);
            Scrub(Time() + static_cast<f32>(frames) / kStepFps);
        }

        /// Jumps to `seconds` into the clip, clamped to its length. If the clip was playing it
        /// continues from there.
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
        /// Whether the clip starts over at its end, rather than stopping there. This changes the
        /// viewer's copy of the clip; the file is untouched.
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

        /// The player, or null until the animator's manager builds it on its first tick.
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
