// Draconic::Audio - :engine partition.
//
// AudioEngine: the engine wrapper over miniaudio (docs/design/audio.md §3). miniaudio is
// the COMMITTED backend - no abstraction layer - but ma_* types never appear here AT ALL:
// this interface unit is miniaudio-free (all miniaudio contact lives in EngineImpl.cpp, a
// module IMPLEMENTATION unit) both for API hygiene and because GCC's C++20-modules
// serializer must not digest the 4 MB header inside an interface unit's global fragment.
//
// What it fixes over the Sedulous audio stack it replaces (§2):
//   * pitch actually resamples (ma_sound_set_pitch), doppler actually shifts
//     (velocities feed ma_spatializer), instead of being stored-and-ignored;
//   * streamed clips route through the SAME bus graph as everything else;
//   * a FIXED voice pool with {slot, generation} handles (the one Sedulous pattern kept),
//     Traktor-style priority stealing + recent-play dedupe instead of unbounded voices;
//   * stop/pause always FADE (~10 ms, the Godot no-click rule); voices reap on the game
//     thread after the fade completes.
//
// Threading: all engine API calls happen on the MAIN thread; miniaudio owns the device/mix
// thread internally and its control surface is safe for this pattern. Headless mode runs
// with NO device at all - Update() pumps the mixer manually - so tests, the cooker, and
// machines without audio hardware run the full state machine deterministically.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.audio:engine;

import draconic.foundation;
import draconic.vfs;
import :clip;
import :reverb; // AudioReverbParams (the scene-reverb seam)

using namespace draconic::foundation;

export namespace draconic::audio
{
    // The default bus layout: Master <- { Effects, Music, UI } (BusLayout-as-data is P2;
    // the enum IS the P1 layout). Runtime mapping: one ma_sound_group per bus.
    enum class AudioBus : u8
    {
        Master = 0,
        Effects, // one-shots / world sounds ("SFX")
        Music,   // streamed music - routed through the graph like everything else
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

    // Generation-checked voice handle over the fixed pool (Sedulous pattern, kept).
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
        // Named-bus addressing: when non-empty AND the applied layout has a custom bus
        // of this name, the voice routes there instead of `bus`. Unknown names fall
        // back to `bus` (warned once per engine) - content never faults playback.
        String busName;
        f32 volume = 1.0f;  // multiplied with the clip's authored gain
        f32 pitch = 1.0f;   // real resampling (not stored-and-ignored)
        f32 pan = 0.0f;     // -1 left .. +1 right (non-spatial voices)
        bool loop = false;  // OR-ed with the clip's loop intent
        u8 priority = 128;  // higher wins pool contention (Traktor stealing)
        u64 sceneGroup = 0; // per-scene pause/teardown group id (0 = global)
        bool startPaused = false;
        // Recent-play merging (shotgun pellets / particle bursts). PERSISTENT sources
        // (scene components) must opt OUT: distinct authored sources playing the same
        // clip in the same instant are distinct voices at distinct positions, never a
        // stack - merging them silently collapsed all-but-one emitter.
        bool allowDedupe = true;

        // Per-voice reverb send (0..1): a splitter after the voice's chain feeds the
        // scene's SEND reverb (wet-only Freeverb; zones drive its room character) in
        // parallel with the dry path, scaled by this. 0 = no splitter, no send.
        // Requires a scene group (the send reverb is per-scene).
        f32 reverbSend = 0.0f;

        // 3D (spatial = true):
        bool spatial = false;
        // Distance low-pass (the muffling-with-distance Godot/Traktor ship and Sedulous
        // left dead): the cutoff glides from fully open at minDistance down to THIS
        // frequency at maxDistance. 0 disables the filter (no node in the chain).
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

    // Introspection for tests/tools (the voice state machine is observable).
    // ---- bus layout data (P2) ----

    enum class AudioBusEffectKind : u8
    {
        None = 0,
        Lowpass,  // frequencyHz = cutoff
        Highpass, // frequencyHz = cutoff
        Delay,    // delaySeconds + delayDecay (feedback 0..1)
        Reverb,   // roomSize + damping + wetLevel (Freeverb tail)
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

    // A named CUSTOM bus (the additive topology freedom over the fixed four): realized
    // as an extra ma_sound_group parented per the layout. The four AudioBus enum buses
    // stay the well-known addressing model; custom buses are addressed BY NAME
    // (AudioPlayParams::busName / AudioSourceComponent::busName).
    struct AudioNamedBus
    {
        String name;   // unique per layout (case-sensitive); empty = ignored
        String parent; // a fixed bus name ("Master"/"Effects"/"Music"/"UI",
                       // case-insensitive) or another custom bus's name; empty =
                       // Master. Cycles are rejected at cook AND defused at apply.
        AudioBusSettings settings;
    };

    struct AudioBusLayout
    {
        AudioBusSettings buses[static_cast<usize>(AudioBus::Count)];
        Array<AudioNamedBus> customBuses; // additive named tree (may be empty)
    };

    /// Fixed-bus lookup by name, ASCII case-insensitive ("effects" == "Effects").
    /// The shared seam between the layout apply, the cook validator, and the Wren
    /// facade's string addressing. False = not one of the four fixed buses.
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
        // TRUE playback cursor (seconds into the clip's data, from the voice itself -
        // not an elapsed-time approximation): honors pitch, pauses, and loop wraps.
        f32 cursorSeconds = 0.0f;
        // Per-voice reverb send level (0 = no splitter in the chain).
        f32 reverbSend = 0.0f;
    };

    struct AudioEngineSettings
    {
        /// True = never touch a device: the engine mixes on demand (Update() pumps it).
        /// False = open the default playback device; on FAILURE the engine logs a warning
        /// and falls back to headless mixing - handles stay valid either way (Null mode).
        bool headless = false;
        u32 voiceCount = 64;          // fixed in-memory voice pool
        u32 streamVoiceCount = 8;     // fixed streamed-voice pool (music etc.)
        u32 sampleRate = 48000;       // headless mixing rate (a real device uses its own)
        u32 listenerCount = 1;        // spatial listeners (1..4; split-screen); voices
                                      // auto-attach to the CLOSEST listener
        f32 stopFadeSeconds = 0.010f; // the always-fade on stop/pause (Godot rule)
        // Faded steal: a stolen voice's ma_sound moves to a bounded "dying" side list
        // and fades out over THIS window while the newcomer starts at once - no click.
        // The mixer briefly carries pool + dying voices; the ADDRESSABLE pool never
        // exceeds voiceCount (see ActiveVoiceCount/DyingVoiceCount).
        f32 stealFadeSeconds = 0.030f;
        u32 dyingVoiceCapacity = 8;             // 0 = legacy immediate cut; full = oldest hard-cuts
        f32 dedupeWindowSeconds = 1.0f / 30.0f; // recent-play merge window (Traktor)
        /// Optional mount for path-addressed streaming (clip stream sources don't need it).
        draconic::vfs::IFileSystem* fileSystem = nullptr;
    };

    class AudioEngine
    {
    public:
        explicit AudioEngine(const AudioEngineSettings& settings = {});
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        /// True when mixing without a device (explicit headless OR device-init fallback).
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
        void StopAll();
        /// Pause fades out but keeps the cursor; resume fades back in.
        void SetPaused(VoiceHandle handle, bool paused);
        [[nodiscard]] bool IsPlaying(VoiceHandle handle) const; // active && !paused && !stopping
        [[nodiscard]] bool IsValidHandle(VoiceHandle handle) const;
        [[nodiscard]] bool GetVoiceStatus(VoiceHandle handle, VoiceStatus& out) const;

        void SetVoiceVolume(VoiceHandle handle, f32 volume);
        void SetVoicePitch(VoiceHandle handle, f32 pitch);
        void SetVoicePan(VoiceHandle handle, f32 pan);
        void SetVoiceLooping(VoiceHandle handle, bool loop);
        /// Per-frame 3D sync: position + velocity (velocity drives doppler).
        void SetVoicePosition(VoiceHandle handle, Float3 position, Float3 velocity);
        /// Live send scaling (splitter output-bus volume). No-op on voices played
        /// with reverbSend 0 - the splitter only splices at Play.
        void SetVoiceReverbSend(VoiceHandle handle, f32 send);

        /// ADDRESSABLE voices: slots owned by a live generation. A stolen voice leaves
        /// this count at the instant of the steal (its handle dies) even though its
        /// audio tail keeps mixing briefly - see DyingVoiceCount().
        [[nodiscard]] usize ActiveVoiceCount() const;
        /// Stolen voices still fading out on the dying side list (steal declick). They
        /// are unaddressable and capacity-bounded (settings.dyingVoiceCapacity).
        [[nodiscard]] usize DyingVoiceCount() const;

        // ---- listener (one active listener; multi-listener deferred) ----
        void SetListenerTransform(Float3 position, Float3 forward, Float3 up, Float3 velocity);
        /// Multi-listener (P3, split-screen): move listener `index` (< ListenerCount()).
        /// Spatial voices attenuate/pan against the CLOSEST enabled listener.
        void SetListenerTransformIndexed(u32 index, Float3 position, Float3 forward, Float3 up,
                                         Float3 velocity);
        void SetListenerEnabled(u32 index, bool enabled);
        [[nodiscard]] u32 ListenerCount() const;

        // ---- buses ----
        void SetBusVolume(AudioBus bus, f32 volume);
        [[nodiscard]] f32 BusVolume(AudioBus bus) const;
        void SetBusMuted(AudioBus bus, bool muted);
        [[nodiscard]] bool BusMuted(AudioBus bus) const;

        // ---- bus layout (P2): per-bus tuning + effect chains as DATA ----
        // v1 keeps the FIXED four-bus topology (the AudioBus enum is the addressing
        // model across components/serialization; free-form named trees are a later
        // migration) and makes everything ELSE data: per-bus volume, mute, and an
        // ordered effect chain spliced between the bus group and its parent.
        void ApplyBusLayout(const AudioBusLayout& layout);
        /// Live effect-node count on a bus (tests/diagnostics).
        [[nodiscard]] u32 BusEffectCount(AudioBus bus) const;

        // ---- named custom buses (additive over the fixed four) ----
        // Realized by ApplyBusLayout from AudioBusLayout::customBuses. Re-applying a
        // layout reconciles BY NAME: kept buses update in place (their voices keep
        // playing), removed buses re-attach their live voices to the voice's fixed
        // fallback bus (they SURVIVE the rebuild), new buses splice in. Volume/mute/
        // effect chains behave exactly like the fixed buses.
        [[nodiscard]] bool HasNamedBus(StringView name) const;
        [[nodiscard]] u32 NamedBusCount() const;
        void SetNamedBusVolume(StringView name, f32 volume);
        [[nodiscard]] f32 NamedBusVolume(StringView name) const; // 0 when unknown
        void SetNamedBusMuted(StringView name, bool muted);
        [[nodiscard]] bool NamedBusMuted(StringView name) const;
        [[nodiscard]] u32 NamedBusEffectCount(StringView name) const;

        // ---- music (P2): scene-less helpers on the Music bus with cross-fade ----
        // Music routes through the SAME graph as everything else (the Sedulous stream-
        // bypass is structurally impossible here); it carries no scene group, so it
        // survives scene swaps. PlayMusic fades the previous music voice out and the
        // new one in over `crossFadeSeconds`.
        VoiceHandle PlayMusic(const RefPtr<AudioClip>& clip, f32 crossFadeSeconds = 1.0f,
                              f32 volume = 1.0f);
        void StopMusic(f32 fadeSeconds = 1.0f);
        [[nodiscard]] VoiceHandle MusicVoice() const;

        // ---- per-scene groups (open question 1: YES - a per-scene child group under
        // each bus, so scene pause/stop-all falls out of the graph naturally) ----
        [[nodiscard]] u64 CreateSceneGroup();
        /// Immediately stops + frees every voice in the group, then drops the group.
        void DestroySceneGroup(u64 sceneGroup);
        /// Fades the group's output and halts its voices in place; resume fades back in.
        void SetSceneGroupPaused(u64 sceneGroup, bool paused);
        [[nodiscard]] bool IsSceneGroupPaused(u64 sceneGroup) const;
        /// Fade-stops every voice in the group (they reap as fades land).
        void StopSceneGroup(u64 sceneGroup);

        // ---- per-scene reverb (P3 zones): a Freeverb node on the scene's Effects
        // child group, wet driven by listener zone occupancy. wet 0 = bypass (the node
        // stays spliced once created; params update live). The same params' roomSize/
        // damping also retune the scene's SEND reverb (per-voice reverbSend), which is
        // wet-only and fed by voice splitters regardless of zone occupancy. ----
        void SetSceneReverb(u64 sceneGroup, const AudioReverbParams& params);
        [[nodiscard]] f32 SceneReverbWet(u64 sceneGroup) const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    /// Decodes `encoded` container bytes (wav/flac/mp3/vorbis) and reduces them to
    /// `buckets` per-bucket PEAK magnitudes in [0, 1] (max |sample| across channels) -
    /// the editor waveform thumbnail. Engine-free (a pure decode; works headless).
    /// False on decode failure, empty input, or zero buckets.
    [[nodiscard]] bool BuildWaveformPeaks(Span<const byte> encoded, u32 buckets,
                                          Array<f32>& outPeaks);
}
