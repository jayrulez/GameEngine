// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Run audio (the run-audio spec): the subsystem groups what one running game plays by its run.
// A run's scenes nest under the run's group and its script music plays into it; ending the run
// fades all of it out and leaves other runs and the editor alone; only the focused run is heard
// unless every run is; the host's pause freezes the run.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <cmath>

import foundation.core;
import foundation.runtime;
import foundation.scene;
import foundation.audio;
import engine.audio;

using namespace foundation::core;
using namespace engine::audio;
using namespace foundation::audio;
namespace scene = foundation::scene;

namespace
{
    [[nodiscard]] RefPtr<AudioClip> Tone(f32 seconds, u32 sampleRate)
    {
        Array<i16> samples;
        const usize frames = static_cast<usize>(seconds * static_cast<f32>(sampleRate));
        for (usize i = 0; i < frames; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(sampleRate);
            samples.PushBack(static_cast<i16>(std::sin(6.2831853f * 440.0f * t) * 16000.0f));
        }
        Array<byte> wav;
        REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, sampleRate, wav));
        RefPtr<AudioClip> clip = MakeRef<AudioClip>(DefaultAllocator());
        AudioClipMetadata metadata;
        REQUIRE(ProbeAudioClipMetadata(Span<const byte>(wav.Data(), wav.Size()), metadata));
        clip->channels = metadata.channels;
        clip->sampleRate = metadata.sampleRate;
        clip->frameCount = metadata.frameCount;
        clip->durationSeconds = metadata.durationSeconds;
        clip->encodedData = wav;
        return clip;
    }

    [[nodiscard]] AudioEngineSettings Headless()
    {
        AudioEngineSettings settings;
        settings.headless = true;
        settings.dedupeWindowSeconds = 0.0f;
        return settings;
    }

    // A scene of `run` with one looping autoplay source, wired to the subsystem as the scene
    // subsystem would (SystemsReady).
    struct RunScene
    {
        scene::Scene scene{DefaultAllocator(), u8"run-scene"};
        scene::EntityHandle source;

        RunScene(AudioSubsystem& audio, const void* run, const RefPtr<AudioClip>& clip)
        {
            RegisterAudioComponentReflection();
            scene.SetRun(run);
            scene.AddSystem<AudioSourceComponentManager>();
            scene.AddSystem<AudioListenerComponentManager>();
            scene.AddSystem<AudioReverbZoneComponentManager>();
            scene.AddSystem<AudioSceneSystem>();
            audio.OnSystemsReady(scene);
            source = scene.CreateEntity(u8"source");
            AudioSourceComponent& c = scene.GetSystem<AudioSourceComponentManager>()->Add(source);
            c.clip = clip;
            c.autoPlay = true;
            c.loop = true;
            scene.Start();
            scene.SetSimulationEnabled(true);
        }
        [[nodiscard]] VoiceHandle Voice()
        {
            return scene.GetSystem<AudioSourceComponentManager>()->Get(source)->voice;
        }
    };
}

TEST_CASE("run audio: ending a run fades out its scenes and its music, not another run's")
{
    foundation::runtime::SubsystemManager ctx(DefaultAllocator());
    AudioSubsystem* audio = ctx.AddSubsystem<AudioSubsystem>(Headless());
    ctx.Startup();
    AudioEngine& engine = *audio->Engine();
    int runA = 0, runB = 0; // two games' keys (a GameInstance is the real one)

    CHECK(audio->RunGroupFor(nullptr) == 0u); // outside every run
    const u64 groupA = audio->RunGroupFor(&runA);
    REQUIRE(groupA != 0u);
    CHECK(audio->RunGroupFor(&runA) == groupA); // one group per run

    RunScene sceneA(*audio, &runA, Tone(2.0f, 8000));
    RunScene sceneB(*audio, &runB, Tone(2.0f, 4000));
    const VoiceHandle musicA = audio->PlayMusic(Tone(2.0f, 16000), 0.0f, 1.0f, groupA);
    const VoiceHandle musicB = audio->PlayMusic(Tone(2.0f, 12000), 0.0f, 1.0f, audio->RunGroupFor(&runB));
    const VoiceHandle editorShot = audio->PlayOneShot(Tone(2.0f, 6000)); // the editor's own
    REQUIRE(sceneA.Voice().IsValid());
    REQUIRE(sceneB.Voice().IsValid());
    REQUIRE(musicA.IsValid());
    REQUIRE(musicB.IsValid());
    const VoiceHandle sourceA = sceneA.Voice();

    audio->EndRun(&runA); // the Game tab's Stop
    for (int i = 0; i < 4; ++i)
    {
        audio->Update(0.1f);
    }
    CHECK_FALSE(engine.IsValidHandle(musicA));  // the music stopped with its run
    CHECK_FALSE(engine.IsValidHandle(sourceA)); // and the run's scene sources
    CHECK(audio->FindRunGroup(&runA) == 0u);    // the run is gone (its group freed)
    CHECK(engine.IsPlaying(musicB));            // another run plays on
    CHECK(engine.IsPlaying(sceneB.Voice()));
    CHECK(engine.IsPlaying(editorShot));        // and the editor

    sceneA.scene.Stop();
    sceneB.scene.Stop();
    ctx.Shutdown();
}

TEST_CASE("run audio: only the focused run is heard, unless every run is; pause freezes a run")
{
    foundation::runtime::SubsystemManager ctx(DefaultAllocator());
    AudioSubsystem* audio = ctx.AddSubsystem<AudioSubsystem>(Headless());
    ctx.Startup();
    AudioEngine& engine = *audio->Engine();
    int runA = 0, runB = 0, runC = 0;
    const u64 a = audio->RunGroupFor(&runA);
    const u64 b = audio->RunGroupFor(&runB);

    // No focus yet: every run is heard (the player's one run never needs one).
    CHECK_FALSE(engine.IsRunGroupMuted(a));
    CHECK_FALSE(engine.IsRunGroupMuted(b));

    audio->SetFocusedRun(&runA); // clicking into a Game tab
    CHECK_FALSE(engine.IsRunGroupMuted(a));
    CHECK(engine.IsRunGroupMuted(b)); // runs on, muted
    CHECK(audio->IsRunAudible(nullptr)); // the editor's own sounds are always heard
    const u64 c = audio->RunGroupFor(&runC);
    CHECK(engine.IsRunGroupMuted(c)); // a new run while another is focused starts muted

    audio->SetFocusedRun(&runB);
    CHECK(engine.IsRunGroupMuted(a));
    CHECK_FALSE(engine.IsRunGroupMuted(b));

    audio->SetHearAllRuns(true); // the editor setting: all instances
    CHECK_FALSE(engine.IsRunGroupMuted(a));
    CHECK_FALSE(engine.IsRunGroupMuted(b));
    CHECK_FALSE(engine.IsRunGroupMuted(c));
    audio->SetHearAllRuns(false);

    audio->EndRun(&runB); // the focused run ends: no focus, every remaining run heard
    CHECK(audio->FocusedRun() == nullptr);
    CHECK_FALSE(engine.IsRunGroupMuted(a));

    audio->SetRunPaused(&runA, true); // the toolbar's pause, a debugger break
    CHECK(engine.IsRunGroupPaused(a));
    audio->SetRunPaused(&runA, false);
    CHECK_FALSE(engine.IsRunGroupPaused(a));
    ctx.Shutdown();
}
