// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Audio - :engine partition.
//
// AudioEngine: the engine's wrapper over miniaudio. miniaudio is the only backend, with no
// abstraction layer, but ma_* types never appear in this interface unit: all miniaudio contact
// lives in AudioEngineImpl.cpp, a module implementation unit, both for API hygiene and because
// GCC's module serializer must not digest the 4 MB header in an interface unit's global fragment.
//
// What it provides:
//   * pitch resamples (ma_sound_set_pitch) and doppler shifts (velocities feed ma_spatializer);
//   * streamed clips route through the same bus graph as everything else;
//   * a fixed voice pool with {slot, generation} handles, priority stealing and recent-play
//     dedupe instead of unbounded voices;
//   * stop and pause always fade (about 10 ms, so nothing clicks); voices are reaped on the game
//     thread after the fade completes.
//
// Threading: every engine API call happens on the main thread; miniaudio owns the device and mix
// thread internally, and its control surface is safe for this pattern. Headless mode runs with no
// device at all (Update() pumps the mixer manually), so tests, the cooker and machines without
// audio hardware run the full state machine deterministically.

module;
#include "Core/Prelude.h"

export module foundation.audio:engine;

import foundation.core;
import foundation.vfs;
import :clip;
import :reverb; // AudioReverbParams (scene reverb)

using namespace foundation::core;

export namespace foundation::audio
{
    // The fixed buses: Effects, Music and UI under Master. Bus layouts (AudioBusLayout) tune
    // these and add named buses; this enum is the fixed set. At runtime each is one ma_sound_group.
    enum class AudioBus : u8
    {
        Master = 0,
        Effects, // one-shots / world sounds ("SFX")
        Music,   // streamed music, routed through the graph like everything else
        UI,      // interface sounds
        Count,
    };

    enum class AudioAttenuationModel : u8
    {
        None = 0, // no distance attenuation (still panned/dopplered)
        Inverse,
        Linear,
        Exponential,
    };

    // Generation-checked voice handle over the fixed pool.
    struct VoiceHandle
    {
        u32 slot = 0xFFFFFFFFu;
        u32 generation = 0;
        [[nodiscard]] bool IsValid() const noexcept { return slot != 0xFFFFFFFFu; }
        friend bool operator==(VoiceHandle a, VoiceHandle b) noexcept
        {
            return a.slot == b.slot && a.generation == b.generation;
        }
    };

    struct AudioPlayParams
    {
        AudioBus bus = AudioBus::Effects;
        // Named-bus addressing: when non-empty and the applied layout has a custom bus
        // of this name, the voice routes there instead of `bus`. Unknown names fall
        // back to `bus` (warned once per engine); content never faults playback.
        String busName;
        f32 volume = 1.0f;  // multiplied with the clip's authored gain
        f32 pitch = 1.0f;   // playback rate (resamples)
        f32 pan = 0.0f;     // -1 left .. +1 right (non-spatial voices)
        bool loop = false;  // OR-ed with the clip's loop intent
        u8 priority = 128;  // higher wins pool contention (priority stealing)
        u64 sceneGroup = 0; // per-scene pause/teardown group id (0 = global)
        // The run a scene-less voice belongs to (a game script's one-shot): it routes through the
        // run's groups, so the run's stop, pause, mute and bus gains reach it. Ignored when
        // sceneGroup is set (the scene group knows its run). 0 = outside every run.
        u64 runGroup = 0;
        bool startPaused = false;
        // Recent-play merging (shotgun pellets, particle bursts). Persistent sources
        // (scene components) must opt out: distinct authored sources playing the same
        // clip in the same instant are distinct voices at distinct positions, and merging
        // them would silence all but one emitter.
        bool allowDedupe = true;

        // Per-voice reverb send (0..1): a splitter after the voice's chain feeds the
        // scene's send reverb (wet-only Freeverb; zones drive its room character) in
        // parallel with the dry path, scaled by this. 0 = no splitter, no send.
        // Requires a scene group (the send reverb is per-scene).
        f32 reverbSend = 0.0f;

        // 3D (spatial = true):
        bool spatial = false;
        // Distance low-pass (muffling with distance): the cutoff glides from fully open at
        // minDistance down to this frequency at maxDistance. 0 disables the filter (no node
        // in the chain).
        f32 distanceLowpassHz = 4000.0f;
        Float3 position{0.0f, 0.0f, 0.0f};
        Float3 velocity{0.0f, 0.0f, 0.0f}; // feeds doppler (per-frame transform deltas)
        f32 minDistance = 1.0f;
        f32 maxDistance = 100.0f;
        AudioAttenuationModel attenuationModel = AudioAttenuationModel::Inverse;
        f32 rolloff = 1.0f;
        f32 dopplerFactor = 1.0f;
        f32 coneInnerAngleDegrees = 360.0f;
        f32 coneOuterAngleDegrees = 360.0f;
        f32 coneOuterGain = 0.0f;
    };

    // ---- bus layout data ----

    enum class AudioBusEffectKind : u8
    {
        None = 0,
        Lowpass,  // frequencyHz = cutoff
        Highpass, // frequencyHz = cutoff
        Delay,    // delaySeconds and delayDecay (feedback 0..1)
        Reverb,   // roomSize, damping, wetLevel (Freeverb tail)
    };

    struct AudioBusEffectDesc
    {
        AudioBusEffectKind kind = AudioBusEffectKind::None;
        f32 frequencyHz = 1000.0f;
        f32 delaySeconds = 0.25f;
        f32 delayDecay = 0.3f;
        f32 roomSize = 0.6f;
        f32 damping = 0.4f;
        f32 wetLevel = 0.4f;
    };

    struct AudioBusSettings
    {
        f32 volume = 1.0f;
        bool muted = false;
        Array<AudioBusEffectDesc> effects; // applied in order; None entries skip
    };

    // A named custom bus, added beside the fixed four: realized as an extra
    // ma_sound_group parented per the layout. The four AudioBus enum buses stay the
    // well-known addressing model; custom buses are addressed by name
    // (AudioPlayParams::busName, AudioSourceComponent::busName).
    struct AudioNamedBus
    {
        String name;   // unique per layout (case-sensitive); empty = ignored
        String parent; // a fixed bus name ("Master"/"Effects"/"Music"/"UI",
                       // case-insensitive) or another custom bus's name; empty =
                       // Master. Cycles are rejected at cook and defused at apply.
        AudioBusSettings settings;
    };

    struct AudioBusLayout
    {
        AudioBusSettings buses[static_cast<usize>(AudioBus::Count)];
        Array<AudioNamedBus> customBuses; // additive named tree (may be empty)
    };

    /// Fixed-bus lookup by name, ASCII case-insensitive ("effects" matches "Effects"),
    /// shared by the layout apply, the cook validator and the facade's string
    /// addressing. False when the name is not one of the four fixed buses.
    [[nodiscard]] inline bool AudioBusFromName(StringView name, AudioBus& out)
    {
        auto equals = [](StringView a, const utf8char* b)
        {
            usize i = 0;
            for (; i < a.Size(); ++i)
            {
                utf8char c = a[i];
                if (c >= u8'A' && c <= u8'Z')
                {
                    c = static_cast<utf8char>(c + 32);
                }
                if (b[i] == 0 || c != b[i])
                {
                    return false;
                }
            }
            return b[i] == 0;
        };
        if (equals(name, u8"master"))
        {
            out = AudioBus::Master;
            return true;
        }
        if (equals(name, u8"effects"))
        {
            out = AudioBus::Effects;
            return true;
        }
        if (equals(name, u8"music"))
        {
            out = AudioBus::Music;
            return true;
        }
        if (equals(name, u8"ui"))
        {
            out = AudioBus::UI;
            return true;
        }
        return false;
    }

    struct VoiceStatus
    {
        bool active = false;  // slot owned by this generation
        bool playing = false; // audible and advancing (not paused, not fading out)
        bool paused = false;
        bool stopping = false; // fade-to-stop in flight; reaped when the fade lands
        bool spatial = false;
        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        AudioBus bus = AudioBus::Effects;
        // The custom bus the voice routes through (empty = the fixed `bus`). Cleared
        // when a layout rebuild removes the bus and the voice falls back to `bus`.
        String busName;
        u8 priority = 0;
        Float3 position{0.0f, 0.0f, 0.0f};
        // Distance low-pass state: the cutoff currently applied (0 = no filter node).
        f32 lowpassCutoffHz = 0.0f;
        // The true playback cursor (seconds into the clip's data, read from the voice
        // itself rather than approximated from elapsed time): honors pitch, pauses and
        // loop wraps.
        f32 cursorSeconds = 0.0f;
        // Per-voice reverb send level (0 = no splitter in the chain).
        f32 reverbSend = 0.0f;
    };

    struct AudioEngineSettings
    {
        /// True = never touch a device: the engine mixes on demand (Update() pumps it).
        /// False = open the default playback device; on failure the engine logs a warning
        /// and falls back to headless mixing, and handles stay valid either way.
        bool headless = false;
        u32 voiceCount = 64;          // fixed in-memory voice pool
        u32 streamVoiceCount = 8;     // fixed streamed-voice pool (music etc.)
        u32 sampleRate = 48000;       // headless mixing rate (a real device uses its own)
        u32 listenerCount = 1;        // spatial listeners (1..4; split-screen); voices
                                      // attach to the closest listener
        f32 stopFadeSeconds = 0.010f; // the fade every stop and pause takes (no click)
        // Faded steal: a stolen voice's ma_sound moves to a bounded "dying" side list
        // and fades out over this window while the newcomer starts at once, so nothing
        // clicks. The mixer briefly carries pool and dying voices; the addressable pool
        // never exceeds voiceCount (see ActiveVoiceCount, DyingVoiceCount).
        f32 stealFadeSeconds = 0.030f;
        u32 dyingVoiceCapacity = 8;             // 0 = immediate cut; when full, the oldest is cut
        f32 dedupeWindowSeconds = 1.0f / 30.0f; // recent-play merge window
        /// Optional mount for path-addressed streaming (clip stream sources don't need it).
        foundation::vfs::IFileSystem* fileSystem = nullptr;
    };

    class AudioEngine
    {
    public:
        // The allocator (required; the owner decides) backs voices, buses,
        // effect nodes, scene groups, and the miniaudio VFS bridge.
        explicit AudioEngine(core::IAllocator& allocator,
                             const AudioEngineSettings& settings = {});
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        /// True when mixing without a device (explicit headless, or the device-init fallback).
        [[nodiscard]] bool IsHeadless() const noexcept;

        /// Main-thread tick: reaps voices whose fade/stream finished, advances the dedupe
        /// clock, and (headless) pumps `deltaTime` worth of frames through the mixer.
        void Update(f32 deltaTime);

        // ---- voices ----
        /// Starts a voice (invalid handle when the clip is empty or the pool rejects it:
        /// full of higher-priority voices). A same-clip play within the dedupe window
        /// returns the existing voice's handle instead of stacking a duplicate.
        [[nodiscard]] VoiceHandle Play(const RefPtr<AudioClip>& clip,
                                       const AudioPlayParams& params = {});
        /// Fade out (~stopFadeSeconds) then reap. Safe on stale handles.
        void Stop(VoiceHandle handle);
        /// The same, fading out over `fadeSeconds` (at least the click-free stopFadeSeconds).
        void Stop(VoiceHandle handle, f32 fadeSeconds);
        void StopAll();
        /// Pause fades out but keeps the cursor; resume fades back in.
        void SetPaused(VoiceHandle handle, bool paused);
        [[nodiscard]] bool IsPlaying(VoiceHandle handle) const; // active && !paused && !stopping
        [[nodiscard]] bool IsValidHandle(VoiceHandle handle) const;
        [[nodiscard]] bool GetVoiceStatus(VoiceHandle handle, VoiceStatus& out) const;

        /// Volume and pitch (the playback rate: 1.2 plays 20% faster and higher) at once, or eased
        /// there over `seconds` (Update advances it; a later set retargets from where it is).
        void SetVoiceVolume(VoiceHandle handle, f32 volume, f32 seconds = 0.0f);
        void SetVoicePitch(VoiceHandle handle, f32 pitch, f32 seconds = 0.0f);
        void SetVoicePan(VoiceHandle handle, f32 pan);
        void SetVoiceLooping(VoiceHandle handle, bool loop);
        /// Per-frame 3D sync: position and velocity (velocity drives doppler).
        void SetVoicePosition(VoiceHandle handle, Float3 position, Float3 velocity);
        /// Live send scaling (splitter output-bus volume). No-op on voices played
        /// with reverbSend 0: the splitter is only spliced in at Play.
        void SetVoiceReverbSend(VoiceHandle handle, f32 send);

        /// Addressable voices: slots owned by a live generation. A stolen voice leaves
        /// this count at the instant of the steal (its handle dies) even though its
        /// audio tail keeps mixing briefly (see DyingVoiceCount()).
        [[nodiscard]] usize ActiveVoiceCount() const;
        /// Stolen voices still fading out on the dying side list (steal declick). They
        /// are unaddressable and capacity-bounded (settings.dyingVoiceCapacity).
        [[nodiscard]] usize DyingVoiceCount() const;

        // ---- listener (one default listener; indexed multi-listener below) ----
        void SetListenerTransform(Float3 position, Float3 forward, Float3 up, Float3 velocity);
        /// Multi-listener (split-screen): move listener `index` (< ListenerCount()).
        /// Spatial voices attenuate and pan against the closest enabled listener.
        void SetListenerTransformIndexed(u32 index, Float3 position, Float3 forward, Float3 up,
                                         Float3 velocity);
        void SetListenerEnabled(u32 index, bool enabled);
        [[nodiscard]] u32 ListenerCount() const;

        // ---- master ----
        // Engine-level output gain, above all buses (ma_engine endpoint). The editor's knob for
        // auditioning with game audio at a comfortable level; games use bus volumes instead.
        // Clamped at >= 0; a fresh engine is 1.0.
        void SetMasterVolume(f32 volume);
        [[nodiscard]] f32 MasterVolume() const;

        // ---- buses ----
        void SetBusVolume(AudioBus bus, f32 volume);
        [[nodiscard]] f32 BusVolume(AudioBus bus) const;
        void SetBusMuted(AudioBus bus, bool muted);
        [[nodiscard]] bool BusMuted(AudioBus bus) const;

        // ---- bus layout: per-bus tuning and effect chains as data ----
        // The four fixed buses stay (the AudioBus enum is the addressing model across
        // components and serialization) and everything else is data: per-bus volume,
        // mute, and an ordered effect chain spliced between the bus group and its parent.
        void ApplyBusLayout(const AudioBusLayout& layout);
        /// Live effect-node count on a bus (tests/diagnostics).
        [[nodiscard]] u32 BusEffectCount(AudioBus bus) const;

        // ---- named custom buses (additive over the fixed four) ----
        // Realized by ApplyBusLayout from AudioBusLayout::customBuses. Re-applying a
        // layout reconciles by name: kept buses update in place (their voices keep
        // playing), removed buses re-attach their live voices to the voice's fixed
        // fallback bus (the voices survive the rebuild), new buses splice in. Volume,
        // mute and effect chains behave as on the fixed buses.
        [[nodiscard]] bool HasNamedBus(StringView name) const;
        [[nodiscard]] u32 NamedBusCount() const;
        void SetNamedBusVolume(StringView name, f32 volume);
        [[nodiscard]] f32 NamedBusVolume(StringView name) const; // 0 when unknown
        void SetNamedBusMuted(StringView name, bool muted);
        [[nodiscard]] bool NamedBusMuted(StringView name) const;
        [[nodiscard]] u32 NamedBusEffectCount(StringView name) const;

        // ---- music: scene-less helpers on the Music bus with cross-fade ----
        // Music routes through the same graph as everything else; it carries no scene
        // group, so it survives scene swaps. PlayMusic fades the previous music voice out and the
        // new one in over `crossFadeSeconds`. Each run has its own music slot (run 0 = the
        // one outside every run), so two runs never cross-fade each other's music.
        VoiceHandle PlayMusic(const RefPtr<AudioClip>& clip, f32 crossFadeSeconds = 1.0f,
                              f32 volume = 1.0f, u64 runGroup = 0);
        void StopMusic(f32 fadeSeconds = 1.0f, u64 runGroup = 0);
        [[nodiscard]] VoiceHandle MusicVoice(u64 runGroup = 0) const;

        // ---- run groups: everything one running game plays (a GameInstance: the player's, each
        // play-in-editor Game tab's), one level above its scenes' groups. A run has its own
        // child group under each bus, carrying the run's own bus gains (a game's volume sliders
        // never touch the editor's or another run's buses), its music slot, and stop, pause,
        // mute and volume for the whole run. ----
        [[nodiscard]] u64 CreateRunGroup();
        /// Immediately stops and frees every voice of the run and its scene groups, then drops it.
        void DestroyRunGroup(u64 runGroup);
        /// Fade-stops every voice of the run, its scenes' and its music included.
        void StopRunGroup(u64 runGroup, f32 fadeSeconds = 0.1f);
        /// Halts the run in place (its voices freeze with their cursors); resume fades back in.
        void SetRunGroupPaused(u64 runGroup, bool paused);
        [[nodiscard]] bool IsRunGroupPaused(u64 runGroup) const;
        /// Silences the run while its voices keep advancing (an unfocused Game tab); muting and
        /// unmuting ramp over a short cross-fade, advanced by Update.
        void SetRunGroupMuted(u64 runGroup, bool muted);
        [[nodiscard]] bool IsRunGroupMuted(u64 runGroup) const;
        void SetRunGroupVolume(u64 runGroup, f32 volume);
        /// The run's own gain for a bus (its Master gain scales every bus of the run).
        void SetRunBusVolume(u64 runGroup, AudioBus bus, f32 volume);
        [[nodiscard]] f32 RunBusVolume(u64 runGroup, AudioBus bus) const; // 1 when unknown
        void SetRunBusMuted(u64 runGroup, AudioBus bus, bool muted);
        [[nodiscard]] bool RunBusMuted(u64 runGroup, AudioBus bus) const;
        /// The run's own gain for a layout's custom bus, by name: a run has a child group under
        /// each custom bus it plays on, as under the fixed buses, carrying this (times its gains
        /// for the bus's ancestors); other runs and the bus itself are untouched. Kept across a
        /// layout rebuild; a name the layout lacks reads 1 and applies once the bus exists.
        void SetRunNamedBusVolume(u64 runGroup, StringView name, f32 volume);
        [[nodiscard]] f32 RunNamedBusVolume(u64 runGroup, StringView name) const;
        void SetRunNamedBusMuted(u64 runGroup, StringView name, bool muted);
        [[nodiscard]] bool RunNamedBusMuted(u64 runGroup, StringView name) const;

        // ---- per-scene groups: a child group per scene under each bus, so scene pause and
        // stop-all fall out of the graph. A scene of a run nests under the run's groups. ----
        [[nodiscard]] u64 CreateSceneGroup(u64 runGroup = 0);
        /// Immediately stops and frees every voice in the group, then drops the group.
        void DestroySceneGroup(u64 sceneGroup);
        /// Fades the group's output and halts its voices in place; resume fades back in.
        void SetSceneGroupPaused(u64 sceneGroup, bool paused);
        [[nodiscard]] bool IsSceneGroupPaused(u64 sceneGroup) const;
        /// Fade-stops every voice in the group (they reap as fades land).
        void StopSceneGroup(u64 sceneGroup);

        // ---- per-scene reverb (zones): a Freeverb node on the scene's Effects
        // child group, wet driven by listener zone occupancy. wet 0 = bypass (the node
        // stays spliced once created; params update live). The same params' roomSize/
        // damping also retune the scene's send reverb (per-voice reverbSend), which is
        // wet-only and fed by voice splitters regardless of zone occupancy. ----
        void SetSceneReverb(u64 sceneGroup, const AudioReverbParams& params);
        [[nodiscard]] f32 SceneReverbWet(u64 sceneGroup) const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    /// Decodes `encoded` container bytes (wav, flac, mp3, vorbis) and reduces them to
    /// `buckets` per-bucket peak magnitudes in [0, 1] (max |sample| across channels),
    /// for the editor's waveform thumbnail. Needs no engine (a pure decode; works headless).
    /// False on decode failure, empty input, or zero buckets.
    [[nodiscard]] bool BuildWaveformPeaks(Span<const byte> encoded, u32 buckets,
                                          Array<f32>& outPeaks);
}
