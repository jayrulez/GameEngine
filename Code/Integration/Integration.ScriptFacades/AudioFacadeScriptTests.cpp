// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Audio script FACADE end-to-end: a script drives Audio.playOneShot/playCue/playMusic by asset id
// (and controls a playing voice: the music's pitch and volume, a one-shot stopped early)
// and the four buses by AudioBus (Sedulous's facade), and the *Path forms by content path, through
// the resource seam. Cross-layer (scripting x audio), so it lives in Integration, not
// in Engine.Audio.Tests (which stays backend-neutral). Driven on both surviving backends (AngelScript
// + Luau) through a shared fixture; guarded so a build with neither compiles to an empty TU.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#if defined(OPTION_HAS_ANGELSCRIPT) || defined(OPTION_HAS_LUAU)
#include <cmath>
#include <string>

import foundation.core;
import foundation.audio;
import engine.audio;
import foundation.audio.resource;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.script;
import foundation.script.facades; // ScriptRuntimeBinding (the run a context belongs to)
import foundation.run;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace engine::audio;
using namespace foundation::audio;

namespace
{
    [[nodiscard]] RefPtr<AudioClip> MakeToneClip(f32 seconds, u32 sampleRate = 8000, u32 channels = 1)
    {
        Array<i16> samples;
        const usize frameCount = static_cast<usize>(seconds * static_cast<f32>(sampleRate));
        for (usize frame = 0; frame < frameCount; ++frame)
        {
            const f32 t = static_cast<f32>(frame) / static_cast<f32>(sampleRate);
            const i16 sample =
                static_cast<i16>(0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t) * 32000.0f);
            for (u32 channel = 0; channel < channels; ++channel)
            {
                samples.PushBack(sample);
            }
        }
        Array<byte> wav;
        REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), channels,
                                   sampleRate, wav));
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

    // The full facade drive, parameterized by the backend's manager + its two scripts (the main
    // program + the null-binding probe). Shared so AngelScript and Luau prove identical behavior.
    // Keeps a script's errors, so a refused load says why.
    struct ErrorLog final : foundation::script::IScriptErrorHandler
    {
        String text;
        void OnError(const foundation::script::ScriptError& error) override
        {
            text += Format(u8"{}:{}: {}\n", error.module, error.line, error.message);
        }
    };

    // `idScript` spells the id-addressed program for the clip's and the cue's guids.
    using IdScript = Function<String(StringView beep, StringView steps)>;

    void DriveAudioFacade(RefPtr<foundation::script::IScriptManager> scripts,
                          foundation::core::StringView script,
                          foundation::core::StringView bareScript, const IdScript& idScript,
                          foundation::core::StringView namedScript)
    {
        REQUIRE(scripts.Get() != nullptr);
        RegisterCoreTypes(); // Guid and Float3, which the id forms take
        RegisterAudioScriptFacade();
        RegisterAudioResource();

        // A hand-cooked content DB: sfx/beep (clip) + sfx/steps (cue referencing it).
        const StringView dir = u8"scratch_audio_scriptdb";
        FileDelete(u8"scratch_audio_scriptdb/sfx/beep.rasset");
        FileDelete(u8"scratch_audio_scriptdb/sfx/beep.data.bin");
        FileDelete(u8"scratch_audio_scriptdb/sfx/steps.rasset");
        RemoveDirectory(u8"scratch_audio_scriptdb/sfx");
        RemoveDirectory(dir);
        foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        foundation::content::Group* sfx = db.RootGroup()->CreateGroup(u8"sfx");
        REQUIRE(sfx != nullptr);

        RefPtr<AudioClip> tone = MakeToneClip(0.5f);
        auto* beep = sfx->CreateInstance(u8"beep", AudioClipSource::StaticType());
        REQUIRE(beep != nullptr);
        AudioClipSource clipRecord;
        clipRecord.channels = tone->channels;
        clipRecord.sampleRate = tone->sampleRate;
        clipRecord.frameCount = tone->frameCount;
        clipRecord.durationSeconds = tone->durationSeconds;
        REQUIRE(beep->WriteObject(clipRecord).IsOk());
        REQUIRE(beep->WriteData(u8"data",
                                Span<const byte>(tone->encodedData.Data(), tone->encodedData.Size()))
                    .IsOk());

        auto* steps = sfx->CreateInstance(u8"steps", SoundCueSource::StaticType());
        REQUIRE(steps != nullptr);
        SoundCueSource cueRecord;
        SoundCueSource::Variant variant;
        variant.clipId = beep->Id();
        variant.weight = 1.0f;
        cueRecord.variants.PushBack(variant);
        REQUIRE(steps->WriteObject(cueRecord).IsOk());

        AudioClipFactory clipFactory(foundation::core::DefaultAllocator());
        SoundCueFactory cueFactory(foundation::core::DefaultAllocator());
        foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), db);
        manager.AddFactory(&clipFactory);
        manager.AddFactory(&cueFactory);

        // A headless subsystem (Init is the public lifecycle seam) + the script binding.
        AudioEngineSettings engineSettings;
        engineSettings.headless = true;
        engineSettings.dedupeWindowSeconds = 0.0f; // each facade call = its own voice
        AudioSubsystem subsystem(engineSettings);
        subsystem.Init();
        REQUIRE(subsystem.Engine() != nullptr);

        foundation::script::RegisterReflectedTypes(*scripts);
        RefPtr<foundation::script::IScriptContext> ctx = scripts->CreateContext();
        REQUIRE(ctx.Get() != nullptr);
        subsystem.ExposeToScript(*ctx, &manager);

        REQUIRE(ctx->Load(script, u8"main").IsOk());
        CHECK(ctx->GetGlobal(u8"Played").Get<bool>());
        CHECK(ctx->GetGlobal(u8"Spatial").Get<bool>());
        CHECK(ctx->GetGlobal(u8"Cue").Get<bool>());
        CHECK(ctx->GetGlobal(u8"Music").Get<bool>());
        CHECK_FALSE(ctx->GetGlobal(u8"Missing").Get<bool>());
        CHECK_FALSE(ctx->GetGlobal(u8"MissingAgain").Get<bool>());
        CHECK(subsystem.Engine()->ActiveVoiceCount() == 4u);
        VoiceStatus music;
        REQUIRE(subsystem.Engine()->GetVoiceStatus(subsystem.Engine()->MusicVoice(), music));
        CHECK(music.bus == AudioBus::Music);

        // By asset id (Sedulous's shape): a voice for the clip and the cue, plain and 3D, music,
        // an invalid voice for an id of the wrong type, and the buses by AudioBus.
        {
            utf8char beepText[37];
            utf8char stepsText[37];
            beep->Id().ToChars(beepText);
            steps->Id().ToChars(stepsText);
            const String byId = idScript(StringView(beepText), StringView(stepsText));
            const usize before = subsystem.Engine()->ActiveVoiceCount();
            RefPtr<foundation::script::IScriptContext> ids = scripts->CreateContext();
            ErrorLog errors;
            ids->SetErrorHandler(&errors);
            subsystem.ExposeToScript(*ids, &manager);
            const Status loaded = ids->Load(byId.AsView(), u8"main");
            if (!loaded.IsOk())
            {
                MESSAGE(std::string(reinterpret_cast<const char*>(errors.text.CStr())));
            }
            REQUIRE(loaded.IsOk());
            CHECK(ids->GetGlobal(u8"Played").Get<bool>());
            CHECK(ids->GetGlobal(u8"Spatial").Get<bool>());
            CHECK(ids->GetGlobal(u8"Cue").Get<bool>());
            CHECK(ids->GetGlobal(u8"Cue3D").Get<bool>());
            CHECK(ids->GetGlobal(u8"Music").Get<bool>());
            CHECK_FALSE(ids->GetGlobal(u8"WrongType").Get<bool>());
            CHECK(subsystem.Engine()->ActiveVoiceCount() >= before + 4u);
            CHECK(subsystem.Engine()->BusVolume(AudioBus::Music) == doctest::Approx(0.5f));
            CHECK(ids->GetGlobal(u8"MusicVolume").Get<f64>() == doctest::Approx(0.5));
            // A playing voice from script: the music sped up at once and its volume easing down,
            // a one-shot stopped early (fading, so no longer playing).
            CHECK(ids->GetGlobal(u8"MusicHeld").Get<bool>());
            CHECK(ids->GetGlobal(u8"ShotPlaying").Get<bool>());
            CHECK(ids->GetGlobal(u8"ShotStopped").Get<bool>());
            VoiceStatus controlled;
            REQUIRE(subsystem.Engine()->GetVoiceStatus(subsystem.Engine()->MusicVoice(), controlled));
            CHECK(controlled.pitch == doctest::Approx(1.25f));
            CHECK(controlled.volume == doctest::Approx(0.55f)); // the ease starts where it was
            for (int i = 0; i < 6; ++i)
            {
                subsystem.Engine()->Update(0.1f);
            }
            REQUIRE(subsystem.Engine()->GetVoiceStatus(subsystem.Engine()->MusicVoice(), controlled));
            CHECK(controlled.volume == doctest::Approx(0.3f));
        }

        // In a run (the context's runtime binding names one, as a GameInstance's run host does):
        // the script's music goes into the run's own group and its bus volumes are the run's, so
        // the run's Stop ends the music and its sliders never move the editor's buses.
        {
            utf8char beepText[37];
            utf8char stepsText[37];
            beep->Id().ToChars(beepText);
            steps->Id().ToChars(stepsText);
            struct TestRun final : foundation::run::RunBase
            {
                TestRun() : RunBase(DefaultAllocator(), u8"test") {}
            } runKey;
            foundation::script::ScriptRuntimeBinding runtime;
            runtime.run = &runKey;
            subsystem.Engine()->SetBusVolume(AudioBus::Music, 1.0f);
            RefPtr<foundation::script::IScriptContext> inRun = scripts->CreateContext();
            inRun->SetService(foundation::script::kScriptRuntimeService, &runtime);
            subsystem.ExposeToScript(*inRun, &manager);
            const String byId = idScript(StringView(beepText), StringView(stepsText));
            REQUIRE(inRun->Load(byId.AsView(), u8"main").IsOk());
            const u64 run = subsystem.FindRunGroup(&runKey);
            REQUIRE(run != 0u);
            const VoiceHandle runMusic = subsystem.Engine()->MusicVoice(run);
            CHECK(runMusic.IsValid());
            CHECK(subsystem.Engine()->RunBusVolume(run, AudioBus::Music) == doctest::Approx(0.5f));
            CHECK(subsystem.Engine()->BusVolume(AudioBus::Music) == doctest::Approx(1.0f));
            CHECK(inRun->GetGlobal(u8"MusicVolume").Get<f64>() == doctest::Approx(0.5));
            // A layout's custom bus, by name, in the run: the run's own gain and mute for it.
            AudioBusLayout layout;
            AudioNamedBus drums;
            drums.name = String(u8"drums");
            layout.customBuses.PushBack(drums);
            subsystem.Engine()->ApplyBusLayout(layout);
            REQUIRE(inRun->Load(namedScript, u8"named").IsOk());
            CHECK(subsystem.Engine()->RunNamedBusVolume(run, u8"drums") == doctest::Approx(0.4f));
            CHECK(subsystem.Engine()->RunNamedBusMuted(run, u8"drums"));
            CHECK(subsystem.Engine()->NamedBusVolume(u8"drums") == doctest::Approx(1.0f));
            CHECK_FALSE(subsystem.Engine()->NamedBusMuted(u8"drums"));
            CHECK(inRun->GetGlobal(u8"DrumsVolume").Get<f64>() == doctest::Approx(0.4));

            subsystem.EndRun(&runKey);
            for (int i = 0; i < 4; ++i)
            {
                subsystem.Update(0.1f);
            }
            CHECK_FALSE(subsystem.Engine()->IsValidHandle(runMusic));
        }

        // No binding bound: playback calls report false, never a fault.
        RefPtr<foundation::script::IScriptContext> bare = scripts->CreateContext();
        REQUIRE(bare->Load(bareScript, u8"main").IsOk());
        CHECK_FALSE(bare->GetGlobal(u8"Played").Get<bool>());

        subsystem.Shutdown();
        FileDelete(u8"scratch_audio_scriptdb/sfx/beep.rasset");
        FileDelete(u8"scratch_audio_scriptdb/sfx/beep.data.bin");
        FileDelete(u8"scratch_audio_scriptdb/sfx/steps.rasset");
        RemoveDirectory(u8"scratch_audio_scriptdb/sfx");
        RemoveDirectory(dir);
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("audio-facade: the AngelScript Audio facade plays clips/cues/music by asset id and by "
          "content path, and sets the buses by AudioBus (missing content no-ops, never faults)")
{
    DriveAudioFacade(
        foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator()),
        u8"bool Played; bool Spatial; bool Cue; bool Music; bool Missing; bool MissingAgain;\n"
        u8"void main() {\n"
        u8"  Played = Audio::playOneShotPath(\"sfx/beep\");\n"
        u8"  Spatial = Audio::playOneShot3DPath(\"sfx/beep\", 1.0f, 2.0f, 3.0f);\n"
        u8"  Cue = Audio::playCuePath(\"sfx/steps\");\n"
        u8"  Music = Audio::playMusicPath(\"sfx/beep\", 0.1f);\n"
        u8"  Missing = Audio::playOneShotPath(\"sfx/nope\");\n"
        u8"  MissingAgain = Audio::playOneShotPath(\"sfx/nope\");\n" // warn-once path
        u8"}\n",
        u8"bool Played; void main() { Played = Audio::playOneShotPath(\"sfx/beep\"); }\n",
        [](StringView beep, StringView steps)
        {
            return Format(
                u8"bool Played; bool Spatial; bool Cue; bool Cue3D; bool Music; bool WrongType;\n"
                u8"double MusicVolume; bool MusicHeld; bool ShotPlaying; bool ShotStopped;\n"
                u8"void main() {{\n"
                u8"  Guid clip = Guid(\"{}\");\n"
                u8"  Guid cue = Guid(\"{}\");\n"
                u8"  Played = Audio::playOneShot(clip, AudioBus::Effects, 0.8f, 1.1f).isValid();\n"
                u8"  Spatial = Audio::playOneShot3D(clip, Float3(1.0f, 2.0f, 3.0f)).isValid();\n"
                u8"  Cue = Audio::playCue(cue, AudioBus::UI).isValid();\n"
                u8"  Cue3D = Audio::playCue3D(cue, Float3(0.0f, 1.0f, 0.0f)).isValid();\n"
                u8"  Music = Audio::playMusic(clip, 0.1f, 0.55f).isValid();\n"
                u8"  WrongType = Audio::playOneShot(cue).isValid();\n"
                u8"  Audio::setBusVolume(AudioBus::Music, 0.5f);\n"
                u8"  MusicVolume = Audio::busVolume(AudioBus::Music);\n"
                u8"  VoiceHandle music = Audio::musicVoice();\n"
                u8"  MusicHeld = music.isValid();\n"
                u8"  Audio::setVoicePitch(music, 1.25f);\n"
                u8"  Audio::setVoiceVolume(music, 0.3f, 0.5f);\n"
                u8"  VoiceHandle shot = Audio::playOneShot(clip);\n"
                u8"  ShotPlaying = Audio::isVoicePlaying(shot);\n"
                u8"  Audio::stopVoice(shot, 0.2f);\n"
                u8"  ShotStopped = !Audio::isVoicePlaying(shot);\n"
                u8"}}\n",
                beep, steps);
        },
        u8"double DrumsVolume;\n"
        u8"void named() {\n"
        u8"  Audio::setNamedBusVolume(\"drums\", 0.4f);\n"
        u8"  Audio::setBusMuted(\"drums\", true);\n"
        u8"  DrumsVolume = Audio::namedBusVolume(\"drums\");\n"
        u8"}\n"
        u8"void main() { named(); }\n");
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("audio-facade: the Luau Audio facade plays clips/cues/music by asset id and by content "
          "path, and sets the buses by AudioBus (missing content no-ops, never faults)")
{
    DriveAudioFacade(foundation::script::CreateLuauScriptManager(DefaultAllocator()),
                     u8"Played = Audio.playOneShotPath(\"sfx/beep\")\n"
                     u8"Spatial = Audio.playOneShot3DPath(\"sfx/beep\", 1, 2, 3)\n"
                     u8"Cue = Audio.playCuePath(\"sfx/steps\")\n"
                     u8"Music = Audio.playMusicPath(\"sfx/beep\", 0.1)\n"
                     u8"Missing = Audio.playOneShotPath(\"sfx/nope\")\n"
                     u8"MissingAgain = Audio.playOneShotPath(\"sfx/nope\")\n",
                     u8"Played = Audio.playOneShotPath(\"sfx/beep\")\n",
                     [](StringView beep, StringView steps)
                     {
                         return Format(
                             u8"local clip = Guid.new(\"{}\")\n"
                             u8"local cue = Guid.new(\"{}\")\n"
                             u8"Played = Audio.playOneShot(clip, AudioBus.Effects, 0.8, 1.1):isValid()\n"
                             u8"Spatial = Audio.playOneShot3D(clip, Float3.new(1, 2, 3)):isValid()\n"
                             u8"Cue = Audio.playCue(cue, AudioBus.UI):isValid()\n"
                             u8"Cue3D = Audio.playCue3D(cue, Float3.new(0, 1, 0)):isValid()\n"
                             u8"Music = Audio.playMusic(clip, 0.1, 0.55):isValid()\n"
                             u8"WrongType = Audio.playOneShot(cue):isValid()\n"
                             u8"Audio.setBusVolume(AudioBus.Music, 0.5)\n"
                             u8"MusicVolume = Audio.busVolume(AudioBus.Music)\n"
                             u8"local music = Audio.musicVoice()\n"
                             u8"MusicHeld = music:isValid()\n"
                             u8"Audio.setVoicePitch(music, 1.25)\n"
                             u8"Audio.setVoiceVolume(music, 0.3, 0.5)\n"
                             u8"local shot = Audio.playOneShot(clip)\n"
                             u8"ShotPlaying = Audio.isVoicePlaying(shot)\n"
                             u8"Audio.stopVoice(shot, 0.2)\n"
                             u8"ShotStopped = not Audio.isVoicePlaying(shot)\n",
                             beep, steps);
                     },
                     u8"Audio.setNamedBusVolume(\"drums\", 0.4)\n"
                     u8"Audio.setBusMuted(\"drums\", true)\n"
                     u8"DrumsVolume = Audio.namedBusVolume(\"drums\")\n");
}
#endif // OPTION_HAS_LUAU

#endif // OPTION_HAS_ANGELSCRIPT || OPTION_HAS_LUAU
