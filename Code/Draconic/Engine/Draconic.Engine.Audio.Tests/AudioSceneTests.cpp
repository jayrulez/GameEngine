// draconic.engine.audio tests: the scene integration headless - autoplay on scene
// start, per-frame position/velocity sync (the doppler feed), finished-voice reap into
// the component, the per-scene group pausing with scene simulation, the component
// control surface, the listener component pose, and component serialization round-trip.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cmath>

import draconic.foundation;
import draconic.scene;
import draconic.scene.resource;
import draconic.audio;
import draconic.engine.audio;
import draconic.audio.resource; // cooked records (the Wren path-play test's DB)
import draconic.settings;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.script;
import draconic.script.wren;

using namespace draconic::foundation;
using namespace draconic::audio;
namespace scene = draconic::scene;

namespace
{
    [[nodiscard]] RefPtr<AudioClip> MakeToneClip(f32 seconds, u32 sampleRate = 8000,
                                                 u32 channels = 1)
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

    struct PlayScene
    {
        AudioEngine engine;
        scene::Scene scene{u8"audio-test"};
        AudioSceneSystem* audio = nullptr;

        PlayScene()
            : engine(
                  []
                  {
                      AudioEngineSettings settings;
                      settings.headless = true;
                      settings.dedupeWindowSeconds = 0.0f; // scene tests place explicit voices
                      return settings;
                  }())
        {
            RegisterAudioComponentReflection();
            scene.AddSystem<AudioSourceComponentManager>();
            scene.AddSystem<AudioListenerComponentManager>();
            scene.AddSystem<AudioReverbZoneComponentManager>();
            audio = scene.AddSystem<AudioSceneSystem>();
            audio->SetEngine(&engine);
        }

        scene::EntityHandle AddSource(const RefPtr<AudioClip>& clip, Float3 position,
                                      bool autoPlay = true, bool loop = true)
        {
            scene::EntityHandle e = scene.CreateEntity(u8"source");
            scene.SetLocalPosition(e, position);
            AudioSourceComponent& c = scene.GetSystem<AudioSourceComponentManager>()->Add(e);
            c.clip = clip;
            c.autoPlay = autoPlay;
            c.loop = loop;
            return e;
        }

        void Start()
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        void Frame(f32 deltaTime = 1.0f / 60.0f)
        {
            scene.Update(deltaTime);
            engine.Update(deltaTime);
        }
    };
}

TEST_CASE("audio.scene: autoplay sources start voices at scene start, positioned where "
          "authored; scene stop tears the group down and clears handles")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const scene::EntityHandle e = play.AddSource(clip, Float3{3.0f, 1.0f, -2.0f});
    play.Start();

    auto* sources = play.scene.GetSystem<AudioSourceComponentManager>();
    AudioSourceComponent* c = sources->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->voice.IsValid());
    CHECK(play.engine.IsPlaying(c->voice));
    CHECK(play.audio->SceneGroup() != 0u);

    VoiceStatus status;
    REQUIRE(play.engine.GetVoiceStatus(c->voice, status));
    CHECK(status.spatial);
    CHECK(status.position.x == doctest::Approx(3.0f));
    CHECK(status.position.z == doctest::Approx(-2.0f));

    const VoiceHandle handle = c->voice;
    play.scene.Stop();
    CHECK_FALSE(c->voice.IsValid());                // component handle cleared
    CHECK_FALSE(play.engine.IsValidHandle(handle)); // group teardown freed the voice
    CHECK(play.audio->SceneGroup() == 0u);
}

TEST_CASE("audio.scene: per-frame sync pushes position AND velocity (the doppler feed) "
          "from transform deltas")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const scene::EntityHandle e = play.AddSource(clip, Float3{0.0f, 0.0f, 0.0f});
    play.Start();
    play.Frame(); // primes previousPosition

    // Move 1 unit in x over one 60 Hz frame = 60 u/s along x.
    scene::EntityHandle entity = e;
    play.scene.SetLocalPosition(entity, Float3{1.0f, 0.0f, 0.0f});
    play.Frame();

    AudioSourceComponent* c = play.scene.GetSystem<AudioSourceComponentManager>()->Get(e);
    REQUIRE(c != nullptr);
    VoiceStatus status;
    REQUIRE(play.engine.GetVoiceStatus(c->voice, status));
    CHECK(status.position.x == doctest::Approx(1.0f));
    CHECK(c->previousPosition.x == doctest::Approx(1.0f));
    CHECK(c->hasPreviousPosition);
}

TEST_CASE("audio.scene: pausing scene simulation pauses the scene's voice group; "
          "resuming unpauses it")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    (void)play.AddSource(clip, Float3{0, 0, 0});
    play.Start();
    play.Frame();
    CHECK_FALSE(play.engine.IsSceneGroupPaused(play.audio->SceneGroup()));

    play.scene.SetSimulationEnabled(false);
    play.Frame();
    CHECK(play.engine.IsSceneGroupPaused(play.audio->SceneGroup()));

    play.scene.SetSimulationEnabled(true);
    play.Frame();
    CHECK_FALSE(play.engine.IsSceneGroupPaused(play.audio->SceneGroup()));
}

TEST_CASE("audio.scene: a finished one-shot reaps and the component handle clears")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(0.1f);
    const scene::EntityHandle e =
        play.AddSource(clip, Float3{0, 0, 0}, /*autoPlay=*/true, /*loop=*/false);
    play.Start();

    AudioSourceComponent* c = play.scene.GetSystem<AudioSourceComponentManager>()->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->voice.IsValid());

    for (int i = 0; i < 30 && c->voice.IsValid(); ++i)
    {
        play.Frame(0.05f);
    }
    CHECK_FALSE(c->voice.IsValid());
    CHECK(play.engine.ActiveVoiceCount() == 0u);
}

TEST_CASE("audio.scene: the component control surface - Play/Stop/SetPaused/IsPlaying")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const scene::EntityHandle e = play.AddSource(clip, Float3{1, 2, 3}, /*autoPlay=*/false);
    play.Start();

    AudioSourceComponent* c = play.scene.GetSystem<AudioSourceComponentManager>()->Get(e);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->voice.IsValid()); // no autoplay

    const VoiceHandle voice = play.audio->Play(e);
    REQUIRE(voice.IsValid());
    CHECK(play.audio->IsPlaying(e));

    play.audio->SetPaused(e, true);
    CHECK_FALSE(play.audio->IsPlaying(e));
    play.audio->SetPaused(e, false);
    CHECK(play.audio->IsPlaying(e));

    play.audio->Stop(e);
    CHECK_FALSE(c->voice.IsValid());
    play.Frame(0.1f);
    CHECK(play.engine.ActiveVoiceCount() == 0u);

    // Entities without a source are inert.
    scene::EntityHandle bare = play.scene.CreateEntity(u8"bare");
    CHECK_FALSE(play.audio->Play(bare).IsValid());
    CHECK_FALSE(play.audio->IsPlaying(bare));
}

TEST_CASE("audio.scene: the first ACTIVE listener component drives the scene's listener "
          "pose (position + forward + velocity)")
{
    PlayScene play;
    scene::EntityHandle inactive = play.scene.CreateEntity(u8"inactive-listener");
    play.scene.GetSystem<AudioListenerComponentManager>()->Add(inactive).isActive = false;
    play.scene.SetLocalPosition(inactive, Float3{100.0f, 0.0f, 0.0f});

    scene::EntityHandle listener = play.scene.CreateEntity(u8"listener");
    play.scene.GetSystem<AudioListenerComponentManager>()->Add(listener);
    play.scene.SetLocalPosition(listener, Float3{5.0f, 2.0f, 0.0f});

    play.Start();
    play.Frame();
    REQUIRE(play.audio->ListenerValid());
    CHECK(play.audio->ListenerPosition().x == doctest::Approx(5.0f)); // inactive skipped
    CHECK(play.audio->ListenerForward().z == doctest::Approx(-1.0f)); // identity: -Z
    CHECK(play.audio->ListenerUp().y == doctest::Approx(1.0f));

    // Velocity from transform deltas: 0.6 units over 1/60 s = 36 u/s.
    play.scene.SetLocalPosition(listener, Float3{5.6f, 2.0f, 0.0f});
    play.Frame();
    CHECK(play.audio->ListenerVelocity().x == doctest::Approx(36.0f).epsilon(0.05));
}

TEST_CASE("audio.scene: components round-trip through SerializeScene (authored fields "
          "kept, runtime voice handles NOT serialized)")
{
    RegisterAudioComponentReflection();
    scene::Scene a(u8"level");
    a.AddSystem<AudioSourceComponentManager>();
    a.AddSystem<AudioListenerComponentManager>();

    scene::EntityHandle source = a.CreateEntity(u8"emitter");
    AudioSourceComponent& sc = a.GetSystem<AudioSourceComponentManager>()->Add(source);
    Guid clipId;
    REQUIRE(Guid::TryParse(u8"12345678-1234-4234-8234-123456789abc", clipId));
    sc.clip.SetId(clipId);
    sc.bus = AudioBus::Music;
    sc.busName = String(u8"drums");
    sc.reverbSend = 0.35f;
    sc.volume = 0.7f;
    sc.pitch = 1.25f;
    sc.loop = true;
    sc.spatial = true;
    sc.autoPlay = true;
    sc.priority = 200;
    sc.minDistance = 2.5f;
    sc.maxDistance = 80.0f;
    sc.attenuationModel = AudioAttenuationModel::Linear;
    sc.rolloff = 1.5f;
    sc.dopplerFactor = 0.5f;
    sc.coneInnerAngleDegrees = 45.0f;
    sc.coneOuterAngleDegrees = 90.0f;
    sc.coneOuterGain = 0.25f;
    sc.voice = VoiceHandle{7, 3}; // runtime junk that must NOT survive

    scene::EntityHandle listener = a.CreateEntity(u8"ears");
    a.GetSystem<AudioListenerComponentManager>()->Add(listener).isActive = false;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        scene::SerializeScene(writer, a);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    scene::Scene b;
    b.AddSystem<AudioSourceComponentManager>();
    b.AddSystem<AudioListenerComponentManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        scene::SerializeScene(reader, b);
    }

    scene::EntityHandle loadedSource = b.FindEntity(a.GetEntityId(source));
    REQUIRE(loadedSource.IsAssigned());
    AudioSourceComponent* loaded = b.GetSystem<AudioSourceComponentManager>()->Get(loadedSource);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->clip.id == clipId);
    CHECK(loaded->bus == AudioBus::Music);
    CHECK(loaded->busName.AsView() == StringView(u8"drums"));
    CHECK(loaded->reverbSend == doctest::Approx(0.35f));
    CHECK(loaded->volume == doctest::Approx(0.7f));
    CHECK(loaded->pitch == doctest::Approx(1.25f));
    CHECK(loaded->loop);
    CHECK(loaded->spatial);
    CHECK(loaded->autoPlay);
    CHECK(loaded->priority == 200);
    CHECK(loaded->minDistance == doctest::Approx(2.5f));
    CHECK(loaded->maxDistance == doctest::Approx(80.0f));
    CHECK(loaded->attenuationModel == AudioAttenuationModel::Linear);
    CHECK(loaded->rolloff == doctest::Approx(1.5f));
    CHECK(loaded->dopplerFactor == doctest::Approx(0.5f));
    CHECK(loaded->coneInnerAngleDegrees == doctest::Approx(45.0f));
    CHECK(loaded->coneOuterAngleDegrees == doctest::Approx(90.0f));
    CHECK(loaded->coneOuterGain == doctest::Approx(0.25f));
    CHECK_FALSE(loaded->voice.IsValid()); // runtime handle did not travel

    scene::EntityHandle loadedListener = b.FindEntity(a.GetEntityId(listener));
    REQUIRE(loadedListener.IsAssigned());
    AudioListenerComponent* loadedEars =
        b.GetSystem<AudioListenerComponentManager>()->Get(loadedListener);
    REQUIRE(loadedEars != nullptr);
    CHECK_FALSE(loadedEars->isActive);
}

TEST_CASE("audio.scene: sources sharing one clip each get their OWN voice (dedupe never merges "
          "components)")
{
    // The AudioPlayground bug: four emitters sharing a clip autoplayed in the same
    // instant and the one-shot dedupe window collapsed them into ONE voice - three
    // gizmos were silent. Component plays opt out of dedupe; this test runs with a
    // REAL window (the shared harness's zero window would mask the regression).
    AudioEngineSettings settings;
    settings.headless = true;
    settings.dedupeWindowSeconds = 1.0f / 30.0f;
    AudioEngine engine(settings);
    RegisterAudioComponentReflection();
    scene::Scene scene{u8"audio-dedupe"};
    scene.AddSystem<AudioSourceComponentManager>();
    scene.AddSystem<AudioListenerComponentManager>();
    AudioSceneSystem* audio = scene.AddSystem<AudioSceneSystem>();
    audio->SetEngine(&engine);

    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    auto* sources = scene.GetSystem<AudioSourceComponentManager>();
    scene::EntityHandle entities[4];
    for (int i = 0; i < 4; ++i)
    {
        entities[i] = scene.CreateEntity(u8"emitter");
        scene.SetLocalPosition(entities[i], Float3{static_cast<f32>(i) * 10.0f, 0.0f, 0.0f});
        AudioSourceComponent& c = sources->Add(entities[i]);
        c.clip = clip;
        c.autoPlay = true;
        c.loop = true;
        c.spatial = true;
    }
    scene.Start();
    scene.SetSimulationEnabled(true);

    VoiceHandle voices[4];
    for (int i = 0; i < 4; ++i)
    {
        AudioSourceComponent* c = sources->Get(entities[i]);
        REQUIRE(c != nullptr);
        REQUIRE(c->voice.IsValid());
        CHECK(engine.IsPlaying(c->voice));
        voices[i] = c->voice;
    }
    for (int i = 1; i < 4; ++i)
    {
        CHECK_FALSE(voices[i] == voices[0]);
    }
    CHECK(engine.ActiveVoiceCount() == 4u);
    scene.Stop();
}

TEST_CASE("audio.scene: a source's reverbSend feeds the scene send reverb from Play")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const scene::EntityHandle e = play.AddSource(clip, Float3{1.0f, 0.0f, 0.0f});
    play.scene.GetSystem<AudioSourceComponentManager>()->Get(e)->reverbSend = 0.4f;
    play.Start();

    AudioSourceComponent* c = play.scene.GetSystem<AudioSourceComponentManager>()->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->voice.IsValid());
    VoiceStatus status;
    REQUIRE(play.engine.GetVoiceStatus(c->voice, status));
    CHECK(status.reverbSend == doctest::Approx(0.4f));
    play.Frame();
    CHECK(play.engine.IsPlaying(c->voice));
    play.scene.Stop();
}

TEST_CASE("audio.scene: a source's busName routes its voice onto the layout's custom bus")
{
    PlayScene play;
    AudioBusLayout layout;
    AudioNamedBus drums;
    drums.name = String(u8"drums");
    drums.parent = String(u8"Effects");
    layout.customBuses.PushBack(drums);
    play.engine.ApplyBusLayout(layout);

    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const scene::EntityHandle e = play.AddSource(clip, Float3{0.0f, 0.0f, 0.0f});
    play.scene.GetSystem<AudioSourceComponentManager>()->Get(e)->busName = String(u8"drums");
    play.Start();

    AudioSourceComponent* c = play.scene.GetSystem<AudioSourceComponentManager>()->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->voice.IsValid());
    VoiceStatus status;
    REQUIRE(play.engine.GetVoiceStatus(c->voice, status));
    CHECK(status.busName.AsView() == StringView(u8"drums"));

    // Scene pause freezes custom-bus voices with the rest of the scene.
    play.scene.SetSimulationEnabled(false);
    play.Frame();
    CHECK(play.engine.IsValidHandle(c->voice));
    play.scene.SetSimulationEnabled(true);
    play.Frame();
    CHECK(play.engine.IsPlaying(c->voice));
    play.scene.Stop();
}

TEST_CASE("audio.scene: the Wren Audio facade plays clips/cues/music by CONTENT PATH "
          "through the resource seam (missing paths no-op, never fault)")
{
    RegisterAudioScriptFacade();
    RegisterAudioResource();

    // A hand-cooked content DB: sfx/beep (clip) + sfx/steps (cue referencing it).
    const StringView dir = u8"draconic_audio_scriptdb";
    FileDelete(u8"draconic_audio_scriptdb/sfx/beep.rasset");
    FileDelete(u8"draconic_audio_scriptdb/sfx/beep.data.bin");
    FileDelete(u8"draconic_audio_scriptdb/sfx/steps.rasset");
    RemoveDirectory(u8"draconic_audio_scriptdb/sfx");
    RemoveDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);
    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    draconic::content::Group* sfx = db.RootGroup()->CreateGroup(u8"sfx");
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

    AudioClipFactory clipFactory;
    SoundCueFactory cueFactory;
    draconic::resource::ResourceManager manager(db);
    manager.AddFactory(&clipFactory);
    manager.AddFactory(&cueFactory);

    // A headless subsystem (Init is the public lifecycle seam) + the script binding.
    AudioEngineSettings engineSettings;
    engineSettings.headless = true;
    engineSettings.dedupeWindowSeconds = 0.0f; // each facade call = its own voice
    AudioSubsystem subsystem(engineSettings);
    subsystem.Init();
    REQUIRE(subsystem.Engine() != nullptr);

    RefPtr<draconic::script::IScriptManager> scripts =
        draconic::script::wren::CreateScriptManager();
    draconic::script::RegisterReflectedTypes(*scripts);
    RefPtr<draconic::script::IScriptContext> ctx = scripts->CreateContext();
    REQUIRE(ctx.Get() != nullptr);
    subsystem.ExposeToScript(*ctx, &manager);

    const StringView script =
        u8"var Played = Audio.playOneShot(\"sfx/beep\")\n"
        u8"var Spatial = Audio.playOneShot3D(\"sfx/beep\", 1, 2, 3)\n"
        u8"var Cue = Audio.playCue(\"sfx/steps\")\n"
        u8"var Music = Audio.playMusic(\"sfx/beep\", 0.1)\n"
        u8"var Missing = Audio.playOneShot(\"sfx/nope\")\n"
        u8"var MissingAgain = Audio.playOneShot(\"sfx/nope\")\n"; // warn-once path
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

    // No binding bound: playback calls report false, never a fault.
    RefPtr<draconic::script::IScriptContext> bare = scripts->CreateContext();
    REQUIRE(bare->Load(u8"var Played = Audio.playOneShot(\"sfx/beep\")\n", u8"main").IsOk());
    CHECK_FALSE(bare->GetGlobal(u8"Played").Get<bool>());

    subsystem.Shutdown();
    FileDelete(u8"draconic_audio_scriptdb/sfx/beep.rasset");
    FileDelete(u8"draconic_audio_scriptdb/sfx/beep.data.bin");
    FileDelete(u8"draconic_audio_scriptdb/sfx/steps.rasset");
    RemoveDirectory(u8"draconic_audio_scriptdb/sfx");
    RemoveDirectory(dir);
}

TEST_CASE("audio.settings: user volumes capture -> store round-trip -> apply")
{
    RegisterAudioSettingsTypes();

    AudioEngineSettings engineSettings;
    engineSettings.headless = true;
    AudioEngine engine(engineSettings);
    engine.SetBusVolume(AudioBus::Music, 0.25f);
    engine.SetBusMuted(AudioBus::Effects, true);

    draconic::settings::Settings store;
    CaptureAudioUserSettings(engine, store.Section<AudioUserSettings>());

    MemoryStream buffer;
    REQUIRE(store.Save(buffer, BinarySerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);
    draconic::settings::Settings loaded;
    REQUIRE(loaded.Load(buffer, BinarySerializerFactory()).IsOk());
    const AudioUserSettings* user = loaded.Find<AudioUserSettings>();
    REQUIRE(user != nullptr);
    CHECK(user->volumes[static_cast<usize>(AudioBus::Music)] == doctest::Approx(0.25f));
    CHECK(user->muted[static_cast<usize>(AudioBus::Effects)]);

    // Applying onto a fresh engine reproduces the mixer state.
    AudioEngine fresh(engineSettings);
    ApplyAudioUserSettings(fresh, *user);
    CHECK(fresh.BusVolume(AudioBus::Music) == doctest::Approx(0.25f));
    CHECK(fresh.BusMuted(AudioBus::Effects));
    CHECK(fresh.BusVolume(AudioBus::Master) == doctest::Approx(1.0f));
}

TEST_CASE("audio.scene: a cue on the source wins over the clip and varies per trigger")
{
    PlayScene play;
    RefPtr<AudioClip> fallback = MakeToneClip(0.2f);
    RefPtr<AudioClip> stepA = MakeToneClip(0.2f);
    RefPtr<AudioClip> stepB = MakeToneClip(0.2f);
    RefPtr<SoundCue> cue = MakeRef<SoundCue>(DefaultAllocator());
    cue->variants.PushBack(SoundCueVariant{stepA, 1.0f});
    cue->variants.PushBack(SoundCueVariant{stepB, 1.0f});
    cue->pitchMin = 0.8f;
    cue->pitchMax = 1.2f;

    const scene::EntityHandle e = play.AddSource(fallback, Float3{0, 0, 0},
                                                 /*autoPlay=*/false, /*loop=*/false);
    auto* sources = play.scene.GetSystem<AudioSourceComponentManager>();
    sources->Get(e)->cue = cue;
    play.Start();

    // Two-variant no-repeat: repeated triggers alternate; jitter lands in range.
    i32 first = -1;
    for (int i = 0; i < 8; ++i)
    {
        const VoiceHandle voice = play.audio->Play(e);
        REQUIRE(voice.IsValid());
        VoiceStatus status;
        REQUIRE(play.engine.GetVoiceStatus(voice, status));
        CHECK(status.pitch >= 0.8f);
        CHECK(status.pitch <= 1.2f);
        AudioSourceComponent* c = sources->Get(e);
        REQUIRE(c != nullptr);
        if (first >= 0)
        {
            CHECK(c->lastCueVariant != first);
        }
        first = c->lastCueVariant;
        play.engine.Stop(voice);
        for (int f = 0; f < 5; ++f)
        {
            play.Frame();
        }
    }
}

TEST_CASE("audio.scene: reverb zones - wet follows listener occupancy, wettest zone wins")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    play.AddSource(clip, Float3{0, 0, 0});

    // Listener entity at the origin.
    scene::EntityHandle listener = play.scene.CreateEntity(u8"ears");
    play.scene.GetSystem<AudioListenerComponentManager>()->Add(listener);

    // Zone A centered at origin (r=10, wet .5); zone B overlapping, wetter (r=4, wet .9).
    scene::EntityHandle za = play.scene.CreateEntity(u8"hall");
    auto* zones = play.scene.GetSystem<AudioReverbZoneComponentManager>();
    REQUIRE(zones != nullptr);
    AudioReverbZoneComponent& zoneA = zones->Add(za);
    zoneA.radius = 10.0f;
    zoneA.wetLevel = 0.5f;
    zoneA.edgeFade = 0.5f;
    scene::EntityHandle zb = play.scene.CreateEntity(u8"cave");
    AudioReverbZoneComponent& zoneB = zones->Add(zb);
    zoneB.radius = 4.0f;
    zoneB.wetLevel = 0.9f;
    zoneB.edgeFade = 0.25f;

    play.Start();
    play.Frame();
    const u64 group = play.audio->SceneGroup();
    REQUIRE(group != 0u);

    // Deep inside both: the wetter zone wins at full blend.
    CHECK(play.engine.SceneReverbWet(group) == doctest::Approx(0.9f).epsilon(0.02));

    // Move the listener outside B but into A's edge band: partial A wet.
    play.scene.SetLocalPosition(listener, Float3{8.0f, 0.0f, 0.0f});
    play.scene.UpdateTransforms();
    play.Frame();
    const f32 edgeWet = play.engine.SceneReverbWet(group);
    CHECK(edgeWet > 0.0f);
    CHECK(edgeWet < 0.45f);

    // Far outside every zone: dry.
    play.scene.SetLocalPosition(listener, Float3{50.0f, 0.0f, 0.0f});
    play.scene.UpdateTransforms();
    play.Frame();
    CHECK(play.engine.SceneReverbWet(group) == doctest::Approx(0.0f));
}

TEST_CASE("audio.scene: multi-listener - every active listener collects, first is primary")
{
    PlayScene play;
    RefPtr<AudioClip> clip = MakeToneClip(0.5f);
    play.AddSource(clip, Float3{0, 0, 0});

    auto* listeners = play.scene.GetSystem<AudioListenerComponentManager>();
    scene::EntityHandle earsA = play.scene.CreateEntity(u8"p1");
    play.scene.SetLocalPosition(earsA, Float3{-5.0f, 0.0f, 0.0f});
    listeners->Add(earsA);
    scene::EntityHandle earsB = play.scene.CreateEntity(u8"p2");
    play.scene.SetLocalPosition(earsB, Float3{5.0f, 0.0f, 0.0f});
    listeners->Add(earsB);
    scene::EntityHandle earsOff = play.scene.CreateEntity(u8"spectator");
    listeners->Add(earsOff).isActive = false; // inactive: never collected

    play.Start();
    play.scene.UpdateTransforms();
    play.Frame();

    const Span<const ListenerPose> poses = play.audio->ListenerPoses();
    REQUIRE(poses.Size() == 2u);
    CHECK(poses[0].position.x == doctest::Approx(-5.0f)); // first component = primary
    CHECK(poses[1].position.x == doctest::Approx(5.0f));
    CHECK(play.audio->ListenerPosition().x == doctest::Approx(-5.0f));
}

TEST_CASE("audio.engine: listener slots honor the configured count and reject OOB")
{
    AudioEngineSettings settings;
    settings.headless = true;
    settings.listenerCount = 3;
    AudioEngine engine(settings);
    CHECK(engine.ListenerCount() == 3u);
    engine.SetListenerTransformIndexed(2, Float3{1, 2, 3}, Float3{0, 0, -1}, Float3{0, 1, 0},
                                       Float3{});
    engine.SetListenerTransformIndexed(7, Float3{}, Float3{0, 0, -1}, Float3{0, 1, 0},
                                       Float3{}); // OOB: no-op
    engine.SetListenerEnabled(1, false);
    engine.Update(1.0f / 60.0f); // pump survives partial listener config
}
