// draconic.audio core tests: the HEADLESS engine (no device - Update() pumps the mixer,
// so the whole voice state machine runs deterministically): handle validity across slot
// generations, the Traktor steal policy (free -> lower priority -> farthest same
// priority), fade-then-reap on stop, pause/resume, recent-play dedupe, buses, per-scene
// groups, streamed voices through the IAudioStreamSource seam, and the codec helpers.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cmath>

import draconic.foundation;
import draconic.audio;

using namespace draconic::foundation;
using namespace draconic::audio;

namespace
{
    [[nodiscard]] Array<i16> MakeTone(f32 seconds, u32 sampleRate, u32 channels,
                                      f32 frequency = 440.0f, f32 amplitude = 0.5f)
    {
        Array<i16> samples;
        const usize frameCount = static_cast<usize>(seconds * static_cast<f32>(sampleRate));
        for (usize frame = 0; frame < frameCount; ++frame)
        {
            const f32 t = static_cast<f32>(frame) / static_cast<f32>(sampleRate);
            const f32 value = amplitude * std::sin(2.0f * 3.14159265f * frequency * t);
            const i16 sample = static_cast<i16>(value * 32000.0f);
            for (u32 channel = 0; channel < channels; ++channel)
            {
                samples.PushBack(sample);
            }
        }
        return samples;
    }

    [[nodiscard]] RefPtr<AudioClip> MakeToneClip(f32 seconds, u32 sampleRate = 8000,
                                                 u32 channels = 1)
    {
        const Array<i16> samples = MakeTone(seconds, sampleRate, channels);
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

    [[nodiscard]] AudioEngineSettings HeadlessSettings(u32 voiceCount = 8, u32 streamVoiceCount = 2,
                                                       f32 dedupeWindowSeconds = 0.0f)
    {
        AudioEngineSettings settings;
        settings.headless = true;
        settings.voiceCount = voiceCount;
        settings.streamVoiceCount = streamVoiceCount;
        settings.dedupeWindowSeconds = dedupeWindowSeconds;
        return settings;
    }

    // Re-openable in-memory stream source (what the cooked-content adapter does over paks).
    class MemoryStreamSource final : public IAudioStreamSource
    {
    public:
        explicit MemoryStreamSource(Array<byte> bytes) : m_bytes(Move(bytes)) {}
        [[nodiscard]] UniquePtr<IStream> OpenStream() override
        {
            UniquePtr<MemoryStream> stream = MakeUnique<MemoryStream>(DefaultAllocator());
            if (stream->Write(m_bytes.Data(), m_bytes.Size()) != m_bytes.Size())
            {
                return {};
            }
            (void)stream->Seek(0, SeekOrigin::Begin);
            return UniquePtr<IStream>(stream.Release(), DefaultAllocator());
        }

    private:
        Array<byte> m_bytes;
    };
}

TEST_CASE("audio.codec: wav encode -> probe -> decode round-trip")
{
    const Array<i16> samples = MakeTone(0.25f, 8000, 2);
    Array<byte> wav;
    REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 2, 8000, wav));

    AudioClipMetadata metadata;
    REQUIRE(ProbeAudioClipMetadata(Span<const byte>(wav.Data(), wav.Size()), metadata));
    CHECK(metadata.channels == 2u);
    CHECK(metadata.sampleRate == 8000u);
    CHECK(metadata.frameCount == samples.Size() / 2);
    CHECK(metadata.durationSeconds == doctest::Approx(0.25f).epsilon(0.01));

    Array<i16> decoded;
    AudioClipMetadata decodedMetadata;
    REQUIRE(DecodeAudioClipToPcm16(Span<const byte>(wav.Data(), wav.Size()), 0, decoded,
                                   decodedMetadata));
    REQUIRE(decoded.Size() == samples.Size());
    CHECK(decoded[100] == samples[100]);
    CHECK(decoded[101] == samples[101]);

    // Force-mono downmix halves the sample count and keeps the frame count.
    Array<i16> mono;
    AudioClipMetadata monoMetadata;
    REQUIRE(
        DecodeAudioClipToPcm16(Span<const byte>(wav.Data(), wav.Size()), 1, mono, monoMetadata));
    CHECK(monoMetadata.channels == 1u);
    CHECK(monoMetadata.frameCount == metadata.frameCount);
    CHECK(mono.Size() == metadata.frameCount);

    // Garbage bytes are rejected, not misread.
    Array<byte> garbage;
    for (int i = 0; i < 64; ++i)
    {
        garbage.PushBack(static_cast<byte>(i * 7));
    }
    AudioClipMetadata rejected;
    CHECK_FALSE(ProbeAudioClipMetadata(Span<const byte>(garbage.Data(), garbage.Size()), rejected));
}

TEST_CASE("audio.engine: headless construction, empty/invalid plays are safely rejected")
{
    AudioEngine engine(HeadlessSettings());
    CHECK(engine.IsHeadless());
    CHECK(engine.ActiveVoiceCount() == 0u);

    CHECK_FALSE(engine.Play(RefPtr<AudioClip>{}).IsValid());
    RefPtr<AudioClip> empty = MakeRef<AudioClip>(DefaultAllocator());
    CHECK_FALSE(engine.Play(empty).IsValid());

    RefPtr<AudioClip> garbage = MakeRef<AudioClip>(DefaultAllocator());
    for (int i = 0; i < 64; ++i)
    {
        garbage->encodedData.PushBack(static_cast<byte>(i));
    }
    garbage->channels = 1;
    CHECK_FALSE(engine.Play(garbage).IsValid());

    // Stale/foreign handles are inert everywhere.
    VoiceHandle bogus{3, 7};
    CHECK_FALSE(engine.IsValidHandle(bogus));
    CHECK_FALSE(engine.IsPlaying(bogus));
    engine.Stop(bogus);
    engine.SetPaused(bogus, true);
    engine.SetVoiceVolume(bogus, 0.5f);
    engine.SetVoicePosition(bogus, Float3{1, 2, 3}, Float3{0, 0, 0});
    engine.Update(0.1f);
}

TEST_CASE("audio.engine: a one-shot plays, reaches its end, and reaps - the slot's "
          "generation invalidates the old handle on reuse")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(0.1f);

    const VoiceHandle first = engine.Play(clip);
    REQUIRE(first.IsValid());
    CHECK(engine.IsPlaying(first));
    CHECK(engine.ActiveVoiceCount() == 1u);

    for (int i = 0; i < 6 && engine.IsValidHandle(first); ++i)
    {
        engine.Update(0.05f);
    }
    CHECK_FALSE(engine.IsValidHandle(first));
    CHECK(engine.ActiveVoiceCount() == 0u);

    // The freed slot comes back with a NEW generation: same slot, old handle stays dead.
    const VoiceHandle second = engine.Play(clip);
    REQUIRE(second.IsValid());
    CHECK(second.slot == first.slot);
    CHECK(second.generation != first.generation);
    CHECK_FALSE(engine.IsValidHandle(first));
    CHECK(engine.IsPlaying(second));
}

TEST_CASE("audio.engine: stop always fades (~10 ms) then reaps - the fade-then-reap "
          "state machine is observable")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(0.5f);
    AudioPlayParams params;
    params.loop = true;

    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    engine.Update(0.05f);
    CHECK(engine.IsPlaying(voice));

    engine.Stop(voice);
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.stopping); // fading out, not yet reaped
    CHECK_FALSE(engine.IsPlaying(voice));
    CHECK(engine.IsValidHandle(voice));

    engine.Update(0.1f); // 100 ms >> the 10 ms fade
    CHECK_FALSE(engine.IsValidHandle(voice));
    CHECK(engine.ActiveVoiceCount() == 0u);
}

TEST_CASE("audio.engine: pause fades out but keeps the voice; resume fades back in")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(0.3f);
    AudioPlayParams params;
    params.loop = true;

    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    engine.SetPaused(voice, true);
    CHECK_FALSE(engine.IsPlaying(voice));
    CHECK(engine.IsValidHandle(voice));

    // A paused voice never reaps, no matter how long the engine runs.
    for (int i = 0; i < 10; ++i)
    {
        engine.Update(0.1f);
    }
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.paused);

    engine.SetPaused(voice, false);
    CHECK(engine.IsPlaying(voice));

    engine.Stop(voice);
    engine.Update(0.1f);
    CHECK_FALSE(engine.IsValidHandle(voice));
}

TEST_CASE("audio.engine: steal policy - free slot, then lowest lower priority, then "
          "farthest same priority; all-higher pools reject the play")
{
    AudioEngine engine(HeadlessSettings(/*voiceCount=*/2, /*streamVoiceCount=*/0));
    RefPtr<AudioClip> clipA = MakeToneClip(2.0f);
    RefPtr<AudioClip> clipB = MakeToneClip(2.0f, 8000, 1);
    RefPtr<AudioClip> clipC = MakeToneClip(2.0f, 4000, 1);
    RefPtr<AudioClip> clipD = MakeToneClip(2.0f, 16000, 1);

    AudioPlayParams low;
    low.priority = 10;
    low.loop = true;
    const VoiceHandle voiceA = engine.Play(clipA, low);
    low.priority = 20;
    const VoiceHandle voiceB = engine.Play(clipB, low);
    REQUIRE(voiceA.IsValid());
    REQUIRE(voiceB.IsValid());
    CHECK(engine.ActiveVoiceCount() == 2u);

    // Pool full: a HIGHER-priority play steals the LOWEST priority below it (A at 10).
    // FADED steal: A's handle dies at once but its ma_sound is NOT uninitialized yet -
    // it fades on the dying side list while C already plays (no click, brief overlap).
    AudioPlayParams high;
    high.priority = 30;
    high.loop = true;
    const VoiceHandle voiceC = engine.Play(clipC, high);
    REQUIRE(voiceC.IsValid());
    CHECK_FALSE(engine.IsValidHandle(voiceA));
    CHECK(engine.IsValidHandle(voiceB));
    CHECK(engine.ActiveVoiceCount() == 2u); // addressable voices only
    CHECK(engine.DyingVoiceCount() == 1u);  // A's tail is still mixing

    // The steal fade (~30 ms) lands and the tail reaps.
    for (int i = 0; i < 10; ++i)
    {
        engine.Update(0.05f);
    }
    CHECK(engine.DyingVoiceCount() == 0u);

    // Pool full of strictly-higher priorities: the new play is REJECTED.
    AudioPlayParams lowest;
    lowest.priority = 5;
    CHECK_FALSE(engine.Play(clipD, lowest).IsValid());
    CHECK(engine.IsValidHandle(voiceB));
    CHECK(engine.IsValidHandle(voiceC));
}

TEST_CASE("audio.engine: same-priority contention steals the voice FARTHEST from the listener")
{
    AudioEngine engine(HeadlessSettings(/*voiceCount=*/2, /*streamVoiceCount=*/0));
    engine.SetListenerTransform(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0},
                                Float3{0, 0, 0});
    RefPtr<AudioClip> clipNear = MakeToneClip(2.0f);
    RefPtr<AudioClip> clipFar = MakeToneClip(2.0f, 8000, 1);
    RefPtr<AudioClip> clipNew = MakeToneClip(2.0f, 4000, 1);

    AudioPlayParams spatial;
    spatial.loop = true;
    spatial.spatial = true;
    spatial.priority = 50;
    spatial.position = Float3{1.0f, 0.0f, 0.0f};
    const VoiceHandle nearVoice = engine.Play(clipNear, spatial);
    spatial.position = Float3{60.0f, 0.0f, 0.0f};
    const VoiceHandle farVoice = engine.Play(clipFar, spatial);
    REQUIRE(nearVoice.IsValid());
    REQUIRE(farVoice.IsValid());

    spatial.position = Float3{2.0f, 0.0f, 0.0f};
    const VoiceHandle newVoice = engine.Play(clipNew, spatial);
    REQUIRE(newVoice.IsValid());
    CHECK(engine.IsValidHandle(nearVoice));      // near survived
    CHECK_FALSE(engine.IsValidHandle(farVoice)); // far was stolen
    CHECK(engine.DyingVoiceCount() == 1u);       // ... but its tail fades, no hard cut
}

TEST_CASE("audio.engine: faded steal - the dying list is capacity-bounded (oldest "
          "hard-cuts) and paused victims skip it")
{
    AudioEngineSettings settings = HeadlessSettings(/*voiceCount=*/1, /*streamVoiceCount=*/0);
    settings.dyingVoiceCapacity = 2;
    AudioEngine engine(settings);
    RefPtr<AudioClip> clips[4] = {MakeToneClip(1.0f), MakeToneClip(1.0f, 4000, 1),
                                  MakeToneClip(1.0f, 16000, 1), MakeToneClip(1.0f, 12000, 1)};

    // Four same-frame plays through a 1-slot pool: each steals the incumbent. The
    // dying list holds at most 2 tails; the overflow hard-cut the oldest.
    AudioPlayParams params;
    params.loop = true;
    params.allowDedupe = false;
    VoiceHandle last;
    for (int i = 0; i < 4; ++i)
    {
        last = engine.Play(clips[i], params);
        REQUIRE(last.IsValid());
    }
    CHECK(engine.ActiveVoiceCount() == 1u);
    CHECK(engine.DyingVoiceCount() == 2u);

    // All tails reap once their fades land; the survivor keeps playing.
    for (int i = 0; i < 10; ++i)
    {
        engine.Update(0.05f);
    }
    CHECK(engine.DyingVoiceCount() == 0u);
    CHECK(engine.IsPlaying(last));

    // A PAUSED victim is already silent: stealing it never busies the dying list.
    engine.SetPaused(last, true);
    const VoiceHandle successor = engine.Play(clips[0], params);
    REQUIRE(successor.IsValid());
    CHECK_FALSE(engine.IsValidHandle(last));
    CHECK(engine.DyingVoiceCount() == 0u);

    // Capacity 0 = the legacy immediate cut.
    AudioEngineSettings immediate = HeadlessSettings(/*voiceCount=*/1, /*streamVoiceCount=*/0);
    immediate.dyingVoiceCapacity = 0;
    AudioEngine hardEngine(immediate);
    REQUIRE(hardEngine.Play(clips[0], params).IsValid());
    REQUIRE(hardEngine.Play(clips[1], params).IsValid());
    CHECK(hardEngine.DyingVoiceCount() == 0u);
    CHECK(hardEngine.ActiveVoiceCount() == 1u);
}

TEST_CASE("audio.engine: recent-play dedupe merges same-clip plays inside the window")
{
    AudioEngineSettings settings = HeadlessSettings();
    settings.dedupeWindowSeconds = 1.0f / 30.0f;
    AudioEngine engine(settings);
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    AudioPlayParams params;
    params.loop = true;

    const VoiceHandle first = engine.Play(clip, params);
    const VoiceHandle merged = engine.Play(clip, params); // same frame: merges
    REQUIRE(first.IsValid());
    CHECK(merged == first);
    CHECK(engine.ActiveVoiceCount() == 1u);

    engine.Update(0.1f); // window expired
    const VoiceHandle second = engine.Play(clip, params);
    REQUIRE(second.IsValid());
    CHECK_FALSE(second == first);
    CHECK(engine.ActiveVoiceCount() == 2u);
}

TEST_CASE("audio.engine: dedupe opt-out - persistent sources sharing a clip stay distinct")
{
    // The component path plays with allowDedupe=false: four authored emitters sharing
    // one clip autoplay in the SAME instant and must each get their own voice (the
    // one-shot window silently collapsed all-but-one emitter - the AudioPlayground bug).
    AudioEngineSettings settings = HeadlessSettings();
    settings.dedupeWindowSeconds = 1.0f / 30.0f;
    AudioEngine engine(settings);
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    AudioPlayParams params;
    params.loop = true;
    params.spatial = true;
    params.allowDedupe = false;

    VoiceHandle voices[4];
    for (int i = 0; i < 4; ++i)
    {
        params.position = Float3{static_cast<f32>(i) * 10.0f, 0.0f, 0.0f};
        voices[i] = engine.Play(clip, params);
        REQUIRE(voices[i].IsValid());
    }
    CHECK(engine.ActiveVoiceCount() == 4u);
    for (int i = 1; i < 4; ++i)
    {
        CHECK_FALSE(voices[i] == voices[0]);
    }

    // Opt-out plays must not ARM the window either: a later defaulted (dedupe-eligible)
    // play still creates a fresh voice instead of merging into a persistent source.
    AudioPlayParams oneShot;
    const VoiceHandle shot = engine.Play(clip, oneShot);
    REQUIRE(shot.IsValid());
    for (int i = 0; i < 4; ++i)
    {
        CHECK_FALSE(shot == voices[i]);
    }
    CHECK(engine.ActiveVoiceCount() == 5u);
}

TEST_CASE("audio.engine: voice parameter setters land (volume/pitch/pan/position/looping)")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(0.5f);
    AudioPlayParams params;
    params.spatial = true;
    params.loop = true;
    params.pitch = 1.5f; // real resampling, not stored-and-ignored
    params.volume = 0.75f;

    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.pitch == doctest::Approx(1.5f));
    CHECK(status.volume == doctest::Approx(0.75f));
    CHECK(status.spatial);

    engine.SetVoicePitch(voice, 0.5f);
    engine.SetVoiceVolume(voice, 0.25f);
    engine.SetVoicePosition(voice, Float3{3, 4, 5}, Float3{1, 0, 0});
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.pitch == doctest::Approx(0.5f));
    CHECK(status.volume == doctest::Approx(0.25f));
    CHECK(status.position.x == doctest::Approx(3.0f));
    CHECK(status.position.z == doctest::Approx(5.0f));

    engine.SetVoiceLooping(voice, false);
    for (int i = 0; i < 20 && engine.IsValidHandle(voice); ++i)
    {
        engine.Update(0.1f);
    }
    CHECK_FALSE(engine.IsValidHandle(voice)); // un-looped voice runs out and reaps
}

TEST_CASE("audio.engine: spatializing a stereo clip downmixes with a one-time warning "
          "(runtime mono-guard) and still plays")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> stereo = MakeToneClip(0.2f, 8000, 2);
    AudioPlayParams params;
    params.spatial = true;
    const VoiceHandle voice = engine.Play(stereo, params);
    REQUIRE(voice.IsValid());
    CHECK(engine.IsPlaying(voice));
    engine.Update(0.05f);
    engine.Stop(voice);
    engine.Update(0.1f);
    CHECK_FALSE(engine.IsValidHandle(voice));
}

TEST_CASE("audio.engine: bus volumes and mutes are independent and re-appliable")
{
    AudioEngine engine(HeadlessSettings());
    CHECK(engine.BusVolume(AudioBus::Master) == doctest::Approx(1.0f));
    engine.SetBusVolume(AudioBus::Music, 0.3f);
    CHECK(engine.BusVolume(AudioBus::Music) == doctest::Approx(0.3f));
    CHECK(engine.BusVolume(AudioBus::Effects) == doctest::Approx(1.0f));

    engine.SetBusMuted(AudioBus::Music, true);
    CHECK(engine.BusMuted(AudioBus::Music));
    CHECK(engine.BusVolume(AudioBus::Music) == doctest::Approx(0.3f)); // remembered
    engine.SetBusMuted(AudioBus::Music, false);
    CHECK_FALSE(engine.BusMuted(AudioBus::Music));
}

TEST_CASE("audio.engine: per-scene groups - pause halts the scene's voices in place, "
          "stop fades them out, destroy frees them immediately")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    const u64 sceneGroup = engine.CreateSceneGroup();
    REQUIRE(sceneGroup != 0u);

    AudioPlayParams params;
    params.loop = true;
    params.sceneGroup = sceneGroup;
    const VoiceHandle voice = engine.Play(clip, params);
    AudioPlayParams globalParams;
    globalParams.loop = true;
    RefPtr<AudioClip> globalClip = MakeToneClip(1.0f, 4000, 1);
    const VoiceHandle globalVoice = engine.Play(globalClip, globalParams);
    REQUIRE(voice.IsValid());
    REQUIRE(globalVoice.IsValid());

    engine.SetSceneGroupPaused(sceneGroup, true);
    CHECK(engine.IsSceneGroupPaused(sceneGroup));
    for (int i = 0; i < 5; ++i)
    {
        engine.Update(0.1f);
    }
    CHECK(engine.IsValidHandle(voice));       // held, not reaped
    CHECK(engine.IsValidHandle(globalVoice)); // untouched by the scene pause

    engine.SetSceneGroupPaused(sceneGroup, false);
    CHECK_FALSE(engine.IsSceneGroupPaused(sceneGroup));

    engine.StopSceneGroup(sceneGroup);
    engine.Update(0.2f);
    CHECK_FALSE(engine.IsValidHandle(voice));
    CHECK(engine.IsValidHandle(globalVoice));

    const VoiceHandle again = engine.Play(clip, params);
    REQUIRE(again.IsValid());
    engine.DestroySceneGroup(sceneGroup);
    CHECK_FALSE(engine.IsValidHandle(again)); // immediate teardown
    CHECK(engine.IsValidHandle(globalVoice));
}

TEST_CASE("audio.engine: streamed clips play from an IAudioStreamSource through the "
          "stream voice pool (the pak-facing seam)")
{
    AudioEngine engine(HeadlessSettings(/*voiceCount=*/2, /*streamVoiceCount=*/1));
    const Array<i16> samples = MakeTone(0.5f, 8000, 1);
    Array<byte> wav;
    REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, 8000, wav));

    RefPtr<AudioClip> clip = MakeRef<AudioClip>(DefaultAllocator());
    AudioClipMetadata metadata;
    REQUIRE(ProbeAudioClipMetadata(Span<const byte>(wav.Data(), wav.Size()), metadata));
    clip->channels = metadata.channels;
    clip->sampleRate = metadata.sampleRate;
    clip->frameCount = metadata.frameCount;
    clip->durationSeconds = metadata.durationSeconds;
    clip->stream = true;
    clip->streamSource = UniquePtr<IAudioStreamSource>(
        DefaultAllocator().New<MemoryStreamSource>(wav), DefaultAllocator());

    AudioPlayParams params;
    params.bus = AudioBus::Music;
    params.loop = true;
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    CHECK(voice.slot == 2u); // stream slots sit after the main pool
    CHECK(engine.IsPlaying(voice));
    engine.Update(0.1f);
    CHECK(engine.IsPlaying(voice));

    // The stream pool is its own contention domain: a second stream play cannot steal
    // a same-priority spatial=false voice... but CAN reject when full of higher priority.
    AudioPlayParams lower;
    lower.bus = AudioBus::Music;
    lower.priority = 1;
    RefPtr<AudioClip> other = MakeRef<AudioClip>(DefaultAllocator());
    other->channels = metadata.channels;
    other->sampleRate = metadata.sampleRate;
    other->frameCount = metadata.frameCount;
    other->durationSeconds = metadata.durationSeconds;
    other->stream = true;
    other->streamSource = UniquePtr<IAudioStreamSource>(
        DefaultAllocator().New<MemoryStreamSource>(wav), DefaultAllocator());
    CHECK_FALSE(engine.Play(other, lower).IsValid());

    engine.Stop(voice);
    engine.Update(0.2f);
    CHECK_FALSE(engine.IsValidHandle(voice));
}

TEST_CASE("audio.engine: StopAll fades every voice out")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clipA = MakeToneClip(1.0f);
    RefPtr<AudioClip> clipB = MakeToneClip(1.0f, 4000, 1);
    AudioPlayParams params;
    params.loop = true;
    const VoiceHandle voiceA = engine.Play(clipA, params);
    const VoiceHandle voiceB = engine.Play(clipB, params);
    REQUIRE(voiceA.IsValid());
    REQUIRE(voiceB.IsValid());

    engine.StopAll();
    engine.Update(0.2f);
    CHECK(engine.ActiveVoiceCount() == 0u);
    CHECK_FALSE(engine.IsValidHandle(voiceA));
    CHECK_FALSE(engine.IsValidHandle(voiceB));
}

TEST_CASE("audio.waveform: peaks bucket the decoded signal; silence reads near zero")
{
    // Tone for the first half, silence for the second: front buckets sit near the tone
    // amplitude (~0.49 after 16-bit quantization), back buckets near zero.
    const u32 rate = 8000;
    Array<i16> samples = MakeTone(0.5f, rate, 1);
    const usize toneCount = samples.Size();
    for (usize i = 0; i < toneCount; ++i)
    {
        samples.PushBack(0);
    }
    Array<byte> wav;
    REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 1, rate, wav));

    Array<f32> peaks;
    REQUIRE(BuildWaveformPeaks(Span<const byte>(wav.Data(), wav.Size()), 16, peaks));
    REQUIRE(peaks.Size() == 16u);
    for (usize i = 0; i < 7; ++i) // tone half (skip the boundary bucket)
    {
        CHECK(peaks[i] > 0.4f);
        CHECK(peaks[i] <= 1.0f);
    }
    for (usize i = 9; i < 16; ++i) // silent half
    {
        CHECK(peaks[i] < 0.01f);
    }

    // Degenerate inputs refuse cleanly.
    Array<f32> none;
    CHECK_FALSE(BuildWaveformPeaks(Span<const byte>{}, 16, none));
    CHECK_FALSE(BuildWaveformPeaks(Span<const byte>(wav.Data(), wav.Size()), 0, none));
    const byte garbage[8] = {};
    CHECK_FALSE(BuildWaveformPeaks(Span<const byte>(garbage, 8), 16, none));
}

TEST_CASE("audio.engine: distance low-pass glides open -> floor across [min, max] distance")
{
    AudioEngine engine(HeadlessSettings());
    engine.SetListenerTransform(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0},
                                Float3{0, 0, 0});
    RefPtr<AudioClip> clip = MakeToneClip(2.0f);

    AudioPlayParams params;
    params.loop = true;
    params.spatial = true;
    params.minDistance = 2.0f;
    params.maxDistance = 20.0f;
    params.distanceLowpassHz = 4000.0f;
    params.position = Float3{0.0f, 0.0f, -2.0f}; // at minDistance: fully open
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    engine.Update(1.0f / 60.0f);

    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    const f32 openCutoff = status.lowpassCutoffHz;
    CHECK(openCutoff > 15000.0f); // inside minDistance = no audible muffling

    // Far: the cutoff lands on the floor.
    engine.SetVoicePosition(voice, Float3{0.0f, 0.0f, -20.0f}, Float3{});
    engine.Update(1.0f / 60.0f);
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.lowpassCutoffHz == doctest::Approx(4000.0f).epsilon(0.02));

    // Midway: strictly between the endpoints (the glide is monotonic).
    engine.SetVoicePosition(voice, Float3{0.0f, 0.0f, -11.0f}, Float3{});
    engine.Update(1.0f / 60.0f);
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.lowpassCutoffHz > 4100.0f);
    CHECK(status.lowpassCutoffHz < openCutoff - 100.0f);

    // 0 Hz disables the filter entirely - no node, no cutoff reported.
    AudioPlayParams unfiltered = params;
    unfiltered.distanceLowpassHz = 0.0f;
    const VoiceHandle plain = engine.Play(clip, unfiltered);
    REQUIRE(plain.IsValid());
    engine.Update(1.0f / 60.0f);
    REQUIRE(engine.GetVoiceStatus(plain, status));
    CHECK(status.lowpassCutoffHz == 0.0f);
}

TEST_CASE("audio.engine: PlayMusic cross-fades - old voice fades out while the new plays")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> trackA = MakeToneClip(3.0f);
    RefPtr<AudioClip> trackB = MakeToneClip(3.0f);

    const VoiceHandle first = engine.PlayMusic(trackA, 0.2f);
    REQUIRE(first.IsValid());
    CHECK(engine.MusicVoice() == first);
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(first, status));
    CHECK(status.bus == AudioBus::Music);
    CHECK(status.playing);

    // Cross-fade to B: A enters its fade-out, B is the tracked music voice, both alive
    // through the overlap window.
    const VoiceHandle second = engine.PlayMusic(trackB, 0.2f);
    REQUIRE(second.IsValid());
    CHECK_FALSE(second == first);
    CHECK(engine.MusicVoice() == second);
    REQUIRE(engine.GetVoiceStatus(first, status));
    CHECK(status.stopping);
    REQUIRE(engine.GetVoiceStatus(second, status));
    CHECK(status.playing);
    CHECK(engine.ActiveVoiceCount() == 2u);

    // The fade lands: A reaps; B keeps playing.
    for (int i = 0; i < 40; ++i)
    {
        engine.Update(1.0f / 60.0f);
    } // ~0.66 s
    CHECK_FALSE(engine.GetVoiceStatus(first, status));
    REQUIRE(engine.GetVoiceStatus(second, status));
    CHECK(status.playing);

    // StopMusic fades the tracked voice and forgets it.
    engine.StopMusic(0.1f);
    CHECK_FALSE(engine.MusicVoice().IsValid());
    for (int i = 0; i < 30; ++i)
    {
        engine.Update(1.0f / 60.0f);
    }
    CHECK(engine.ActiveVoiceCount() == 0u);
}

TEST_CASE("audio.engine: bus layout applies volumes/mutes and splices effect chains")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    layout.buses[static_cast<usize>(AudioBus::Music)].volume = 0.5f;
    layout.buses[static_cast<usize>(AudioBus::UI)].muted = true;
    AudioBusEffectDesc lowpass;
    lowpass.kind = AudioBusEffectKind::Lowpass;
    lowpass.frequencyHz = 2000.0f;
    AudioBusEffectDesc delay;
    delay.kind = AudioBusEffectKind::Delay;
    delay.delaySeconds = 0.1f;
    delay.delayDecay = 0.4f;
    layout.buses[static_cast<usize>(AudioBus::Effects)].effects.PushBack(lowpass);
    layout.buses[static_cast<usize>(AudioBus::Effects)].effects.PushBack(delay);

    engine.ApplyBusLayout(layout);
    CHECK(engine.BusVolume(AudioBus::Music) == doctest::Approx(0.5f));
    CHECK(engine.BusMuted(AudioBus::UI));
    CHECK(engine.BusEffectCount(AudioBus::Effects) == 2u);
    CHECK(engine.BusEffectCount(AudioBus::Master) == 0u);

    // Voices still route and play through the spliced chain.
    RefPtr<AudioClip> clip = MakeToneClip(0.5f);
    AudioPlayParams params;
    params.loop = true;
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(voice));

    // Re-apply an empty layout: chains tear down cleanly, the voice survives.
    engine.ApplyBusLayout(AudioBusLayout{});
    CHECK(engine.BusEffectCount(AudioBus::Effects) == 0u);
    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(voice));
}

TEST_CASE("audio.engine: VoiceStatus.cursorSeconds is the TRUE voice cursor - it "
          "advances with the mixer and wraps on loop")
{
    AudioEngine engine(HeadlessSettings());
    RefPtr<AudioClip> clip = MakeToneClip(1.0f); // 1 s one-shot
    AudioPlayParams params;
    params.allowDedupe = false;
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());

    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    const f32 start = status.cursorSeconds;
    CHECK(start >= 0.0f);
    CHECK(start < 0.05f);

    // Pump ~0.3 s of mixing: the cursor advances with the DATA, not wall time.
    for (int i = 0; i < 18; ++i)
    {
        engine.Update(1.0f / 60.0f);
    }
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.cursorSeconds > start + 0.2f);
    CHECK(status.cursorSeconds < 0.6f);
    const f32 mid = status.cursorSeconds;

    // A paused voice's cursor holds still.
    engine.SetPaused(voice, true);
    for (int i = 0; i < 12; ++i)
    {
        engine.Update(1.0f / 60.0f);
    }
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.cursorSeconds == doctest::Approx(mid).epsilon(0.02));
    engine.SetPaused(voice, false);

    // Looping wraps: a 0.25 s loop pumped ~0.6 s reads back inside the clip.
    RefPtr<AudioClip> shortClip = MakeToneClip(0.25f, 8000, 1);
    AudioPlayParams loopParams;
    loopParams.loop = true;
    loopParams.allowDedupe = false;
    const VoiceHandle looping = engine.Play(shortClip, loopParams);
    REQUIRE(looping.IsValid());
    for (int i = 0; i < 36; ++i)
    {
        engine.Update(1.0f / 60.0f);
    }
    REQUIRE(engine.GetVoiceStatus(looping, status));
    CHECK(engine.IsPlaying(looping));
    CHECK(status.cursorSeconds >= 0.0f);
    CHECK(status.cursorSeconds < 0.26f); // wrapped, not 0.6
}

TEST_CASE("audio.engine: named custom buses - layout realizes the tree, voices route by "
          "name, volume/mute/effects work like fixed buses")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    AudioNamedBus drums;
    drums.name = String(u8"drums");
    drums.parent = String(u8"Effects");
    drums.settings.volume = 0.5f;
    AudioBusEffectDesc lowpass;
    lowpass.kind = AudioBusEffectKind::Lowpass;
    lowpass.frequencyHz = 1500.0f;
    drums.settings.effects.PushBack(lowpass);
    AudioNamedBus quiet;
    quiet.name = String(u8"quiet");
    quiet.parent = String(u8"drums"); // custom-under-custom nesting
    quiet.settings.muted = true;
    layout.customBuses.PushBack(drums);
    layout.customBuses.PushBack(quiet);
    engine.ApplyBusLayout(layout);

    CHECK(engine.NamedBusCount() == 2u);
    CHECK(engine.HasNamedBus(u8"drums"));
    CHECK(engine.HasNamedBus(u8"quiet"));
    CHECK_FALSE(engine.HasNamedBus(u8"nope"));
    CHECK(engine.NamedBusVolume(u8"drums") == doctest::Approx(0.5f));
    CHECK(engine.NamedBusMuted(u8"quiet"));
    CHECK(engine.NamedBusEffectCount(u8"drums") == 1u);
    CHECK(engine.BusEffectCount(AudioBus::Effects) == 0u); // fixed buses untouched

    // Voices address the custom bus by name; unknown names fall back to the enum bus.
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    AudioPlayParams params;
    params.loop = true;
    params.busName = String(u8"drums");
    const VoiceHandle onDrums = engine.Play(clip, params);
    REQUIRE(onDrums.IsValid());
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(onDrums, status));
    CHECK(status.busName.AsView() == StringView(u8"drums"));

    AudioPlayParams unknown;
    unknown.loop = true;
    unknown.busName = String(u8"missing");
    unknown.allowDedupe = false;
    const VoiceHandle fallback = engine.Play(clip, unknown);
    REQUIRE(fallback.IsValid());
    REQUIRE(engine.GetVoiceStatus(fallback, status));
    CHECK(status.busName.IsEmpty());
    CHECK(status.bus == AudioBus::Effects);

    // Live named-bus tuning mirrors the fixed accessors.
    engine.SetNamedBusVolume(u8"drums", 0.25f);
    CHECK(engine.NamedBusVolume(u8"drums") == doctest::Approx(0.25f));
    engine.SetNamedBusMuted(u8"drums", true);
    CHECK(engine.NamedBusMuted(u8"drums"));
    engine.SetNamedBusMuted(u8"drums", false);
    CHECK(engine.NamedBusVolume(u8"drums") == doctest::Approx(0.25f)); // remembered

    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(onDrums));
}

TEST_CASE("audio.engine: a layout rebuild keeps voices ALIVE - kept buses update in "
          "place, removed buses re-home their voices to the fixed fallback")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    AudioNamedBus drums;
    drums.name = String(u8"drums");
    drums.parent = String(u8"Effects");
    layout.customBuses.PushBack(drums);
    AudioNamedBus voices;
    voices.name = String(u8"voices");
    layout.customBuses.PushBack(voices);
    engine.ApplyBusLayout(layout);

    RefPtr<AudioClip> clipA = MakeToneClip(1.0f);
    RefPtr<AudioClip> clipB = MakeToneClip(1.0f, 4000, 1);
    AudioPlayParams params;
    params.loop = true;
    params.busName = String(u8"drums");
    const VoiceHandle onDrums = engine.Play(clipA, params);
    params.busName = String(u8"voices");
    params.allowDedupe = false;
    const VoiceHandle onVoices = engine.Play(clipB, params);
    REQUIRE(onDrums.IsValid());
    REQUIRE(onVoices.IsValid());

    // Rebuild WITHOUT "voices": drums persists (voice keeps its name), the removed
    // bus's voice survives on its fixed fallback (busName clears).
    AudioBusLayout rebuilt;
    AudioNamedBus drumsKept;
    drumsKept.name = String(u8"drums");
    drumsKept.parent = String(u8"Music"); // re-parent while live
    drumsKept.settings.volume = 0.8f;
    rebuilt.customBuses.PushBack(drumsKept);
    engine.ApplyBusLayout(rebuilt);

    CHECK(engine.NamedBusCount() == 1u);
    CHECK(engine.NamedBusVolume(u8"drums") == doctest::Approx(0.8f));
    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(onDrums));
    CHECK(engine.IsPlaying(onVoices));
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(onDrums, status));
    CHECK(status.busName.AsView() == StringView(u8"drums"));
    REQUIRE(engine.GetVoiceStatus(onVoices, status));
    CHECK(status.busName.IsEmpty()); // re-homed to the fixed bus
    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(onVoices));
}

TEST_CASE("audio.engine: custom-bus degenerates defuse - parent cycles land on Master, "
          "unknown parents warn, duplicates and fixed-name shadows are skipped")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    AudioNamedBus a;
    a.name = String(u8"a");
    a.parent = String(u8"b");
    AudioNamedBus b;
    b.name = String(u8"b");
    b.parent = String(u8"a"); // a <-> b cycle
    AudioNamedBus orphan;
    orphan.name = String(u8"orphan");
    orphan.parent = String(u8"ghost"); // unknown parent -> Master
    AudioNamedBus dupe;
    dupe.name = String(u8"a"); // duplicate -> skipped
    AudioNamedBus shadow;
    shadow.name = String(u8"Effects"); // shadows a fixed bus -> skipped
    layout.customBuses.PushBack(a);
    layout.customBuses.PushBack(b);
    layout.customBuses.PushBack(orphan);
    layout.customBuses.PushBack(dupe);
    layout.customBuses.PushBack(shadow);
    engine.ApplyBusLayout(layout);

    CHECK(engine.NamedBusCount() == 3u); // a, b, orphan
    CHECK(engine.HasNamedBus(u8"a"));
    CHECK(engine.HasNamedBus(u8"b"));
    CHECK(engine.HasNamedBus(u8"orphan"));
    CHECK_FALSE(engine.HasNamedBus(u8"Effects"));

    // The defused graph still mixes: play on every custom bus, pump, all audible.
    RefPtr<AudioClip> clip = MakeToneClip(0.5f);
    AudioPlayParams params;
    params.loop = true;
    params.allowDedupe = false;
    const StringView names[3] = {u8"a", u8"b", u8"orphan"};
    for (StringView name : names)
    {
        params.busName = String(name);
        const VoiceHandle voice = engine.Play(clip, params);
        REQUIRE(voice.IsValid());
    }
    engine.Update(0.1f);
    CHECK(engine.ActiveVoiceCount() == 3u);
}

TEST_CASE("audio.engine: scene-group pause freezes custom-bus voices too (they bypass "
          "the scene child groups)")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    AudioNamedBus drums;
    drums.name = String(u8"drums");
    layout.customBuses.PushBack(drums);
    engine.ApplyBusLayout(layout);

    const u64 sceneGroup = engine.CreateSceneGroup();
    REQUIRE(sceneGroup != 0u);
    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    AudioPlayParams params;
    params.loop = true;
    params.sceneGroup = sceneGroup;
    params.busName = String(u8"drums");
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());

    engine.SetSceneGroupPaused(sceneGroup, true);
    for (int i = 0; i < 5; ++i)
    {
        engine.Update(0.1f);
    }
    CHECK(engine.IsValidHandle(voice)); // held, not reaped
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(voice, status));
    CHECK(status.busName.AsView() == StringView(u8"drums"));

    engine.SetSceneGroupPaused(sceneGroup, false);
    engine.Update(0.1f);
    CHECK(engine.IsPlaying(voice));

    engine.DestroySceneGroup(sceneGroup);
    CHECK_FALSE(engine.IsValidHandle(voice));
}

TEST_CASE("audio.cue: weighted resolution - no-repeat, sequential, jitter, degenerate")
{
    RefPtr<AudioClip> a = MakeToneClip(0.1f);
    RefPtr<AudioClip> b = MakeToneClip(0.1f);
    RefPtr<AudioClip> c = MakeToneClip(0.1f);

    SoundCue cue;
    cue.variants.PushBack(SoundCueVariant{a, 1.0f});
    cue.variants.PushBack(SoundCueVariant{RefPtr<AudioClip>{}, 5.0f}); // empty: skipped
    cue.variants.PushBack(SoundCueVariant{b, 0.0f});                   // zero weight: skipped
    cue.variants.PushBack(SoundCueVariant{c, 3.0f});
    cue.pitchMin = 0.9f;
    cue.pitchMax = 1.1f;

    Random rng(42);
    u32 cursor = 0;

    // RandomNoRepeat: only slots 0 and 3 are eligible; consecutive picks always differ
    // (with two choices, no-repeat must alternate), jitter stays in range.
    i32 last = -1;
    for (int i = 0; i < 50; ++i)
    {
        const SoundCuePick pick = ResolveSoundCue(cue, rng, last, cursor);
        REQUIRE((pick.variantIndex == 0 || pick.variantIndex == 3));
        if (last >= 0)
        {
            CHECK(pick.variantIndex != last);
        }
        CHECK(pick.pitch >= 0.9f);
        CHECK(pick.pitch <= 1.1f);
        CHECK(pick.volume == doctest::Approx(1.0f));
        last = pick.variantIndex;
    }

    // Random (repeats allowed): weights bias toward slot 3 (3:1).
    cue.mode = SoundCueMode::Random;
    int hits0 = 0, hits3 = 0;
    for (int i = 0; i < 400; ++i)
    {
        const SoundCuePick pick = ResolveSoundCue(cue, rng, -1, cursor);
        if (pick.variantIndex == 0)
        {
            ++hits0;
        }
        if (pick.variantIndex == 3)
        {
            ++hits3;
        }
    }
    CHECK(hits0 + hits3 == 400);
    CHECK(hits3 > hits0 * 2); // ~3x expected; 2x is a generous statistical floor

    // Sequential: round-robin over the eligible set in slot order.
    cue.mode = SoundCueMode::Sequential;
    cursor = 0;
    CHECK(ResolveSoundCue(cue, rng, -1, cursor).variantIndex == 0);
    CHECK(ResolveSoundCue(cue, rng, -1, cursor).variantIndex == 3);
    CHECK(ResolveSoundCue(cue, rng, -1, cursor).variantIndex == 0);

    // No playable variant -> -1.
    SoundCue empty;
    empty.variants.PushBack(SoundCueVariant{RefPtr<AudioClip>{}, 1.0f});
    CHECK(ResolveSoundCue(empty, rng, -1, cursor).variantIndex == -1);
}

TEST_CASE("audio.reverb: freeverb - dry passthrough at wet 0, a tail past the impulse, "
          "and damping shortens it")
{
    FreeverbState reverb;
    reverb.Initialize(44100);
    REQUIRE(reverb.IsInitialized());

    // wet 0: byte-exact passthrough.
    AudioReverbParams params;
    params.wet = 0.0f;
    reverb.SetParams(params);
    f32 impulse[512 * 2] = {};
    impulse[0] = 1.0f;
    impulse[1] = 1.0f;
    f32 output[512 * 2] = {};
    reverb.ProcessStereo(impulse, output, 512);
    CHECK(output[0] == doctest::Approx(1.0f));
    f32 tail = 0.0f;
    for (usize i = 2; i < 512 * 2; ++i)
    {
        tail += output[i] * output[i];
    }
    CHECK(tail == doctest::Approx(0.0f));

    // wet > 0: energy exists WELL past the impulse (the reverb tail), for seconds.
    FreeverbState wetReverb;
    wetReverb.Initialize(44100);
    params.wet = 0.8f;
    params.roomSize = 0.8f;
    params.damping = 0.1f;
    wetReverb.SetParams(params);
    f32 silent[512 * 2] = {};
    wetReverb.ProcessStereo(impulse, output, 512);
    f32 longTail = 0.0f;
    for (int block = 0; block < 40; ++block) // ~0.46 s after the impulse
    {
        wetReverb.ProcessStereo(silent, output, 512);
        if (block > 20)
        {
            for (usize i = 0; i < 512 * 2; ++i)
            {
                longTail += output[i] * output[i];
            }
        }
    }
    CHECK(longTail > 1.0e-6f);

    // Heavier damping + smaller room: the same late window carries LESS energy.
    FreeverbState dampedReverb;
    dampedReverb.Initialize(44100);
    params.roomSize = 0.2f;
    params.damping = 0.9f;
    dampedReverb.SetParams(params);
    dampedReverb.ProcessStereo(impulse, output, 512);
    f32 dampedTail = 0.0f;
    for (int block = 0; block < 40; ++block)
    {
        dampedReverb.ProcessStereo(silent, output, 512);
        if (block > 20)
        {
            for (usize i = 0; i < 512 * 2; ++i)
            {
                dampedTail += output[i] * output[i];
            }
        }
    }
    CHECK(dampedTail < longTail * 0.5f);
}

TEST_CASE("audio.reverb: wet-only send mode - dry pinned to 0 passes NO dry signal but "
          "still rings a tail")
{
    FreeverbState send;
    send.Initialize(44100);
    AudioReverbParams params;
    params.wet = 1.0f;
    params.dry = 0.0f;
    params.roomSize = 0.8f;
    params.damping = 0.1f;
    send.SetParams(params);
    CHECK(send.Dry() == doctest::Approx(0.0f));

    f32 impulse[512 * 2] = {};
    impulse[0] = 1.0f;
    impulse[1] = 1.0f;
    f32 output[512 * 2] = {};
    send.ProcessStereo(impulse, output, 512);
    CHECK(output[0] == doctest::Approx(0.0f)); // no dry passthrough
    CHECK(output[1] == doctest::Approx(0.0f));

    f32 silent[512 * 2] = {};
    f32 tail = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        send.ProcessStereo(silent, output, 512);
        for (usize i = 0; i < 512 * 2; ++i)
        {
            tail += output[i] * output[i];
        }
    }
    CHECK(tail > 1.0e-6f); // the send tail rings

    // Default dry (< 0) keeps tracking 1 - wet (the classic insert mix).
    AudioReverbParams insert;
    insert.wet = 0.25f;
    send.SetParams(insert);
    CHECK(send.Dry() == doctest::Approx(0.75f));
}

TEST_CASE("audio.engine: per-voice reverb sends - splitter splices per voice, live "
          "scaling works, steal hands the splitter to the dying list")
{
    AudioEngine engine(HeadlessSettings(/*voiceCount=*/2, /*streamVoiceCount=*/0));
    const u64 sceneGroup = engine.CreateSceneGroup();
    REQUIRE(sceneGroup != 0u);

    RefPtr<AudioClip> clip = MakeToneClip(1.0f);
    AudioPlayParams wet;
    wet.loop = true;
    wet.sceneGroup = sceneGroup;
    wet.reverbSend = 0.5f;
    wet.spatial = true;
    wet.distanceLowpassHz = 4000.0f; // send + low-pass coexist in one chain
    const VoiceHandle sending = engine.Play(clip, wet);
    REQUIRE(sending.IsValid());
    VoiceStatus status;
    REQUIRE(engine.GetVoiceStatus(sending, status));
    CHECK(status.reverbSend == doctest::Approx(0.5f));

    AudioPlayParams dry;
    dry.loop = true;
    dry.sceneGroup = sceneGroup;
    dry.allowDedupe = false;
    RefPtr<AudioClip> other = MakeToneClip(1.0f, 4000, 1);
    const VoiceHandle drier = engine.Play(other, dry);
    REQUIRE(drier.IsValid());
    REQUIRE(engine.GetVoiceStatus(drier, status));
    CHECK(status.reverbSend == doctest::Approx(0.0f)); // no splitter, no send

    // The graph mixes cleanly with the send spliced in.
    for (int i = 0; i < 10; ++i)
    {
        engine.Update(1.0f / 60.0f);
    }
    CHECK(engine.IsPlaying(sending));

    // Live scaling lands (and clamps); dry voices ignore it.
    engine.SetVoiceReverbSend(sending, 2.0f);
    REQUIRE(engine.GetVoiceStatus(sending, status));
    CHECK(status.reverbSend == doctest::Approx(1.0f));
    engine.SetVoiceReverbSend(drier, 0.7f);
    REQUIRE(engine.GetVoiceStatus(drier, status));
    CHECK(status.reverbSend == doctest::Approx(0.0f));

    // Zone params retune the send reverb without touching the voice's send level.
    AudioReverbParams zone;
    zone.roomSize = 0.9f;
    zone.damping = 0.2f;
    zone.wet = 0.6f;
    engine.SetSceneReverb(sceneGroup, zone);
    REQUIRE(engine.GetVoiceStatus(sending, status));
    CHECK(status.reverbSend == doctest::Approx(1.0f));

    // Steal the SEND voice (pool of 2, both taken, higher priority incoming): its
    // splitter+sound hand over to the dying list and reap cleanly.
    AudioPlayParams high;
    high.loop = true;
    high.priority = 200;
    high.allowDedupe = false;
    RefPtr<AudioClip> third = MakeToneClip(1.0f, 16000, 1);
    const VoiceHandle stealer = engine.Play(third, high);
    REQUIRE(stealer.IsValid());
    CHECK(engine.DyingVoiceCount() == 1u);
    for (int i = 0; i < 10; ++i)
    {
        engine.Update(0.05f);
    }
    CHECK(engine.DyingVoiceCount() == 0u);

    // Scene teardown destroys the send reverb with the group.
    engine.DestroySceneGroup(sceneGroup);
    engine.Update(1.0f / 60.0f);

    // Sends OUTSIDE a scene group are inert (no send reverb to feed).
    AudioPlayParams global;
    global.loop = true;
    global.reverbSend = 0.8f;
    global.allowDedupe = false;
    const VoiceHandle globalVoice = engine.Play(clip, global);
    REQUIRE(globalVoice.IsValid());
    REQUIRE(engine.GetVoiceStatus(globalVoice, status));
    CHECK(status.reverbSend == doctest::Approx(0.0f));
}

TEST_CASE("audio.engine: a Reverb bus effect splices and the headless mixer survives it")
{
    AudioEngine engine(HeadlessSettings());
    AudioBusLayout layout;
    AudioBusEffectDesc reverb;
    reverb.kind = AudioBusEffectKind::Reverb;
    reverb.roomSize = 0.7f;
    reverb.damping = 0.3f;
    reverb.wetLevel = 0.5f;
    layout.buses[static_cast<usize>(AudioBus::Effects)].effects.PushBack(reverb);
    engine.ApplyBusLayout(layout);
    CHECK(engine.BusEffectCount(AudioBus::Effects) == 1u);

    RefPtr<AudioClip> clip = MakeToneClip(0.3f);
    AudioPlayParams params;
    params.loop = true;
    const VoiceHandle voice = engine.Play(clip, params);
    REQUIRE(voice.IsValid());
    for (int i = 0; i < 30; ++i)
    {
        engine.Update(1.0f / 60.0f);
    } // pumps through the tail
    CHECK(engine.IsPlaying(voice));
    engine.ApplyBusLayout(AudioBusLayout{}); // teardown mid-play stays clean
    engine.Update(1.0f / 60.0f);
    CHECK(engine.IsPlaying(voice));
}
