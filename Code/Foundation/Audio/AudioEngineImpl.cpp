// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Audio - AudioEngine implementation (module implementation unit).
//
// All miniaudio contact lives here, so the interface stays free of ma_* types (for API
// hygiene, and because GCC's module serializer must not see the 4 MB header in an interface
// unit's global fragment). The MINIAUDIO_IMPLEMENTATION translation unit is MiniaudioImpl.cpp;
// this file only consumes the declarations.
//
// Layout of the miniaudio object graph:
//   ma_engine (device or headless mixer, and a resource manager with our ma_vfs bridge)
//     Master group, with the Effects, Music and UI groups under it (the fixed buses)
//       run and scene child groups under each bus (lazy; a scene pause stops its group node)
//         ma_sound voices from the fixed pool
//           [an ma_lpf node per 3D voice for distance low-pass (VoiceSlot::lowpassNode),
//            and a splitter for the reverb send (VoiceSlot::splitterNode)]

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

#include "miniaudio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

module foundation.audio;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;

namespace foundation::audio
{
    namespace
    {
        [[nodiscard]] ma_attenuation_model ToMiniaudio(AudioAttenuationModel model)
        {
            switch (model)
            {
            case AudioAttenuationModel::None:
                return ma_attenuation_model_none;
            case AudioAttenuationModel::Inverse:
                return ma_attenuation_model_inverse;
            case AudioAttenuationModel::Linear:
                return ma_attenuation_model_linear;
            case AudioAttenuationModel::Exponential:
                return ma_attenuation_model_exponential;
            }
            return ma_attenuation_model_inverse;
        }

        constexpr f32 kDegreesToRadians = 3.14159265358979323846f / 180.0f;

        enum class VoiceState : u8
        {
            Free = 0,
            Playing,
            Paused,
            Stopping, // fade-to-stop in flight; reaped when the fade lands
        };

        // Registered in-memory payload handed to miniaudio's resource manager. The
        // manager does not copy: the registry owns the decoded frames and keeps the
        // clip (and with it the encoded bytes) alive.
        struct RegisteredClip
        {
            RefPtr<AudioClip> keepAlive;
            Array<f32> decodedFrames; // decode-on-load payload (empty = encoded)
            u64 decodedFrameCount = 0;
            u32 decodedChannels = 0;
            bool registered = false;     // full-quality registration succeeded
            bool monoRegistered = false; // spatial downmix variant ("dclipm:")
            Array<f32> monoFrames;
            u64 monoFrameCount = 0;
        };

        struct DedupeEntry
        {
            f64 time = -1.0e9;
            VoiceHandle handle;
        };

        // One open stream handed to miniaudio through the ma_vfs bridge.
        struct BridgedFile
        {
            UniquePtr<IStream> stream;
            IAllocator* allocator = nullptr; // frees this file in VfsClose
        };

        void FormatClipName(char* buffer, usize size, const char* prefix, const void* key)
        {
            std::snprintf(buffer, size, "%s%016llx", prefix,
                          static_cast<unsigned long long>(reinterpret_cast<uptr>(key)));
        }

        // ---- the Freeverb ma_node wrapper: pure DSP lives in :reverb ----
        // Continuous processing, so the tail keeps ringing after inputs stop.
        struct ReverbNode
        {
            ma_node_base base; // first, so a ReverbNode* is a valid ma_node*
            FreeverbState* state = nullptr;
            u32 channels = 0;
        };

        void ReverbNodeProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn,
                               float** framesOut, ma_uint32* frameCountOut)
        {
            auto* reverb = reinterpret_cast<ReverbNode*>(node);
            const ma_uint32 frames =
                *frameCountOut < *frameCountIn ? *frameCountOut : *frameCountIn;
            if (reverb->channels == 2 && reverb->state != nullptr)
            {
                reverb->state->ProcessStereo(framesIn[0], framesOut[0], frames);
            }
            else
            {
                std::memcpy(framesOut[0], framesIn[0],
                            static_cast<usize>(frames) * reverb->channels * sizeof(float));
            }
            *frameCountIn = frames;
            *frameCountOut = frames;
        }

        ma_node_vtable g_reverbNodeVtable = {ReverbNodeProcess, nullptr, 1, 1,
                                             MA_NODE_FLAG_CONTINUOUS_PROCESSING};

        [[nodiscard]] ReverbNode* CreateReverbNode(IAllocator& allocator, ma_engine& engine,
                                                   const AudioReverbParams& params)
        {
            auto* node = allocator.New<ReverbNode>();
            node->channels = ma_engine_get_channels(&engine);
            node->state = allocator.New<FreeverbState>();
            node->state->Initialize(ma_engine_get_sample_rate(&engine));
            node->state->SetParams(params);
            const ma_uint32 channels[1] = {node->channels};
            ma_node_config config = ma_node_config_init();
            config.vtable = &g_reverbNodeVtable;
            config.pInputChannels = channels;
            config.pOutputChannels = channels;
            if (ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &node->base) !=
                MA_SUCCESS)
            {
                allocator.Delete(node->state);
                allocator.Delete(node);
                return nullptr;
            }
            return node;
        }

        void DestroyReverbNode(IAllocator& allocator, ReverbNode* node)
        {
            if (node == nullptr)
            {
                return;
            }
            ma_node_uninit(&node->base, nullptr);
            allocator.Delete(node->state);
            allocator.Delete(node);
        }
    }

    // Per-bus effect chain node: typed so teardown calls the right uninit. Shared by
    // the fixed buses and the named custom buses.
    struct BusEffectNode
    {
        AudioBusEffectKind kind = AudioBusEffectKind::None;
        void* node = nullptr;
    };

    // A named custom bus: one extra ma_sound_group, parented to a fixed bus or another
    // custom bus per the layout.
    struct CustomBusData
    {
        String name;
        AudioBus fixedParent = AudioBus::Master; // parent when parentCustom < 0
        i32 parentCustom = -1;                   // index into Impl::customBuses
        f32 volume = 1.0f;
        bool muted = false;
        ma_sound_group group{};
        bool initialized = false;
        Array<BusEffectNode> effects;
        // Runs' and scenes' child groups under this bus (made lazily, heap-held: ma_sound_group is
        // address-stable), the same way the fixed buses have a child per run and per scene: a
        // voice on this bus routes through its scene's child, then its run's child, then this
        // bus, so the run's and the scene's stop, pause, mute and gains reach it through the graph.
        HashMap<u64, ma_sound_group*> runChildren;
        HashMap<u64, ma_sound_group*> sceneChildren;
    };

    // A value eased from `from` to `to` over `duration` seconds (inactive = settled).
    struct VoiceRamp
    {
        f32 from = 0.0f;
        f32 to = 0.0f;
        f32 elapsed = 0.0f;
        f32 duration = 0.0f;
        bool active = false;

        void Start(f32 current, f32 target, f32 seconds)
        {
            from = current;
            to = target;
            elapsed = 0.0f;
            duration = seconds;
            active = true;
        }
        // Advance by dt; the value now (eased in and out, so a speed-up has no corner).
        [[nodiscard]] f32 Advance(f32 dt)
        {
            elapsed += dt;
            if (elapsed >= duration)
            {
                active = false;
                return to;
            }
            const f32 t = elapsed / duration;
            return from + (to - from) * (t * t * (3.0f - 2.0f * t));
        }
    };

    struct VoiceSlot
    {
        // Arena-backed (Impl::soundArena): the ma_sound must survive its slot on a
        // faded steal: the pointer is handed over to the dying side list while the slot
        // is immediately reused. ma_sound is not movable (the node graph points into
        // it), so ownership moves, the storage never does.
        ma_sound* sound = nullptr;
        bool soundInitialized = false;
        u32 generation = 1;
        VoiceState state = VoiceState::Free;
        u8 priority = 0;
        bool spatial = false;
        bool looping = false;
        String customBusName; // non-empty = routed through a named custom bus
        Float3 position{0.0f, 0.0f, 0.0f};
        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        VoiceRamp volumeRamp; // SetVoiceVolume over seconds
        VoiceRamp pitchRamp;  // SetVoicePitch over seconds
        AudioBus bus = AudioBus::Effects;
        u64 sceneGroup = 0;
        u64 runGroup = 0; // the run this voice belongs to (its own, or its scene group's)
        RefPtr<AudioClip> clip;
        // Per-voice reverb send: a splitter at the end of the voice chain. Out 0 is
        // the dry path to the group; out 1 (volume = reverbSend) feeds the scene's
        // wet-only send reverb in parallel. nullptr = played with reverbSend 0.
        ma_splitter_node* splitterNode = nullptr;
        f32 reverbSend = 0.0f;
        // Distance low-pass: an ma_lpf node between the sound and its group; the
        // cutoff glides from open to the floor across [min, max] distance every Update.
        ma_lpf_node* lowpassNode = nullptr;
        f32 lowpassFloorHz = 0.0f;
        f32 lowpassMinDistance = 1.0f;
        f32 lowpassMaxDistance = 100.0f;
        f32 lowpassCutoffHz = 0.0f;
    };

    // Per-scene child groups, one under each bus the scene actually uses (lazy).
    struct SceneGroupData
    {
        u64 runGroup = 0; // the run whose groups this scene nests under (0 = the buses)
        ma_sound_group group[static_cast<usize>(AudioBus::Count)]{};
        bool initialized[static_cast<usize>(AudioBus::Count)] = {};
        bool paused = false;
        ReverbNode* reverb = nullptr; // zone reverb on the Effects child group
        f32 reverbWet = 0.0f;
        // The per-voice send target: a wet-only Freeverb (dry pinned to 0) fed by
        // voice splitters, straight into the Effects bus. Zones retune roomSize and
        // damping through SetSceneReverb; sends stay audible regardless of zone
        // occupancy (the send level is the voice's own knob).
        ReverbNode* sendReverb = nullptr;
    };

    // One running game's groups: a child under each bus the run uses (lazy), the run's own bus
    // gains, its music slot, and its whole-run pause / mute / volume. Scene groups of the run
    // parent under these instead of the buses.
    struct RunGroupData
    {
        ma_sound_group group[static_cast<usize>(AudioBus::Count)]{};
        bool initialized[static_cast<usize>(AudioBus::Count)] = {};
        f32 busGain[static_cast<usize>(AudioBus::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool busMuted[static_cast<usize>(AudioBus::Count)] = {};
        f32 volume = 1.0f;
        bool paused = false;
        bool muted = false;
        f32 muteFactor = 1.0f; // ramps toward 0 (muted) or 1 over kRunMuteFadeSeconds
        VoiceHandle musicVoice;
        // The run's own gains for a layout's custom buses, by name (a layout rebuild keeps them).
        struct NamedGain
        {
            String name;
            f32 gain = 1.0f;
            bool muted = false;
        };
        Array<NamedGain> namedGains;

        [[nodiscard]] const NamedGain* FindNamed(StringView name) const
        {
            for (const NamedGain& entry : namedGains)
            {
                if (entry.name.AsView() == name)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
        [[nodiscard]] NamedGain& EnsureNamed(StringView name)
        {
            for (NamedGain& entry : namedGains)
            {
                if (entry.name.AsView() == name)
                {
                    return entry;
                }
            }
            namedGains.PushBack(NamedGain{String(name), 1.0f, false});
            return namedGains[namedGains.Size() - 1];
        }
    };

    static constexpr f32 kRunMuteFadeSeconds = 0.1f;

    struct AudioEngine::Impl
    {
        // ---- ma_vfs bridge: miniaudio streams straight out of foundation.vfs ----
        // First member so a pointer to Bridge is a valid ma_vfs*.
        struct Bridge
        {
            ma_vfs_callbacks callbacks{};
            Impl* impl = nullptr;
        };

        IAllocator* allocator;
        AudioEngineSettings settings;
        bool headless = false;
        bool engineInitialized = false;
        ma_engine engine{};
        Bridge bridge{};

        f32 masterVolume = 1.0f; // engine endpoint gain, applied at init and on set

        ma_sound_group busGroups[static_cast<usize>(AudioBus::Count)]{};
        bool busGroupInitialized[static_cast<usize>(AudioBus::Count)] = {};
        f32 busVolume[static_cast<usize>(AudioBus::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool busMuted[static_cast<usize>(AudioBus::Count)] = {};

        Array<VoiceSlot> voices; // [0, voiceCount) in-memory, then stream slots
        u32 voiceCount = 0;
        u32 streamVoiceCount = 0;

        // Fixed ma_sound storage for the pool and the dying headroom (never resized after
        // init, so pointers into it are stable), and the free-index stack.
        Array<ma_sound> soundArena;
        Array<u32> freeSounds;

        // Faded steal: the victim's ma_sound fades here (~30 ms)
        // while its slot is reused at once. Capacity-bounded; oldest hard-cuts on
        // overflow. Entries keep the clip alive until the fade lands.
        struct DyingVoice
        {
            ma_sound* sound = nullptr;
            ma_lpf_node* lowpass = nullptr;
            ma_splitter_node* splitter = nullptr;
            RefPtr<AudioClip> clip;
        };
        Array<DyingVoice> dyingVoices;

        HashMap<u64, SceneGroupData*> sceneGroups; // owned via the engine allocator New/Delete
        u64 nextSceneGroupId = 1;
        HashMap<u64, RunGroupData*> runGroups; // owned via the engine allocator New/Delete
        u64 nextRunGroupId = 1;

        HashMap<void*, RegisteredClip> registeredClips; // key = AudioClip*
        HashMap<void*, RefPtr<AudioClip>> streamClips;  // ma_vfs name -> clip keep-alive
        HashMap<void*, DedupeEntry> recentPlays;        // key = AudioClip*

        Float3 listenerPosition{0.0f, 0.0f, 0.0f};
        VoiceHandle musicVoice; // the PlayMusic cross-fade tracks one music voice

        Array<BusEffectNode> busEffects[static_cast<usize>(AudioBus::Count)];
        // Named custom buses (heap entries: ma_sound_group is address-stable).
        Array<CustomBusData*> customBuses;
        f64 timeSeconds = 0.0;
        bool warnedMonoDownmix = false;
        bool warnedStreamStereoSpatial = false;
        bool warnedUnknownBusName = false;
        Array<f32> pumpScratch; // headless mixing scratch

        // ---------------- ma_vfs callbacks ----------------

        static ma_result VfsOpen(ma_vfs* vfs, const char* path, ma_uint32 openMode,
                                 ma_vfs_file* outFile)
        {
            if ((openMode & MA_OPEN_MODE_WRITE) != 0)
            {
                return MA_NOT_IMPLEMENTED;
            }
            Impl* impl = reinterpret_cast<Bridge*>(vfs)->impl;
            UniquePtr<IStream> stream = impl->OpenBridgedStream(path);
            if (stream.Get() == nullptr)
            {
                return MA_DOES_NOT_EXIST;
            }
            auto* file = impl->allocator->New<BridgedFile>();
            file->stream = Move(stream);
            file->allocator = impl->allocator;
            *outFile = reinterpret_cast<ma_vfs_file>(file);
            return MA_SUCCESS;
        }
        static ma_result VfsClose(ma_vfs*, ma_vfs_file file)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            bridged->allocator->Delete(bridged);
            return MA_SUCCESS;
        }
        static ma_result VfsRead(ma_vfs*, ma_vfs_file file, void* destination, size_t bytes,
                                 size_t* outRead)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            const u64 read = bridged->stream->Read(destination, static_cast<u64>(bytes));
            if (outRead != nullptr)
            {
                *outRead = static_cast<size_t>(read);
            }
            return read == 0 && bytes > 0 ? MA_AT_END : MA_SUCCESS;
        }
        static ma_result VfsSeek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            const SeekOrigin mapped = origin == ma_seek_origin_start     ? SeekOrigin::Begin
                                      : origin == ma_seek_origin_current ? SeekOrigin::Current
                                                                         : SeekOrigin::End;
            return bridged->stream->Seek(static_cast<i64>(offset), mapped) < 0 ? MA_ERROR
                                                                               : MA_SUCCESS;
        }
        static ma_result VfsTell(ma_vfs*, ma_vfs_file file, ma_int64* outCursor)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            const i64 cursor = bridged->stream->Tell();
            if (cursor < 0)
            {
                return MA_ERROR;
            }
            *outCursor = static_cast<ma_int64>(cursor);
            return MA_SUCCESS;
        }
        static ma_result VfsInfo(ma_vfs*, ma_vfs_file file, ma_file_info* outInfo)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            const i64 size = bridged->stream->Size();
            if (size < 0)
            {
                return MA_ERROR;
            }
            outInfo->sizeInBytes = static_cast<ma_uint64>(size);
            return MA_SUCCESS;
        }

        // "dstream:<hex>" = a registered stream clip's IAudioStreamSource; anything else
        // resolves through the optional settings mount.
        [[nodiscard]] UniquePtr<IStream> OpenBridgedStream(const char* path)
        {
            if (std::strncmp(path, "dstream:", 8) == 0)
            {
                const unsigned long long key = std::strtoull(path + 8, nullptr, 16);
                RefPtr<AudioClip>* clip =
                    streamClips.Find(reinterpret_cast<void*>(static_cast<uptr>(key)));
                if (clip == nullptr || clip->Get() == nullptr ||
                    clip->Get()->streamSource.Get() == nullptr)
                {
                    return {};
                }
                return clip->Get()->streamSource->OpenStream();
            }
            if (settings.fileSystem != nullptr)
            {
                return settings.fileSystem->Open(
                    StringView(reinterpret_cast<const utf8char*>(path)), FileMode::Read);
            }
            return {};
        }

        // ---------------- construction ----------------

        Impl(IAllocator& alloc, const AudioEngineSettings& engineSettings)
            : allocator(&alloc), settings(engineSettings)
        {
            bridge.callbacks.onOpen = &VfsOpen;
            bridge.callbacks.onClose = &VfsClose;
            bridge.callbacks.onRead = &VfsRead;
            bridge.callbacks.onWrite = nullptr;
            bridge.callbacks.onSeek = &VfsSeek;
            bridge.callbacks.onTell = &VfsTell;
            bridge.callbacks.onInfo = &VfsInfo;
            bridge.impl = this;

            headless = settings.headless;
            if (!InitializeEngine(headless) && !headless)
            {
                LOG_WARNING(
                    u8"Audio",
                    u8"no playback device available - running headless (Null mode: voices "
                    u8"advance silently, handles stay valid)");
                headless = true;
                (void)InitializeEngine(true);
            }
            if (!engineInitialized)
            {
                LOG_ERROR(u8"Audio",
                                   u8"audio engine failed to initialize - audio disabled");
                return;
            }

            InitializeBusGroups();

            voiceCount = settings.voiceCount;
            streamVoiceCount = settings.streamVoiceCount;
            voices.Resize(static_cast<usize>(voiceCount) + streamVoiceCount);
            // Storage for every pool slot plus the dying headroom (a steal briefly
            // needs both the victim's fading sound and the newcomer's).
            soundArena.Resize(static_cast<usize>(voiceCount) + streamVoiceCount +
                              settings.dyingVoiceCapacity);
            for (usize i = soundArena.Size(); i > 0; --i)
            {
                freeSounds.PushBack(static_cast<u32>(i - 1));
            }
        }

        [[nodiscard]] ma_sound* AcquireSound()
        {
            if (freeSounds.IsEmpty())
            {
                return nullptr;
            } // unreachable by construction
            const u32 index = freeSounds.Back();
            freeSounds.PopBack();
            return &soundArena[index];
        }

        void ReleaseSound(ma_sound* sound)
        {
            if (sound == nullptr)
            {
                return;
            }
            freeSounds.PushBack(static_cast<u32>(sound - soundArena.Data()));
        }

        [[nodiscard]] bool InitializeEngine(bool withoutDevice)
        {
            ma_engine_config config = ma_engine_config_init();
            config.pResourceManagerVFS = &bridge;
            config.listenerCount = Clamp<u32>(settings.listenerCount, 1, 4);
            if (withoutDevice)
            {
                config.noDevice = MA_TRUE;
                config.channels = 2;
                config.sampleRate = settings.sampleRate != 0 ? settings.sampleRate : 48000;
            }
            engineInitialized = ma_engine_init(&config, &engine) == MA_SUCCESS;
            if (engineInitialized && masterVolume != 1.0f)
            {
                ma_engine_set_volume(&engine, masterVolume); // honor a pre-init SetMasterVolume
            }
            return engineInitialized;
        }

        // The node a bus's output feeds when it has no effects: Master for the leaves,
        // the graph endpoint for Master itself.
        [[nodiscard]] ma_node* BusParentNode(usize bus)
        {
            return bus == static_cast<usize>(AudioBus::Master)
                       ? ma_node_graph_get_endpoint(ma_engine_get_node_graph(&engine))
                       : reinterpret_cast<ma_node*>(
                             &busGroups[static_cast<usize>(AudioBus::Master)]);
        }

        // Frees every node in `chain` (the caller re-attaches the source first).
        void ClearEffectChain(Array<BusEffectNode>& chain)
        {
            for (BusEffectNode& effect : chain)
            {
                switch (effect.kind)
                {
                case AudioBusEffectKind::Lowpass:
                    ma_lpf_node_uninit(static_cast<ma_lpf_node*>(effect.node), nullptr);
                    allocator->Delete(static_cast<ma_lpf_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Highpass:
                    ma_hpf_node_uninit(static_cast<ma_hpf_node*>(effect.node), nullptr);
                    allocator->Delete(static_cast<ma_hpf_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Delay:
                    ma_delay_node_uninit(static_cast<ma_delay_node*>(effect.node), nullptr);
                    allocator->Delete(static_cast<ma_delay_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Reverb:
                    DestroyReverbNode(*allocator, static_cast<ReverbNode*>(effect.node));
                    break;
                case AudioBusEffectKind::None:
                    break;
                }
            }
            chain.Clear();
        }

        void ClearBusEffects(usize bus)
        {
            if (busGroupInitialized[bus])
            {
                (void)ma_node_attach_output_bus(&busGroups[bus], 0, BusParentNode(bus), 0);
            }
            ClearEffectChain(busEffects[bus]);
        }

        // Rebuild one bus's chain, from its group through each effect in order to its parent.
        void BuildBusEffects(usize bus, Span<const AudioBusEffectDesc> effects)
        {
            ClearBusEffects(bus);
            if (!busGroupInitialized[bus])
            {
                return;
            }
            BuildEffectChain(reinterpret_cast<ma_node*>(&busGroups[bus]), busEffects[bus],
                             BusParentNode(bus), effects);
        }

        // Generic splice from `source` through each effect in order to `parent` (shared by
        // fixed and named custom buses). `chain` must be empty on entry.
        void BuildEffectChain(ma_node* source, Array<BusEffectNode>& chain, ma_node* parent,
                              Span<const AudioBusEffectDesc> effects)
        {
            const u32 channels = ma_engine_get_channels(&engine);
            const u32 sampleRate = ma_engine_get_sample_rate(&engine);
            ma_node_graph* graph = ma_engine_get_node_graph(&engine);

            ma_node* upstream = source;
            for (const AudioBusEffectDesc& desc : effects)
            {
                BusEffectNode effect;
                effect.kind = desc.kind;
                switch (desc.kind)
                {
                case AudioBusEffectKind::Lowpass:
                {
                    auto* node = allocator->New<ma_lpf_node>();
                    ma_lpf_node_config config = ma_lpf_node_config_init(
                        channels, sampleRate, Max(desc.frequencyHz, 10.0f), kLowpassOrder);
                    if (ma_lpf_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        allocator->Delete(node);
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::Highpass:
                {
                    auto* node = allocator->New<ma_hpf_node>();
                    ma_hpf_node_config config = ma_hpf_node_config_init(
                        channels, sampleRate, Max(desc.frequencyHz, 10.0f), kLowpassOrder);
                    if (ma_hpf_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        allocator->Delete(node);
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::Delay:
                {
                    auto* node = allocator->New<ma_delay_node>();
                    const u32 delayFrames = static_cast<u32>(Max(desc.delaySeconds, 0.001f) *
                                                             static_cast<f32>(sampleRate));
                    ma_delay_node_config config = ma_delay_node_config_init(
                        channels, sampleRate, delayFrames, Clamp(desc.delayDecay, 0.0f, 0.99f));
                    if (ma_delay_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        allocator->Delete(node);
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::Reverb:
                {
                    AudioReverbParams params;
                    params.roomSize = desc.roomSize;
                    params.damping = desc.damping;
                    params.wet = desc.wetLevel;
                    ReverbNode* node = CreateReverbNode(*allocator, engine, params);
                    if (node == nullptr)
                    {
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::None:
                    continue;
                }
                (void)ma_node_attach_output_bus(upstream, 0, effect.node, 0);
                upstream = effect.node;
                chain.PushBack(effect);
            }
            (void)ma_node_attach_output_bus(upstream, 0, parent, 0);
        }

        // ---------------- named custom buses ----------------

        [[nodiscard]] i32 FindCustomBus(StringView name) const
        {
            for (usize i = 0; i < customBuses.Size(); ++i)
            {
                if (customBuses[i]->name.AsView() == name)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] ma_node* CustomBusParentNode(const CustomBusData& bus)
        {
            if (bus.parentCustom >= 0 &&
                static_cast<usize>(bus.parentCustom) < customBuses.Size() &&
                customBuses[static_cast<usize>(bus.parentCustom)]->initialized)
            {
                return reinterpret_cast<ma_node*>(
                    &customBuses[static_cast<usize>(bus.parentCustom)]->group);
            }
            const usize fixed = static_cast<usize>(bus.fixedParent);
            return busGroupInitialized[fixed] ? reinterpret_cast<ma_node*>(&busGroups[fixed])
                                              : BusParentNode(static_cast<usize>(AudioBus::Master));
        }

        // A removed custom bus hands its live voices back to their fixed fallback bus
        // (voices survive a layout rebuild; only the routing changes).
        void DetachVoicesFromCustomBus(CustomBusData& bus)
        {
            for (VoiceSlot& slot : voices)
            {
                if (slot.state == VoiceState::Free ||
                    slot.customBusName.AsView() != bus.name.AsView())
                {
                    continue;
                }
                if (ma_sound_group* fallback = GroupFor(slot.sceneGroup, slot.runGroup, slot.bus))
                {
                    (void)ma_node_attach_output_bus(VoiceOutputNode(slot), 0, fallback, 0);
                }
                slot.customBusName = String{};
            }
        }

        // The voice chain's last node (what attaches to the group): the splitter (dry out
        // 0) if present, else the distance low-pass, else the sound itself.
        [[nodiscard]] static ma_node* VoiceOutputNode(VoiceSlot& slot)
        {
            if (slot.splitterNode != nullptr)
            {
                return slot.splitterNode;
            }
            if (slot.lowpassNode != nullptr)
            {
                return reinterpret_cast<ma_node*>(slot.lowpassNode);
            }
            return reinterpret_cast<ma_node*>(slot.sound);
        }

        // Lazily creates the scene's wet-only send reverb (per-voice reverbSend
        // target), attached straight to the Effects bus. Zone updates retune it.
        [[nodiscard]] ReverbNode* EnsureSceneSendReverb(u64 sceneGroup)
        {
            SceneGroupData** found = sceneGroups.Find(sceneGroup);
            if (found == nullptr)
            {
                return nullptr;
            }
            SceneGroupData& data = **found;
            if (data.sendReverb == nullptr)
            {
                AudioReverbParams params;
                params.wet = 1.0f; // full tail; the send level is the voice's knob
                params.dry = 0.0f; // wet-only: the dry path already reaches the bus
                data.sendReverb = CreateReverbNode(*allocator, engine, params);
                if (data.sendReverb != nullptr &&
                    busGroupInitialized[static_cast<usize>(AudioBus::Effects)])
                {
                    (void)ma_node_attach_output_bus(
                        data.sendReverb, 0, &busGroups[static_cast<usize>(AudioBus::Effects)], 0);
                }
            }
            return data.sendReverb;
        }

        void DestroyCustomBus(CustomBusData* bus)
        {
            DetachVoicesFromCustomBus(*bus);
            // Scene children first (they may hang under run children), then the runs'.
            Array<u64> keys;
            for (auto& entry : bus->sceneChildren)
            {
                keys.PushBack(entry.key);
            }
            for (u64 key : keys)
            {
                FreeChildGroup(bus->sceneChildren, key);
            }
            keys.Clear();
            for (auto& entry : bus->runChildren)
            {
                keys.PushBack(entry.key);
            }
            for (u64 key : keys)
            {
                FreeChildGroup(bus->runChildren, key);
            }
            ClearEffectChain(bus->effects);
            if (bus->initialized)
            {
                ma_sound_group_uninit(&bus->group);
            }
            allocator->Delete(bus);
        }

        // Reconcile the live custom-bus set with the layout by name: kept buses update
        // in place (voices keep playing through them), removed buses hand their voices
        // back, new buses splice in. Cycles are defused to Master (the cook rejects them;
        // this is the runtime backstop).
        void RebuildCustomBuses(Span<const AudioNamedBus> desired)
        {
            // Filter: named, first occurrence wins.
            Array<const AudioNamedBus*> wanted;
            for (const AudioNamedBus& named : desired)
            {
                if (named.name.IsEmpty())
                {
                    continue;
                }
                bool duplicate = false;
                for (const AudioNamedBus* seen : wanted)
                {
                    if (seen->name.AsView() == named.name.AsView())
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                {
                    LOG_WARNING(
                        u8"Audio", u8"bus layout: duplicate custom bus '{}' ignored", named.name);
                    continue;
                }
                AudioBus fixedAlias{};
                if (AudioBusFromName(named.name.AsView(), fixedAlias))
                {
                    LOG_WARNING(
                        u8"Audio", u8"bus layout: custom bus '{}' shadows a fixed bus - ignored",
                        named.name);
                    continue;
                }
                wanted.PushBack(&named);
            }

            // Drop buses no longer in the layout.
            for (usize i = customBuses.Size(); i > 0; --i)
            {
                CustomBusData* bus = customBuses[i - 1];
                bool keep = false;
                for (const AudioNamedBus* named : wanted)
                {
                    if (named->name.AsView() == bus->name.AsView())
                    {
                        keep = true;
                        break;
                    }
                }
                if (!keep)
                {
                    DestroyCustomBus(bus);
                    customBuses.RemoveAt(i - 1);
                }
            }

            // Create the missing groups (parented to Master; re-parented below).
            for (const AudioNamedBus* named : wanted)
            {
                if (FindCustomBus(named->name.AsView()) >= 0)
                {
                    continue;
                }
                auto* bus = allocator->New<CustomBusData>();
                bus->name = String(named->name.AsView());
                bus->initialized = ma_sound_group_init(
                                       &engine, 0, &busGroups[static_cast<usize>(AudioBus::Master)],
                                       &bus->group) == MA_SUCCESS;
                customBuses.PushBack(bus);
            }

            // Resolve parents (fixed bus name, custom bus name, or empty = Master).
            for (const AudioNamedBus* named : wanted)
            {
                const i32 index = FindCustomBus(named->name.AsView());
                if (index < 0)
                {
                    continue;
                }
                CustomBusData& bus = *customBuses[static_cast<usize>(index)];
                bus.fixedParent = AudioBus::Master;
                bus.parentCustom = -1;
                if (!named->parent.IsEmpty())
                {
                    AudioBus fixed{};
                    if (AudioBusFromName(named->parent.AsView(), fixed))
                    {
                        bus.fixedParent = fixed;
                    }
                    else if (const i32 parent = FindCustomBus(named->parent.AsView());
                             parent >= 0 && parent != index)
                    {
                        bus.parentCustom = parent;
                    }
                    else
                    {
                        LOG_WARNING(
                            u8"Audio",
                            u8"bus layout: custom bus '{}' has unknown parent '{}' - "
                            u8"parented to Master",
                            named->name, named->parent);
                    }
                }
            }

            // Defuse parent cycles (each cycle breaks at the first member that sees it).
            for (usize i = 0; i < customBuses.Size(); ++i)
            {
                i32 cursor = customBuses[i]->parentCustom;
                usize steps = 0;
                while (cursor >= 0 && steps <= customBuses.Size())
                {
                    if (cursor == static_cast<i32>(i))
                    {
                        LOG_WARNING(
                            u8"Audio",
                            u8"bus layout: custom bus '{}' is part of a parent CYCLE - "
                            u8"parented to Master",
                            customBuses[i]->name);
                        customBuses[i]->parentCustom = -1;
                        customBuses[i]->fixedParent = AudioBus::Master;
                        break;
                    }
                    cursor = customBuses[static_cast<usize>(cursor)]->parentCustom;
                    ++steps;
                }
            }

            // Apply settings and rebuild each bus's effect chain into its parent.
            for (const AudioNamedBus* named : wanted)
            {
                const i32 index = FindCustomBus(named->name.AsView());
                if (index < 0)
                {
                    continue;
                }
                CustomBusData& bus = *customBuses[static_cast<usize>(index)];
                bus.volume = named->settings.volume < 0.0f ? 0.0f : named->settings.volume;
                bus.muted = named->settings.muted;
                if (!bus.initialized)
                {
                    continue;
                }
                ma_sound_group_set_volume(&bus.group, bus.muted ? 0.0f : bus.volume);
                (void)ma_node_attach_output_bus(&bus.group, 0, CustomBusParentNode(bus), 0);
                ClearEffectChain(bus.effects);
                BuildEffectChain(reinterpret_cast<ma_node*>(&bus.group), bus.effects,
                                 CustomBusParentNode(bus),
                                 Span<const AudioBusEffectDesc>(named->settings.effects.Data(),
                                                                named->settings.effects.Size()));
            }

            // A re-parented bus changes each run's gain down its chain: re-apply them.
            for (auto& entry : runGroups)
            {
                ApplyRunGains(entry.key, *entry.value);
            }
        }

        void InitializeBusGroups()
        {
            // Master first; the leaf buses parent to it (the fixed layout).
            for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
            {
                ma_sound_group* parent = bus == static_cast<usize>(AudioBus::Master)
                                             ? nullptr
                                             : &busGroups[static_cast<usize>(AudioBus::Master)];
                busGroupInitialized[bus] =
                    ma_sound_group_init(&engine, 0, parent, &busGroups[bus]) == MA_SUCCESS;
            }
        }

        ~Impl()
        {
            if (engineInitialized)
            {
                while (!dyingVoices.IsEmpty())
                {
                    ReapDyingVoice(dyingVoices.Size() - 1);
                }
                for (VoiceSlot& slot : voices)
                {
                    ReleaseSlot(slot);
                }
                Array<u64> groupIds;
                for (auto& entry : sceneGroups)
                {
                    groupIds.PushBack(entry.key);
                }
                for (u64 id : groupIds)
                {
                    DestroySceneGroupData(id);
                }
                Array<u64> runIds;
                for (auto& entry : runGroups)
                {
                    runIds.PushBack(entry.key);
                }
                for (u64 id : runIds)
                {
                    DestroyRunGroupData(id);
                }
                for (CustomBusData* bus : customBuses)
                {
                    DestroyCustomBus(bus);
                }
                customBuses.Clear();
                for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
                {
                    ClearBusEffects(bus);
                }
                for (usize bus = static_cast<usize>(AudioBus::Count); bus > 0; --bus)
                {
                    if (busGroupInitialized[bus - 1])
                    {
                        ma_sound_group_uninit(&busGroups[bus - 1]);
                    }
                }
                ma_engine_uninit(&engine);
            }
            // registeredClips / streamClips free their payloads after the engine is gone.
        }

        // ---------------- registration (decode-on-load / compressed-in-memory) ----------------

        [[nodiscard]] RegisteredClip* EnsureRegistered(const RefPtr<AudioClip>& clip, bool wantMono)
        {
            void* key = clip.Get();
            RegisteredClip* entry = registeredClips.Find(key);
            if (entry == nullptr)
            {
                entry = &registeredClips.InsertOrAssign(key, RegisteredClip{});
                entry->keepAlive = clip;
            }

            char name[64];
            if (!entry->registered)
            {
                FormatClipName(name, sizeof(name), "dclip:", key);
                ma_result result = MA_ERROR;
                if (clip->keepCompressed)
                {
                    // Compressed-in-memory: decode on the fly while the voice plays.
                    result = ma_resource_manager_register_encoded_data(
                        ma_engine_get_resource_manager(&engine), name, clip->encodedData.Data(),
                        clip->encodedData.Size());
                }
                else
                {
                    // Decode-on-load, the default for sound effects.
                    if (DecodeToF32(clip->EncodedBytes(), 0, entry->decodedFrames,
                                    entry->decodedChannels, entry->decodedFrameCount))
                    {
                        result = ma_resource_manager_register_decoded_data(
                            ma_engine_get_resource_manager(&engine), name,
                            entry->decodedFrames.Data(), entry->decodedFrameCount, ma_format_f32,
                            entry->decodedChannels, clip->sampleRate);
                    }
                }
                entry->registered = result == MA_SUCCESS;
                if (!entry->registered)
                {
                    LOG_WARNING(u8"Audio",
                                         u8"clip failed to decode/register - not playable");
                    return nullptr;
                }
            }

            if (wantMono && !entry->monoRegistered)
            {
                FormatClipName(name, sizeof(name), "dclipm:", key);
                u32 channels = 0;
                if (DecodeToF32(clip->EncodedBytes(), 1, entry->monoFrames, channels,
                                entry->monoFrameCount) &&
                    ma_resource_manager_register_decoded_data(
                        ma_engine_get_resource_manager(&engine), name, entry->monoFrames.Data(),
                        entry->monoFrameCount, ma_format_f32, 1, clip->sampleRate) == MA_SUCCESS)
                {
                    entry->monoRegistered = true;
                }
            }
            return entry;
        }

        [[nodiscard]] static bool DecodeToF32(Span<const byte> encoded, u32 targetChannels,
                                              Array<f32>& outFrames, u32& outChannels,
                                              u64& outFrameCount)
        {
            ma_decoder_config config = ma_decoder_config_init(ma_format_f32, targetChannels, 0);
            ma_decoder decoder;
            if (ma_decoder_init_memory(encoded.Data(), encoded.Size(), &config, &decoder) !=
                MA_SUCCESS)
            {
                return false;
            }
            outChannels = decoder.outputChannels;
            outFrames.Clear();
            outFrameCount = 0;
            f32 chunk[4096];
            const u64 chunkFrames = 4096 / decoder.outputChannels;
            for (;;)
            {
                ma_uint64 read = 0;
                const ma_result result =
                    ma_decoder_read_pcm_frames(&decoder, chunk, chunkFrames, &read);
                for (u64 i = 0; i < read * decoder.outputChannels; ++i)
                {
                    outFrames.PushBack(chunk[i]);
                }
                outFrameCount += read;
                if (result != MA_SUCCESS || read < chunkFrames)
                {
                    break;
                }
            }
            ma_decoder_uninit(&decoder);
            return outFrameCount > 0;
        }

        // ---------------- pool ----------------

        [[nodiscard]] VoiceHandle HandleFor(usize slotIndex) const
        {
            return VoiceHandle{static_cast<u32>(slotIndex), voices[slotIndex].generation};
        }

        [[nodiscard]] VoiceSlot* Resolve(VoiceHandle handle)
        {
            if (!handle.IsValid() || handle.slot >= voices.Size())
            {
                return nullptr;
            }
            VoiceSlot& slot = voices[handle.slot];
            if (slot.state == VoiceState::Free || slot.generation != handle.generation)
            {
                return nullptr;
            }
            return &slot;
        }
        [[nodiscard]] const VoiceSlot* Resolve(VoiceHandle handle) const
        {
            return const_cast<Impl*>(this)->Resolve(handle);
        }

        void ReleaseSlot(VoiceSlot& slot)
        {
            if (slot.soundInitialized)
            {
                ma_sound_uninit(slot.sound);
                slot.soundInitialized = false;
            }
            ReleaseSound(slot.sound);
            slot.sound = nullptr;
            if (slot.lowpassNode != nullptr)
            {
                ma_lpf_node_uninit(slot.lowpassNode, nullptr);
                allocator->Delete(slot.lowpassNode);
                slot.lowpassNode = nullptr;
            }
            if (slot.splitterNode != nullptr)
            {
                ma_splitter_node_uninit(slot.splitterNode, nullptr);
                allocator->Delete(slot.splitterNode);
                slot.splitterNode = nullptr;
            }
            ClearSlotBookkeeping(slot);
        }

        void ClearSlotBookkeeping(VoiceSlot& slot)
        {
            slot.lowpassFloorHz = 0.0f;
            slot.lowpassCutoffHz = 0.0f;
            slot.reverbSend = 0.0f;
            slot.state = VoiceState::Free;
            slot.clip = nullptr;
            slot.sceneGroup = 0;
            slot.runGroup = 0;
            slot.customBusName = String{};
            ++slot.generation;
        }

        // Faded steal: the victim's handle dies now (the slot is reused at once), but
        // its ma_sound fades on the dying list instead of clicking off. Silent victims
        // (paused) release immediately: there is nothing audible to protect.
        void StealSlot(VoiceSlot& slot)
        {
            if (!slot.soundInitialized || slot.state == VoiceState::Paused ||
                settings.dyingVoiceCapacity == 0)
            {
                ReleaseSlot(slot);
                return;
            }
            while (dyingVoices.Size() >= settings.dyingVoiceCapacity)
            {
                ReapDyingVoice(0); // full: hard-cut the oldest tail
            }
            (void)ma_sound_stop_with_fade_in_milliseconds(slot.sound, StealFadeMilliseconds());
            DyingVoice dying;
            dying.sound = slot.sound;
            dying.lowpass = slot.lowpassNode;
            dying.splitter = slot.splitterNode;
            dying.clip = slot.clip;
            dyingVoices.PushBack(Move(dying));
            slot.sound = nullptr;
            slot.soundInitialized = false;
            slot.lowpassNode = nullptr;
            slot.splitterNode = nullptr;
            ClearSlotBookkeeping(slot);
        }

        void ReapDyingVoice(usize index)
        {
            DyingVoice& dying = dyingVoices[index];
            if (dying.sound != nullptr)
            {
                ma_sound_uninit(dying.sound);
                ReleaseSound(dying.sound);
            }
            if (dying.lowpass != nullptr)
            {
                ma_lpf_node_uninit(dying.lowpass, nullptr);
                allocator->Delete(dying.lowpass);
            }
            if (dying.splitter != nullptr)
            {
                ma_splitter_node_uninit(dying.splitter, nullptr);
                allocator->Delete(dying.splitter);
            }
            dyingVoices.RemoveAt(index);
        }

        [[nodiscard]] u64 StealFadeMilliseconds() const
        {
            const f32 seconds = settings.stealFadeSeconds > 0.0f ? settings.stealFadeSeconds : 0.0f;
            return static_cast<u64>(seconds * 1000.0f + 0.5f);
        }

        [[nodiscard]] f32 DistanceToListener(const VoiceSlot& slot) const
        {
            if (!slot.spatial)
            {
                return 0.0f;
            }
            const f32 dx = slot.position.x - listenerPosition.x;
            const f32 dy = slot.position.y - listenerPosition.y;
            const f32 dz = slot.position.z - listenerPosition.z;
            return dx * dx + dy * dy + dz * dz; // squared is fine for ordering
        }

        // A free slot, else the lowest priority strictly below the new voice, else the
        // farthest voice of the same priority. Returns the pool size when nothing may be taken.
        [[nodiscard]] usize AcquireSlot(bool streamPool, u8 priority)
        {
            const usize begin = streamPool ? voiceCount : 0;
            const usize end =
                streamPool ? voiceCount + streamVoiceCount : static_cast<usize>(voiceCount);
            for (usize i = begin; i < end; ++i)
            {
                if (voices[i].state == VoiceState::Free)
                {
                    return i;
                }
            }

            usize victim = voices.Size();
            u8 victimPriority = priority; // must be < priority to steal outright
            for (usize i = begin; i < end; ++i)
            {
                if (voices[i].priority < victimPriority ||
                    (victim < voices.Size() && voices[i].priority == victimPriority &&
                     DistanceToListener(voices[i]) > DistanceToListener(voices[victim])))
                {
                    victim = i;
                    victimPriority = voices[i].priority;
                }
            }
            if (victim == voices.Size())
            {
                // No lower-priority victim; try the farthest voice of equal priority.
                for (usize i = begin; i < end; ++i)
                {
                    if (voices[i].priority != priority)
                    {
                        continue;
                    }
                    if (victim == voices.Size() ||
                        DistanceToListener(voices[i]) > DistanceToListener(voices[victim]))
                    {
                        victim = i;
                    }
                }
            }
            if (victim < voices.Size())
            {
                // Faded steal: the victim's slot frees now, its audio tail fades on
                // the dying list (~stealFadeSeconds) instead of clicking off.
                StealSlot(voices[victim]);
                return victim;
            }
            return voices.Size();
        }

        // ---------------- distance low-pass ----------------

        // Cutoff mapping: fully open inside minDistance, gliding linearly down to the
        // voice's floor at maxDistance. Reinit only on an audible change (>1%), since
        // filter reconfiguration is not free.
        void UpdateVoiceLowpass(VoiceSlot& slot)
        {
            if (slot.lowpassNode == nullptr)
            {
                return;
            }
            const f32 dx = slot.position.x - listenerPosition.x;
            const f32 dy = slot.position.y - listenerPosition.y;
            const f32 dz = slot.position.z - listenerPosition.z;
            const f32 distance = Sqrt(dx * dx + dy * dy + dz * dz);
            const f32 open = OpenCutoffHz();
            const f32 range = Max(slot.lowpassMaxDistance - slot.lowpassMinDistance, 0.001f);
            const f32 t = Clamp((distance - slot.lowpassMinDistance) / range, 0.0f, 1.0f);
            const f32 cutoff = open + (slot.lowpassFloorHz - open) * t;
            if (slot.lowpassCutoffHz > 0.0f &&
                Abs(cutoff - slot.lowpassCutoffHz) < slot.lowpassCutoffHz * 0.01f)
            {
                return;
            }
            ma_lpf_config config =
                ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine),
                                   ma_engine_get_sample_rate(&engine), cutoff, kLowpassOrder);
            if (ma_lpf_node_reinit(&config, slot.lowpassNode) == MA_SUCCESS)
            {
                slot.lowpassCutoffHz = cutoff;
            }
        }

        [[nodiscard]] f32 OpenCutoffHz() const
        {
            // "No muffling" = just under Nyquist (a 20 kHz ceiling on high rates).
            return Min(20000.0f, static_cast<f32>(
                                     ma_engine_get_sample_rate(const_cast<ma_engine*>(&engine))) *
                                     0.45f);
        }

        static constexpr u32 kLowpassOrder = 2;

        // ---------------- groups ----------------

        [[nodiscard]] static usize BusIndex(AudioBus bus) noexcept
        {
            return static_cast<usize>(bus) < static_cast<usize>(AudioBus::Count)
                       ? static_cast<usize>(bus)
                       : static_cast<usize>(AudioBus::Effects);
        }

        // A run's effective gain on one of its bus groups: the run's gain for that bus, times its
        // Master gain (once, not twice, on the Master group itself), its volume and its mute ramp.
        [[nodiscard]] static f32 RunGroupGain(const RunGroupData& run, usize busIndex) noexcept
        {
            constexpr usize master = static_cast<usize>(AudioBus::Master);
            const f32 masterGain = run.busMuted[master] ? 0.0f : run.busGain[master];
            if (busIndex == master)
            {
                return masterGain * run.volume * run.muteFactor;
            }
            const f32 bus = run.busMuted[busIndex] ? 0.0f : run.busGain[busIndex];
            return bus * masterGain * run.volume * run.muteFactor;
        }

        // The run-wide part of every gain the run carries: its Master gain, volume and mute ramp.
        [[nodiscard]] static f32 RunVoiceGain(const RunGroupData& run) noexcept
        {
            constexpr usize master = static_cast<usize>(AudioBus::Master);
            return (run.busMuted[master] ? 0.0f : run.busGain[master]) * run.volume * run.muteFactor;
        }

        [[nodiscard]] RunGroupData* FindRun(u64 runGroup) const
        {
            if (runGroup == 0)
            {
                return nullptr;
            }
            RunGroupData* const* data = runGroups.Find(runGroup);
            return data != nullptr ? *data : nullptr;
        }

        // The run's child group under `bus` (lazily made), or the bus itself outside a run.
        [[nodiscard]] ma_sound_group* RunParentFor(u64 runGroup, usize busIndex)
        {
            if (RunGroupData* run = FindRun(runGroup))
            {
                if (!run->initialized[busIndex] && busGroupInitialized[busIndex])
                {
                    run->initialized[busIndex] =
                        ma_sound_group_init(&engine, 0, &busGroups[busIndex],
                                            &run->group[busIndex]) == MA_SUCCESS;
                    if (run->initialized[busIndex])
                    {
                        ma_sound_group_set_volume(&run->group[busIndex], RunGroupGain(*run, busIndex));
                        if (run->paused)
                        {
                            (void)ma_sound_group_stop(&run->group[busIndex]);
                        }
                    }
                }
                if (run->initialized[busIndex])
                {
                    return &run->group[busIndex];
                }
            }
            return busGroupInitialized[busIndex] ? &busGroups[busIndex] : nullptr;
        }

        // A run's gain on its child under custom bus `index`: its own gains down the bus's chain
        // (each custom ancestor's by name, then the fixed bus the chain roots in) under its Master
        // gain, volume and mute ramp, as its children under the fixed buses carry.
        [[nodiscard]] f32 RunCustomGain(const RunGroupData& run, i32 index) const
        {
            f32 gain = 1.0f;
            AudioBus root = AudioBus::Master;
            i32 cursor = index;
            usize steps = 0;
            while (cursor >= 0 && static_cast<usize>(cursor) < customBuses.Size() &&
                   steps++ <= customBuses.Size())
            {
                const CustomBusData& bus = *customBuses[static_cast<usize>(cursor)];
                if (const RunGroupData::NamedGain* named = run.FindNamed(bus.name.AsView()))
                {
                    gain *= named->muted ? 0.0f : named->gain;
                }
                root = bus.fixedParent;
                cursor = bus.parentCustom;
            }
            const usize rootIndex = static_cast<usize>(root);
            if (root != AudioBus::Master)
            {
                gain *= run.busMuted[rootIndex] ? 0.0f : run.busGain[rootIndex];
            }
            return gain * RunVoiceGain(run);
        }

        void ApplyRunGains(u64 runGroup, RunGroupData& run)
        {
            for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
            {
                if (run.initialized[bus])
                {
                    ma_sound_group_set_volume(&run.group[bus], RunGroupGain(run, bus));
                }
            }
            for (usize i = 0; i < customBuses.Size(); ++i)
            {
                if (ma_sound_group* const* child = customBuses[i]->runChildren.Find(runGroup))
                {
                    ma_sound_group_set_volume(*child, RunCustomGain(run, static_cast<i32>(i)));
                }
            }
        }

        // ---- runs' and scenes' children under the custom buses ----

        [[nodiscard]] ma_sound_group* MakeChildGroup(ma_sound_group* parent)
        {
            if (parent == nullptr)
            {
                return nullptr;
            }
            ma_sound_group* child = allocator->New<ma_sound_group>();
            if (ma_sound_group_init(&engine, 0, parent, child) != MA_SUCCESS)
            {
                allocator->Delete(child);
                return nullptr;
            }
            return child;
        }

        void FreeChildGroup(HashMap<u64, ma_sound_group*>& children, u64 key)
        {
            if (ma_sound_group** child = children.Find(key))
            {
                ma_sound_group_uninit(*child);
                allocator->Delete(*child);
                children.Remove(key);
            }
        }

        [[nodiscard]] ma_sound_group* CustomRunChild(u64 runGroup, i32 index)
        {
            CustomBusData& bus = *customBuses[static_cast<usize>(index)];
            RunGroupData* run = FindRun(runGroup);
            if (run == nullptr || !bus.initialized)
            {
                return bus.initialized ? &bus.group : nullptr;
            }
            if (ma_sound_group** found = bus.runChildren.Find(runGroup))
            {
                return *found;
            }
            ma_sound_group* child = MakeChildGroup(&bus.group);
            if (child == nullptr)
            {
                return &bus.group;
            }
            ma_sound_group_set_volume(child, RunCustomGain(*run, index));
            if (run->paused)
            {
                (void)ma_sound_group_stop(child);
            }
            bus.runChildren.InsertOrAssign(runGroup, child);
            return child;
        }

        // The group a voice on custom bus `index` attaches to: its scene's child (under its run's
        // child, or the bus outside a run), else its run's child, else the bus itself.
        [[nodiscard]] ma_sound_group* CustomGroupFor(u64 sceneGroup, u64 runGroup, i32 index)
        {
            CustomBusData& bus = *customBuses[static_cast<usize>(index)];
            if (!bus.initialized)
            {
                return nullptr;
            }
            ma_sound_group* runParent = CustomRunChild(runGroup, index);
            if (sceneGroup == 0)
            {
                return runParent;
            }
            SceneGroupData** sceneData = sceneGroups.Find(sceneGroup);
            if (sceneData == nullptr)
            {
                return runParent;
            }
            if (ma_sound_group** found = bus.sceneChildren.Find(sceneGroup))
            {
                return *found;
            }
            ma_sound_group* child = MakeChildGroup(runParent);
            if (child == nullptr)
            {
                return runParent;
            }
            if ((*sceneData)->paused)
            {
                (void)ma_sound_group_stop(child);
            }
            bus.sceneChildren.InsertOrAssign(sceneGroup, child);
            return child;
        }

        // Pause or resume a set of groups with the same declick a scene group's pause uses.
        void SetGroupPaused(ma_sound_group* group, bool paused)
        {
            if (paused)
            {
                (void)ma_sound_stop_with_fade_in_milliseconds(group, FadeMilliseconds());
            }
            else
            {
                ma_sound_reset_stop_time_and_fade(group);
                ma_sound_set_fade_in_milliseconds(group, 0.0f, 1.0f, FadeMilliseconds());
                (void)ma_sound_group_start(group);
            }
        }

        [[nodiscard]] ma_sound_group* GroupFor(u64 sceneGroup, u64 runGroup, AudioBus bus)
        {
            const usize busIndex = BusIndex(bus);
            if (sceneGroup != 0)
            {
                if (SceneGroupData** data = sceneGroups.Find(sceneGroup))
                {
                    SceneGroupData& groups = **data;
                    if (!groups.initialized[busIndex])
                    {
                        ma_sound_group* parent = RunParentFor(groups.runGroup, busIndex);
                        groups.initialized[busIndex] =
                            parent != nullptr && ma_sound_group_init(&engine, 0, parent,
                                                                     &groups.group[busIndex]) == MA_SUCCESS;
                        if (groups.initialized[busIndex] && groups.paused)
                        {
                            (void)ma_sound_group_stop(&groups.group[busIndex]);
                        }
                    }
                    if (groups.initialized[busIndex])
                    {
                        return &groups.group[busIndex];
                    }
                }
            }
            return RunParentFor(runGroup, busIndex);
        }

        void DestroyRunGroupData(u64 runGroup)
        {
            RunGroupData** data = runGroups.Find(runGroup);
            if (data == nullptr)
            {
                return;
            }
            // Its scenes' groups first: they are children of the run's groups.
            Array<u64> scenes;
            for (auto& entry : sceneGroups)
            {
                if (entry.value->runGroup == runGroup)
                {
                    scenes.PushBack(entry.key);
                }
            }
            for (u64 id : scenes)
            {
                DestroySceneGroupData(id);
            }
            for (VoiceSlot& slot : voices)
            {
                if (slot.state != VoiceState::Free && slot.runGroup == runGroup)
                {
                    ReleaseSlot(slot);
                }
            }
            for (CustomBusData* bus : customBuses)
            {
                FreeChildGroup(bus->runChildren, runGroup);
            }
            for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
            {
                if ((*data)->initialized[bus])
                {
                    ma_sound_group_uninit(&(*data)->group[bus]);
                }
            }
            allocator->Delete(*data);
            runGroups.Remove(runGroup);
        }

        void DestroySceneGroupData(u64 sceneGroup)
        {
            SceneGroupData** data = sceneGroups.Find(sceneGroup);
            if (data == nullptr)
            {
                return;
            }
            for (VoiceSlot& slot : voices)
            {
                if (slot.state != VoiceState::Free && slot.sceneGroup == sceneGroup)
                {
                    ReleaseSlot(slot);
                }
            }
            for (CustomBusData* bus : customBuses)
            {
                FreeChildGroup(bus->sceneChildren, sceneGroup);
            }
            DestroyReverbNode(*allocator, (*data)->reverb);
            DestroyReverbNode(*allocator, (*data)->sendReverb);
            for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
            {
                if ((*data)->initialized[bus])
                {
                    ma_sound_group_uninit(&(*data)->group[bus]);
                }
            }
            allocator->Delete(*data);
            sceneGroups.Remove(sceneGroup);
        }

        [[nodiscard]] u64 FadeMilliseconds() const
        {
            const f32 seconds = settings.stopFadeSeconds > 0.0f ? settings.stopFadeSeconds : 0.0f;
            return static_cast<u64>(seconds * 1000.0f + 0.5f);
        }
    };

    // ---------------- public surface ----------------

    AudioEngine::AudioEngine(IAllocator& allocator, const AudioEngineSettings& settings)
        : m_impl(MakeUnique<Impl>(allocator, allocator, settings))
    {
    }

    AudioEngine::~AudioEngine() = default;

    bool AudioEngine::IsHeadless() const noexcept { return m_impl->headless; }

    void AudioEngine::Update(f32 deltaTime)
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized)
        {
            return;
        }
        if (deltaTime < 0.0f)
        {
            deltaTime = 0.0f;
        }
        if (deltaTime > 0.25f)
        {
            deltaTime = 0.25f;
        } // hitch clamp
        impl.timeSeconds += deltaTime;

        // Run mute ramps: a muted run fades out (and an unmuted one back in) over a short window
        // instead of clicking, so moving the audible Game tab cross-fades.
        const f32 rampStep = deltaTime / kRunMuteFadeSeconds;
        for (auto& entry : impl.runGroups)
        {
            RunGroupData& run = *entry.value;
            const f32 target = run.muted ? 0.0f : 1.0f;
            if (run.muteFactor != target)
            {
                run.muteFactor = (run.muteFactor < target) ? Min(run.muteFactor + rampStep, target)
                                                           : Max(run.muteFactor - rampStep, target);
                impl.ApplyRunGains(entry.key, run);
            }
        }

        if (impl.headless && deltaTime > 0.0f)
        {
            // Pump the mixer manually: headless mode advances in game time.
            const u32 channels = ma_engine_get_channels(&impl.engine);
            const u32 sampleRate = ma_engine_get_sample_rate(&impl.engine);
            u64 remaining = static_cast<u64>(static_cast<f64>(deltaTime) * sampleRate);
            constexpr u64 kChunkFrames = 1024;
            if (impl.pumpScratch.Size() < kChunkFrames * channels)
            {
                impl.pumpScratch.Resize(kChunkFrames * channels);
            }
            while (remaining > 0)
            {
                const u64 frames = remaining < kChunkFrames ? remaining : kChunkFrames;
                ma_uint64 read = 0;
                if (ma_engine_read_pcm_frames(&impl.engine, impl.pumpScratch.Data(), frames,
                                              &read) != MA_SUCCESS ||
                    read == 0)
                {
                    break;
                }
                remaining -= read;
            }
        }

        // Dying voices (faded steal): reap the ones whose fade completed.
        for (usize i = impl.dyingVoices.Size(); i > 0; --i)
        {
            if (ma_sound_is_playing(impl.dyingVoices[i - 1].sound) == MA_FALSE)
            {
                impl.ReapDyingVoice(i - 1);
            }
        }

        // Reap: fades that completed, one-shots that reached their end. The same walk
        // glides every filtered voice's distance low-pass (covers one-shots and a
        // moving listener: voices the scene sync never repositions).
        for (VoiceSlot& slot : impl.voices)
        {
            if (slot.state != VoiceState::Free)
            {
                impl.UpdateVoiceLowpass(slot);
            }
            // Eased volume and pitch: held while the voice is paused, so a pause keeps its place.
            if (slot.state == VoiceState::Playing || slot.state == VoiceState::Stopping)
            {
                if (slot.volumeRamp.active)
                {
                    slot.volume = slot.volumeRamp.Advance(deltaTime);
                    const AudioClip* clip = slot.clip.Get();
                    ma_sound_set_volume(slot.sound, slot.volume * (clip != nullptr ? clip->gain : 1.0f));
                }
                if (slot.pitchRamp.active)
                {
                    slot.pitch = slot.pitchRamp.Advance(deltaTime);
                    ma_sound_set_pitch(slot.sound, slot.pitch);
                }
            }
            if (slot.state == VoiceState::Stopping)
            {
                if (ma_sound_is_playing(slot.sound) == MA_FALSE)
                {
                    impl.ReleaseSlot(slot);
                }
            }
            else if (slot.state == VoiceState::Playing && !slot.looping &&
                     ma_sound_at_end(slot.sound) == MA_TRUE)
            {
                impl.ReleaseSlot(slot);
            }
        }
    }

    VoiceHandle AudioEngine::Play(const RefPtr<AudioClip>& clip, const AudioPlayParams& params)
    {
        Impl& impl = *m_impl;
        AudioClip* clipPtr = clip.Get();
        if (!impl.engineInitialized || clipPtr == nullptr)
        {
            return {};
        }
        if (!clipPtr->stream && clipPtr->encodedData.IsEmpty())
        {
            return {};
        }
        if (clipPtr->stream && clipPtr->streamSource.Get() == nullptr)
        {
            return {};
        }

        // Recent-play dedupe: a same-clip play inside the window merges into the
        // existing voice instead of stacking (shotgun pellets, particle bursts).
        // Opt-out plays (persistent component sources) neither merge nor arm the
        // window: a persistent voice must not swallow later legitimate one-shots.
        if (params.allowDedupe)
        {
            if (DedupeEntry* recent = impl.recentPlays.Find(clipPtr))
            {
                if (impl.timeSeconds - recent->time < impl.settings.dedupeWindowSeconds &&
                    impl.Resolve(recent->handle) != nullptr)
                {
                    return recent->handle;
                }
            }
        }

        const bool wantMonoDownmix = params.spatial && clipPtr->channels > 1 && !clipPtr->stream;
        if (params.spatial && clipPtr->channels > 1)
        {
            // Mono guard: 3D wants mono, so warn once and downmix in-memory clips;
            // streamed stereo spatializes as is (reimport it with force-mono).
            if (clipPtr->stream)
            {
                if (!impl.warnedStreamStereoSpatial)
                {
                    impl.warnedStreamStereoSpatial = true;
                    LOG_WARNING(
                        u8"Audio", u8"spatializing a STREAMED multi-channel clip - reimport with "
                                   u8"force-mono for correct 3D imaging (warned once)");
                }
            }
            else if (!impl.warnedMonoDownmix)
            {
                impl.warnedMonoDownmix = true;
                LOG_WARNING(
                    u8"Audio",
                    u8"spatializing a multi-channel clip - downmixing to mono at play; "
                    u8"reimport with force-mono to avoid the runtime cost (warned once)");
            }
        }

        char name[64];
        bool useMonoVariant = false;
        if (clipPtr->stream)
        {
            impl.streamClips.InsertOrAssign(clipPtr, clip);
            FormatClipName(name, sizeof(name), "dstream:", clipPtr);
        }
        else
        {
            RegisteredClip* registered = impl.EnsureRegistered(clip, wantMonoDownmix);
            if (registered == nullptr)
            {
                return {};
            }
            useMonoVariant = wantMonoDownmix && registered->monoRegistered;
            FormatClipName(name, sizeof(name), useMonoVariant ? "dclipm:" : "dclip:", clipPtr);
        }

        const usize slotIndex = impl.AcquireSlot(clipPtr->stream, params.priority);
        if (slotIndex >= impl.voices.Size())
        {
            return {};
        } // pool full of higher priority
        VoiceSlot& slot = impl.voices[slotIndex];
        slot.sound = impl.AcquireSound();
        if (slot.sound == nullptr)
        {
            return {};
        } // unreachable: the arena covers the pool and the dying list

        // Named-bus addressing: a known custom bus overrides the enum bus. Unknown
        // names warn once and fall back, so a typo in content never silences a game.
        // A scene group's voice belongs to the scene's run; a scene-less voice to its own.
        u64 runGroup = params.runGroup;
        if (params.sceneGroup != 0)
        {
            SceneGroupData** sceneData = impl.sceneGroups.Find(params.sceneGroup);
            runGroup = (sceneData != nullptr) ? (*sceneData)->runGroup : 0;
        }
        ma_sound_group* group = impl.GroupFor(params.sceneGroup, runGroup, params.bus);
        i32 customBusIndex = -1;
        if (!params.busName.IsEmpty())
        {
            customBusIndex = impl.FindCustomBus(params.busName.AsView());
            if (customBusIndex >= 0 &&
                impl.customBuses[static_cast<usize>(customBusIndex)]->initialized)
            {
                group = impl.CustomGroupFor(params.sceneGroup, runGroup, customBusIndex);
            }
            else
            {
                customBusIndex = -1;
                if (!impl.warnedUnknownBusName)
                {
                    impl.warnedUnknownBusName = true;
                    LOG_WARNING(
                        u8"Audio",
                        u8"play addressed unknown custom bus '{}' - using the fixed bus "
                        u8"(warned once)",
                        params.busName);
                }
            }
        }
        const ma_uint32 flags = clipPtr->stream ? MA_SOUND_FLAG_STREAM : 0;
        if (ma_sound_init_from_file(&impl.engine, name, flags, group, nullptr, slot.sound) !=
            MA_SUCCESS)
        {
            impl.ReleaseSound(slot.sound);
            slot.sound = nullptr;
            ++slot.generation;
            LOG_WARNING(u8"Audio", u8"voice init failed for clip");
            return {};
        }
        slot.soundInitialized = true;
        slot.state = params.startPaused ? VoiceState::Paused : VoiceState::Playing;
        slot.priority = params.priority;
        slot.spatial = params.spatial;
        slot.position = params.position;
        slot.volume = params.volume;
        slot.pitch = params.pitch;
        slot.volumeRamp = {};
        slot.pitchRamp = {};
        slot.bus = params.bus;
        slot.customBusName = customBusIndex >= 0 ? String(params.busName.AsView()) : String{};
        slot.sceneGroup = params.sceneGroup;
        slot.runGroup = runGroup;
        slot.clip = clip;
        slot.looping = params.loop || clipPtr->loop;

        // Distance low-pass: splice an ma_lpf between the sound and its group. Wholly
        // per-voice; 0 Hz = no node (the sound feeds its group directly).
        if (params.spatial && params.distanceLowpassHz > 0.0f && group != nullptr)
        {
            auto* node = impl.allocator->New<ma_lpf_node>();
            ma_lpf_node_config config = ma_lpf_node_config_init(
                ma_engine_get_channels(&impl.engine), ma_engine_get_sample_rate(&impl.engine),
                impl.OpenCutoffHz(), Impl::kLowpassOrder);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&impl.engine), &config, nullptr, node) ==
                    MA_SUCCESS &&
                ma_node_attach_output_bus(node, 0, group, 0) == MA_SUCCESS &&
                ma_node_attach_output_bus(slot.sound, 0, node, 0) == MA_SUCCESS)
            {
                slot.lowpassNode = node;
                slot.lowpassFloorHz = params.distanceLowpassHz;
                slot.lowpassMinDistance = params.minDistance;
                slot.lowpassMaxDistance = params.maxDistance;
                slot.lowpassCutoffHz = 0.0f; // forces the first mapping to land
            }
            else
            {
                ma_lpf_node_uninit(node, nullptr);
                impl.allocator->Delete(node);
            }
        }

        // Per-voice reverb send: splice a splitter at the end of the chain. Out 0
        // stays the dry route into the group; out 1 (volume = reverbSend) feeds the
        // scene's wet-only send reverb in parallel. Scene-group voices only (the send
        // reverb is per-scene); failure just plays dry.
        if (params.reverbSend > 0.0f && params.sceneGroup != 0 && group != nullptr)
        {
            if (ReverbNode* sendReverb = impl.EnsureSceneSendReverb(params.sceneGroup))
            {
                const f32 send = Clamp(params.reverbSend, 0.0f, 1.0f);
                ma_node* tail = slot.lowpassNode != nullptr
                                    ? reinterpret_cast<ma_node*>(slot.lowpassNode)
                                    : reinterpret_cast<ma_node*>(slot.sound);
                auto* splitter = impl.allocator->New<ma_splitter_node>();
                ma_splitter_node_config config =
                    ma_splitter_node_config_init(ma_engine_get_channels(&impl.engine));
                if (ma_splitter_node_init(ma_engine_get_node_graph(&impl.engine), &config, nullptr,
                                          splitter) != MA_SUCCESS)
                {
                    impl.allocator->Delete(splitter);
                }
                else if (ma_node_attach_output_bus(splitter, 0, group, 0) == MA_SUCCESS &&
                         ma_node_attach_output_bus(splitter, 1, sendReverb, 0) == MA_SUCCESS &&
                         ma_node_attach_output_bus(tail, 0, splitter, 0) == MA_SUCCESS)
                {
                    (void)ma_node_set_output_bus_volume(splitter, 1, send);
                    slot.splitterNode = splitter;
                    slot.reverbSend = send;
                }
                else
                {
                    ma_splitter_node_uninit(splitter, nullptr);
                    impl.allocator->Delete(splitter);
                }
            }
        }

        ma_sound_set_volume(slot.sound, params.volume * clipPtr->gain);
        ma_sound_set_pitch(slot.sound, params.pitch);
        ma_sound_set_looping(slot.sound, slot.looping ? MA_TRUE : MA_FALSE);
        if (slot.looping && (clipPtr->loopStartFrame > 0 || clipPtr->loopEndFrame > 0))
        {
            const u64 loopEnd =
                clipPtr->loopEndFrame > 0 ? clipPtr->loopEndFrame : clipPtr->frameCount;
            (void)ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(slot.sound),
                                                              clipPtr->loopStartFrame, loopEnd);
        }

        if (params.spatial)
        {
            ma_sound_set_spatialization_enabled(slot.sound, MA_TRUE);
            ma_sound_set_positioning(slot.sound, ma_positioning_absolute);
            ma_sound_set_position(slot.sound, params.position.x, params.position.y,
                                  params.position.z);
            ma_sound_set_velocity(slot.sound, params.velocity.x, params.velocity.y,
                                  params.velocity.z);
            ma_sound_set_attenuation_model(slot.sound, ToMiniaudio(params.attenuationModel));
            ma_sound_set_min_distance(slot.sound, params.minDistance);
            ma_sound_set_max_distance(slot.sound, params.maxDistance);
            ma_sound_set_rolloff(slot.sound, params.rolloff);
            ma_sound_set_doppler_factor(slot.sound, params.dopplerFactor);
            if (params.coneInnerAngleDegrees < 360.0f || params.coneOuterAngleDegrees < 360.0f)
            {
                ma_sound_set_cone(slot.sound, params.coneInnerAngleDegrees * kDegreesToRadians,
                                  params.coneOuterAngleDegrees * kDegreesToRadians,
                                  params.coneOuterGain);
            }
        }
        else
        {
            ma_sound_set_spatialization_enabled(slot.sound, MA_FALSE);
            ma_sound_set_pan(slot.sound, params.pan);
        }

        // A paused scene or run halts its group nodes, custom-bus children included, so a voice
        // started beneath one waits in place like every other voice there.
        if (!params.startPaused)
        {
            (void)ma_sound_start(slot.sound);
        }

        const VoiceHandle handle = impl.HandleFor(slotIndex);
        if (params.allowDedupe)
        {
            impl.recentPlays.InsertOrAssign(static_cast<void*>(clipPtr),
                                            DedupeEntry{impl.timeSeconds, handle});
        }
        (void)useMonoVariant;
        return handle;
    }

    void AudioEngine::Stop(VoiceHandle handle)
    {
        Stop(handle, 0.0f);
    }

    void AudioEngine::Stop(VoiceHandle handle, f32 fadeSeconds)
    {
        VoiceSlot* slot = m_impl->Resolve(handle);
        if (slot == nullptr)
        {
            return;
        }
        if (slot->state == VoiceState::Paused)
        {
            m_impl->ReleaseSlot(*slot); // already silent: no fade needed
            return;
        }
        if (slot->state != VoiceState::Stopping)
        {
            const u64 fadeMs = Max(static_cast<u64>(Max(fadeSeconds, 0.0f) * 1000.0f + 0.5f),
                                   m_impl->FadeMilliseconds());
            (void)ma_sound_stop_with_fade_in_milliseconds(slot->sound, fadeMs);
            slot->state = VoiceState::Stopping;
        }
    }

    // ---- music: one tracked voice on the Music bus, cross-faded ----

    VoiceHandle AudioEngine::PlayMusic(const RefPtr<AudioClip>& clip, f32 crossFadeSeconds,
                                       f32 volume, u64 runGroup)
    {
        Impl& impl = *m_impl;
        const u64 fadeMs = static_cast<u64>(Max(crossFadeSeconds, 0.0f) * 1000.0f + 0.5f);
        RunGroupData* run = impl.FindRun(runGroup);
        VoiceHandle& slotHandle = (run != nullptr) ? run->musicVoice : impl.musicVoice;

        // Fade the incumbent out over the same window the newcomer fades in.
        if (VoiceSlot* current = impl.Resolve(slotHandle))
        {
            if (current->state != VoiceState::Stopping)
            {
                (void)ma_sound_stop_with_fade_in_milliseconds(current->sound, fadeMs);
                current->state = VoiceState::Stopping;
            }
        }
        slotHandle = VoiceHandle{};

        AudioPlayParams params;
        params.bus = AudioBus::Music;
        params.loop = true; // music loops unless the clip says otherwise anyway
        params.volume = volume;
        params.allowDedupe = false; // replaying the same track restarts it
        params.runGroup = (run != nullptr) ? runGroup : 0;
        const VoiceHandle handle = Play(clip, params);
        if (VoiceSlot* slot = impl.Resolve(handle); slot != nullptr && fadeMs > 0)
        {
            ma_sound_set_fade_in_milliseconds(slot->sound, 0.0f, 1.0f, fadeMs);
        }
        slotHandle = handle; // run data is heap-held: Play never moves it
        return handle;
    }

    void AudioEngine::StopMusic(f32 fadeSeconds, u64 runGroup)
    {
        Impl& impl = *m_impl;
        RunGroupData* run = impl.FindRun(runGroup);
        VoiceHandle& slotHandle = (run != nullptr) ? run->musicVoice : impl.musicVoice;
        if (VoiceSlot* slot = impl.Resolve(slotHandle))
        {
            if (slot->state != VoiceState::Stopping)
            {
                const u64 fadeMs = static_cast<u64>(Max(fadeSeconds, 0.0f) * 1000.0f + 0.5f);
                (void)ma_sound_stop_with_fade_in_milliseconds(slot->sound, fadeMs);
                slot->state = VoiceState::Stopping;
            }
        }
        slotHandle = VoiceHandle{};
    }

    VoiceHandle AudioEngine::MusicVoice(u64 runGroup) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        return (run != nullptr) ? run->musicVoice : m_impl->musicVoice;
    }

    // ---- bus layout ----

    void AudioEngine::ApplyBusLayout(const AudioBusLayout& layout)
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized)
        {
            return;
        }
        for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
        {
            const AudioBusSettings& settings = layout.buses[bus];
            SetBusVolume(static_cast<AudioBus>(bus), settings.volume);
            SetBusMuted(static_cast<AudioBus>(bus), settings.muted);
            impl.BuildBusEffects(bus, Span<const AudioBusEffectDesc>(settings.effects.Data(),
                                                                     settings.effects.Size()));
        }
        impl.RebuildCustomBuses(
            Span<const AudioNamedBus>(layout.customBuses.Data(), layout.customBuses.Size()));
    }

    // ---- named custom buses ----

    bool AudioEngine::HasNamedBus(StringView name) const
    {
        return m_impl->FindCustomBus(name) >= 0;
    }

    u32 AudioEngine::NamedBusCount() const { return static_cast<u32>(m_impl->customBuses.Size()); }

    void AudioEngine::SetNamedBusVolume(StringView name, f32 volume)
    {
        const i32 index = m_impl->FindCustomBus(name);
        if (index < 0)
        {
            return;
        }
        CustomBusData& bus = *m_impl->customBuses[static_cast<usize>(index)];
        bus.volume = volume < 0.0f ? 0.0f : volume;
        if (bus.initialized && !bus.muted)
        {
            ma_sound_group_set_volume(&bus.group, bus.volume);
        }
    }

    f32 AudioEngine::NamedBusVolume(StringView name) const
    {
        const i32 index = m_impl->FindCustomBus(name);
        return index >= 0 ? m_impl->customBuses[static_cast<usize>(index)]->volume : 0.0f;
    }

    void AudioEngine::SetNamedBusMuted(StringView name, bool muted)
    {
        const i32 index = m_impl->FindCustomBus(name);
        if (index < 0)
        {
            return;
        }
        CustomBusData& bus = *m_impl->customBuses[static_cast<usize>(index)];
        bus.muted = muted;
        if (bus.initialized)
        {
            ma_sound_group_set_volume(&bus.group, muted ? 0.0f : bus.volume);
        }
    }

    bool AudioEngine::NamedBusMuted(StringView name) const
    {
        const i32 index = m_impl->FindCustomBus(name);
        return index >= 0 && m_impl->customBuses[static_cast<usize>(index)]->muted;
    }

    u32 AudioEngine::NamedBusEffectCount(StringView name) const
    {
        const i32 index = m_impl->FindCustomBus(name);
        return index >= 0 ? static_cast<u32>(
                                m_impl->customBuses[static_cast<usize>(index)]->effects.Size())
                          : 0u;
    }

    u32 AudioEngine::BusEffectCount(AudioBus bus) const
    {
        const usize index = static_cast<usize>(bus);
        if (index >= static_cast<usize>(AudioBus::Count))
        {
            return 0;
        }
        return static_cast<u32>(m_impl->busEffects[index].Size());
    }

    void AudioEngine::StopAll()
    {
        for (usize i = 0; i < m_impl->voices.Size(); ++i)
        {
            if (m_impl->voices[i].state != VoiceState::Free)
            {
                Stop(m_impl->HandleFor(i));
            }
        }
    }

    void AudioEngine::SetPaused(VoiceHandle handle, bool paused)
    {
        VoiceSlot* slot = m_impl->Resolve(handle);
        if (slot == nullptr || slot->state == VoiceState::Stopping)
        {
            return;
        }
        if (paused && slot->state == VoiceState::Playing)
        {
            (void)ma_sound_stop_with_fade_in_milliseconds(slot->sound, m_impl->FadeMilliseconds());
            slot->state = VoiceState::Paused;
        }
        else if (!paused && slot->state == VoiceState::Paused)
        {
            ma_sound_reset_stop_time_and_fade(slot->sound);
            ma_sound_set_fade_in_milliseconds(slot->sound, 0.0f, 1.0f, m_impl->FadeMilliseconds());
            (void)ma_sound_start(slot->sound);
            slot->state = VoiceState::Playing;
        }
    }

    bool AudioEngine::IsPlaying(VoiceHandle handle) const
    {
        const VoiceSlot* slot = m_impl->Resolve(handle);
        return slot != nullptr && slot->state == VoiceState::Playing;
    }

    bool AudioEngine::IsValidHandle(VoiceHandle handle) const
    {
        return m_impl->Resolve(handle) != nullptr;
    }

    bool AudioEngine::GetVoiceStatus(VoiceHandle handle, VoiceStatus& out) const
    {
        const VoiceSlot* slot = m_impl->Resolve(handle);
        if (slot == nullptr)
        {
            out = VoiceStatus{};
            return false;
        }
        out.active = true;
        out.playing = slot->state == VoiceState::Playing;
        out.paused = slot->state == VoiceState::Paused;
        out.stopping = slot->state == VoiceState::Stopping;
        out.spatial = slot->spatial;
        out.volume = slot->volume;
        out.pitch = slot->pitch;
        out.bus = slot->bus;
        out.busName = String(slot->customBusName.AsView());
        out.priority = slot->priority;
        out.position = slot->position;
        out.lowpassCutoffHz = slot->lowpassCutoffHz;
        out.cursorSeconds = 0.0f;
        if (slot->soundInitialized)
        {
            float cursor = 0.0f;
            if (ma_sound_get_cursor_in_seconds(slot->sound, &cursor) == MA_SUCCESS)
            {
                out.cursorSeconds = cursor;
            }
        }
        out.reverbSend = slot->reverbSend;
        return true;
    }

    void AudioEngine::SetVoiceReverbSend(VoiceHandle handle, f32 send)
    {
        VoiceSlot* slot = m_impl->Resolve(handle);
        if (slot == nullptr || slot->splitterNode == nullptr)
        {
            return;
        }
        slot->reverbSend = Clamp(send, 0.0f, 1.0f);
        (void)ma_node_set_output_bus_volume(slot->splitterNode, 1, slot->reverbSend);
    }

    void AudioEngine::SetVoiceVolume(VoiceHandle handle, f32 volume, f32 seconds)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            volume = Max(volume, 0.0f);
            if (seconds > 0.0f)
            {
                slot->volumeRamp.Start(slot->volume, volume, seconds);
                return;
            }
            slot->volumeRamp = {};
            slot->volume = volume;
            const AudioClip* clip = slot->clip.Get();
            ma_sound_set_volume(slot->sound, volume * (clip != nullptr ? clip->gain : 1.0f));
        }
    }

    void AudioEngine::SetVoicePitch(VoiceHandle handle, f32 pitch, f32 seconds)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            pitch = Max(pitch, 0.01f); // a rate of 0 would stall the voice for good
            if (seconds > 0.0f)
            {
                slot->pitchRamp.Start(slot->pitch, pitch, seconds);
                return;
            }
            slot->pitchRamp = {};
            slot->pitch = pitch;
            ma_sound_set_pitch(slot->sound, pitch);
        }
    }

    void AudioEngine::SetVoicePan(VoiceHandle handle, f32 pan)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            ma_sound_set_pan(slot->sound, pan);
        }
    }

    void AudioEngine::SetVoiceLooping(VoiceHandle handle, bool loop)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            slot->looping = loop;
            ma_sound_set_looping(slot->sound, loop ? MA_TRUE : MA_FALSE);
        }
    }

    void AudioEngine::SetVoicePosition(VoiceHandle handle, Float3 position, Float3 velocity)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            slot->position = position;
            ma_sound_set_position(slot->sound, position.x, position.y, position.z);
            ma_sound_set_velocity(slot->sound, velocity.x, velocity.y, velocity.z);
        }
    }

    usize AudioEngine::ActiveVoiceCount() const
    {
        usize count = 0;
        for (const VoiceSlot& slot : m_impl->voices)
        {
            if (slot.state != VoiceState::Free)
            {
                ++count;
            }
        }
        return count;
    }

    usize AudioEngine::DyingVoiceCount() const { return m_impl->dyingVoices.Size(); }

    void AudioEngine::SetListenerTransform(Float3 position, Float3 forward, Float3 up,
                                           Float3 velocity)
    {
        SetListenerTransformIndexed(0, position, forward, up, velocity);
    }

    void AudioEngine::SetListenerTransformIndexed(u32 index, Float3 position, Float3 forward,
                                                  Float3 up, Float3 velocity)
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized || index >= ma_engine_get_listener_count(&impl.engine))
        {
            return;
        }
        // The steal-farthest heuristic and the distance low-pass track listener 0 (primary).
        if (index == 0)
        {
            impl.listenerPosition = position;
        }
        ma_engine_listener_set_position(&impl.engine, index, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&impl.engine, index, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&impl.engine, index, up.x, up.y, up.z);
        ma_engine_listener_set_velocity(&impl.engine, index, velocity.x, velocity.y, velocity.z);
    }

    void AudioEngine::SetListenerEnabled(u32 index, bool enabled)
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized || index >= ma_engine_get_listener_count(&impl.engine))
        {
            return;
        }
        ma_engine_listener_set_enabled(&impl.engine, index, enabled ? MA_TRUE : MA_FALSE);
    }

    u32 AudioEngine::ListenerCount() const
    {
        return m_impl->engineInitialized ? ma_engine_get_listener_count(&m_impl->engine) : 0;
    }

    void AudioEngine::SetMasterVolume(f32 volume)
    {
        Impl& impl = *m_impl;
        impl.masterVolume = volume < 0.0f ? 0.0f : volume;
        if (impl.engineInitialized)
        {
            ma_engine_set_volume(&impl.engine, impl.masterVolume);
        }
    }

    f32 AudioEngine::MasterVolume() const { return m_impl->masterVolume; }

    void AudioEngine::SetBusVolume(AudioBus bus, f32 volume)
    {
        Impl& impl = *m_impl;
        const usize index = static_cast<usize>(bus);
        if (index >= static_cast<usize>(AudioBus::Count))
        {
            return;
        }
        impl.busVolume[index] = volume < 0.0f ? 0.0f : volume;
        if (impl.busGroupInitialized[index] && !impl.busMuted[index])
        {
            ma_sound_group_set_volume(&impl.busGroups[index], impl.busVolume[index]);
        }
    }

    f32 AudioEngine::BusVolume(AudioBus bus) const
    {
        const usize index = static_cast<usize>(bus);
        return index < static_cast<usize>(AudioBus::Count) ? m_impl->busVolume[index] : 0.0f;
    }

    void AudioEngine::SetBusMuted(AudioBus bus, bool muted)
    {
        Impl& impl = *m_impl;
        const usize index = static_cast<usize>(bus);
        if (index >= static_cast<usize>(AudioBus::Count))
        {
            return;
        }
        impl.busMuted[index] = muted;
        if (impl.busGroupInitialized[index])
        {
            ma_sound_group_set_volume(&impl.busGroups[index], muted ? 0.0f : impl.busVolume[index]);
        }
    }

    bool AudioEngine::BusMuted(AudioBus bus) const
    {
        const usize index = static_cast<usize>(bus);
        return index < static_cast<usize>(AudioBus::Count) && m_impl->busMuted[index];
    }

    u64 AudioEngine::CreateSceneGroup(u64 runGroup)
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized)
        {
            return 0;
        }
        const u64 id = impl.nextSceneGroupId++;
        SceneGroupData* data = impl.allocator->New<SceneGroupData>();
        data->runGroup = (impl.FindRun(runGroup) != nullptr) ? runGroup : 0;
        impl.sceneGroups.InsertOrAssign(id, data);
        return id;
    }

    // ---------------- run groups ----------------

    u64 AudioEngine::CreateRunGroup()
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized)
        {
            return 0;
        }
        const u64 id = impl.nextRunGroupId++;
        impl.runGroups.InsertOrAssign(id, impl.allocator->New<RunGroupData>());
        return id;
    }

    void AudioEngine::DestroyRunGroup(u64 runGroup)
    {
        if (runGroup != 0)
        {
            m_impl->DestroyRunGroupData(runGroup);
        }
    }

    void AudioEngine::StopRunGroup(u64 runGroup, f32 fadeSeconds)
    {
        Impl& impl = *m_impl;
        RunGroupData* run = impl.FindRun(runGroup);
        if (run == nullptr)
        {
            return;
        }
        const u64 fadeMs = static_cast<u64>(Max(fadeSeconds, 0.0f) * 1000.0f + 0.5f);
        for (VoiceSlot& slot : impl.voices)
        {
            if (slot.state == VoiceState::Free || slot.runGroup != runGroup)
            {
                continue;
            }
            if (slot.state == VoiceState::Paused)
            {
                impl.ReleaseSlot(slot); // silent already
            }
            else if (slot.state != VoiceState::Stopping)
            {
                (void)ma_sound_stop_with_fade_in_milliseconds(slot.sound, fadeMs);
                slot.state = VoiceState::Stopping;
            }
        }
        run->musicVoice = VoiceHandle{};
        // A paused run's group nodes are halted, so its voices' fades would never land: resume
        // the groups (silently, the voices are fading) so the reap can finish.
        if (run->paused)
        {
            SetRunGroupPaused(runGroup, false);
        }
    }

    void AudioEngine::SetRunGroupPaused(u64 runGroup, bool paused)
    {
        Impl& impl = *m_impl;
        RunGroupData* run = impl.FindRun(runGroup);
        if (run == nullptr || run->paused == paused)
        {
            return;
        }
        run->paused = paused;
        const u64 fadeMs = impl.FadeMilliseconds();
        for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
        {
            if (!run->initialized[bus])
            {
                continue;
            }
            ma_sound_group* group = &run->group[bus];
            if (paused)
            {
                // Halting the run's group node freezes every voice beneath it in place, its
                // scenes' included (the same declick as a scene group's pause).
                (void)ma_sound_stop_with_fade_in_milliseconds(group, fadeMs);
            }
            else
            {
                ma_sound_reset_stop_time_and_fade(group);
                ma_sound_set_fade_in_milliseconds(group, 0.0f, 1.0f, fadeMs);
                (void)ma_sound_group_start(group);
            }
        }
        // Its children under the custom buses halt the same way.
        for (CustomBusData* bus : impl.customBuses)
        {
            if (ma_sound_group** child = bus->runChildren.Find(runGroup))
            {
                impl.SetGroupPaused(*child, paused);
            }
        }
    }

    bool AudioEngine::IsRunGroupPaused(u64 runGroup) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        return run != nullptr && run->paused;
    }

    void AudioEngine::SetRunGroupMuted(u64 runGroup, bool muted)
    {
        if (RunGroupData* run = m_impl->FindRun(runGroup))
        {
            run->muted = muted; // Update ramps muteFactor toward it
        }
    }

    bool AudioEngine::IsRunGroupMuted(u64 runGroup) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        return run != nullptr && run->muted;
    }

    void AudioEngine::SetRunGroupVolume(u64 runGroup, f32 volume)
    {
        if (RunGroupData* run = m_impl->FindRun(runGroup))
        {
            run->volume = Max(volume, 0.0f);
            m_impl->ApplyRunGains(runGroup, *run);
        }
    }

    void AudioEngine::SetRunBusVolume(u64 runGroup, AudioBus bus, f32 volume)
    {
        RunGroupData* run = m_impl->FindRun(runGroup);
        if (run == nullptr || static_cast<usize>(bus) >= static_cast<usize>(AudioBus::Count))
        {
            return;
        }
        run->busGain[static_cast<usize>(bus)] = Max(volume, 0.0f);
        m_impl->ApplyRunGains(runGroup, *run);
    }

    f32 AudioEngine::RunBusVolume(u64 runGroup, AudioBus bus) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        if (run == nullptr || static_cast<usize>(bus) >= static_cast<usize>(AudioBus::Count))
        {
            return 1.0f;
        }
        return run->busGain[static_cast<usize>(bus)];
    }

    void AudioEngine::SetRunBusMuted(u64 runGroup, AudioBus bus, bool muted)
    {
        RunGroupData* run = m_impl->FindRun(runGroup);
        if (run == nullptr || static_cast<usize>(bus) >= static_cast<usize>(AudioBus::Count))
        {
            return;
        }
        run->busMuted[static_cast<usize>(bus)] = muted;
        m_impl->ApplyRunGains(runGroup, *run);
    }

    bool AudioEngine::RunBusMuted(u64 runGroup, AudioBus bus) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        return run != nullptr && static_cast<usize>(bus) < static_cast<usize>(AudioBus::Count) &&
               run->busMuted[static_cast<usize>(bus)];
    }

    void AudioEngine::SetRunNamedBusVolume(u64 runGroup, StringView name, f32 volume)
    {
        if (RunGroupData* run = m_impl->FindRun(runGroup))
        {
            run->EnsureNamed(name).gain = Max(volume, 0.0f);
            m_impl->ApplyRunGains(runGroup, *run);
        }
    }

    f32 AudioEngine::RunNamedBusVolume(u64 runGroup, StringView name) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        const RunGroupData::NamedGain* named = run != nullptr ? run->FindNamed(name) : nullptr;
        return named != nullptr ? named->gain : 1.0f;
    }

    void AudioEngine::SetRunNamedBusMuted(u64 runGroup, StringView name, bool muted)
    {
        if (RunGroupData* run = m_impl->FindRun(runGroup))
        {
            run->EnsureNamed(name).muted = muted;
            m_impl->ApplyRunGains(runGroup, *run);
        }
    }

    bool AudioEngine::RunNamedBusMuted(u64 runGroup, StringView name) const
    {
        const RunGroupData* run = m_impl->FindRun(runGroup);
        const RunGroupData::NamedGain* named = run != nullptr ? run->FindNamed(name) : nullptr;
        return named != nullptr && named->muted;
    }

    void AudioEngine::DestroySceneGroup(u64 sceneGroup)
    {
        if (sceneGroup != 0)
        {
            m_impl->DestroySceneGroupData(sceneGroup);
        }
    }

    void AudioEngine::SetSceneGroupPaused(u64 sceneGroup, bool paused)
    {
        Impl& impl = *m_impl;
        SceneGroupData** data = impl.sceneGroups.Find(sceneGroup);
        if (data == nullptr || (*data)->paused == paused)
        {
            return;
        }
        (*data)->paused = paused;
        for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
        {
            if (!(*data)->initialized[bus])
            {
                continue;
            }
            ma_sound_group* group = &(*data)->group[bus];
            if (paused)
            {
                // Groups are sounds: the same fade-then-stop declick applies. Halting
                // the group node freezes every voice routed through it in place.
                (void)ma_sound_stop_with_fade_in_milliseconds(group, impl.FadeMilliseconds());
            }
            else
            {
                ma_sound_reset_stop_time_and_fade(group);
                ma_sound_set_fade_in_milliseconds(group, 0.0f, 1.0f, impl.FadeMilliseconds());
                (void)ma_sound_group_start(group);
            }
        }
        // Its children under the custom buses halt the same way (a voice on a named bus routes
        // through the scene's child of that bus).
        for (CustomBusData* bus : impl.customBuses)
        {
            if (ma_sound_group** child = bus->sceneChildren.Find(sceneGroup))
            {
                impl.SetGroupPaused(*child, paused);
            }
        }
    }

    bool AudioEngine::IsSceneGroupPaused(u64 sceneGroup) const
    {
        SceneGroupData** data = m_impl->sceneGroups.Find(sceneGroup);
        return data != nullptr && (*data)->paused;
    }

    void AudioEngine::SetSceneReverb(u64 sceneGroup, const AudioReverbParams& params)
    {
        Impl& impl = *m_impl;
        SceneGroupData** found = impl.sceneGroups.Find(sceneGroup);
        if (found == nullptr)
        {
            return;
        }
        SceneGroupData& data = **found;
        if (data.reverb == nullptr)
        {
            if (params.wet <= 0.0f)
            {
                data.reverbWet = 0.0f;
                return;
            } // nothing to build
            // Splice on the scene's Effects child group: the group feeds the reverb, which
            // feeds the Effects bus.
            ma_sound_group* group = impl.GroupFor(sceneGroup, 0, AudioBus::Effects);
            if (group == nullptr || group == &impl.busGroups[static_cast<usize>(AudioBus::Effects)])
            {
                return; // no per-scene child group available
            }
            data.reverb = CreateReverbNode(*impl.allocator, impl.engine, params);
            if (data.reverb == nullptr)
            {
                return;
            }
            (void)ma_node_attach_output_bus(
                data.reverb, 0, &impl.busGroups[static_cast<usize>(AudioBus::Effects)], 0);
            (void)ma_node_attach_output_bus(group, 0, data.reverb, 0);
        }
        else
        {
            data.reverb->state->SetParams(params);
        }
        data.reverbWet = Clamp(params.wet, 0.0f, 1.0f);
        if (data.reverb != nullptr && data.reverb->state != nullptr)
        {
            AudioReverbParams applied = params;
            applied.wet = data.reverbWet;
            data.reverb->state->SetParams(applied);
        }
        // The per-voice send reverb shares the room character but stays wet-only and
        // fully open: the send level lives on each voice's splitter.
        if (data.sendReverb != nullptr && data.sendReverb->state != nullptr)
        {
            AudioReverbParams sendParams = params;
            sendParams.wet = 1.0f;
            sendParams.dry = 0.0f;
            data.sendReverb->state->SetParams(sendParams);
        }
    }

    f32 AudioEngine::SceneReverbWet(u64 sceneGroup) const
    {
        SceneGroupData** data = m_impl->sceneGroups.Find(sceneGroup);
        return data != nullptr ? (*data)->reverbWet : 0.0f;
    }

    void AudioEngine::StopSceneGroup(u64 sceneGroup)
    {
        if (sceneGroup == 0)
        {
            return;
        }
        for (usize i = 0; i < m_impl->voices.Size(); ++i)
        {
            if (m_impl->voices[i].state != VoiceState::Free &&
                m_impl->voices[i].sceneGroup == sceneGroup)
            {
                Stop(m_impl->HandleFor(i));
            }
        }
    }

    // ---------------- codec helpers (declared in :clip) ----------------

    bool ProbeAudioClipMetadata(Span<const byte> encodedBytes, AudioClipMetadata& outMetadata)
    {
        outMetadata = AudioClipMetadata{};
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
        ma_decoder decoder;
        if (ma_decoder_init_memory(encodedBytes.Data(), encodedBytes.Size(), &config, &decoder) !=
            MA_SUCCESS)
        {
            return false;
        }
        outMetadata.channels = decoder.outputChannels;
        outMetadata.sampleRate = decoder.outputSampleRate;
        ma_uint64 frames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS)
        {
            outMetadata.frameCount = frames;
            outMetadata.durationSeconds =
                decoder.outputSampleRate > 0
                    ? static_cast<f32>(static_cast<f64>(frames) / decoder.outputSampleRate)
                    : 0.0f;
        }
        ma_decoder_uninit(&decoder);
        return outMetadata.channels > 0 && outMetadata.sampleRate > 0;
    }

    bool DecodeAudioClipToPcm16(Span<const byte> encodedBytes, u32 targetChannels,
                                Array<i16>& outInterleavedSamples, AudioClipMetadata& outMetadata)
    {
        outMetadata = AudioClipMetadata{};
        outInterleavedSamples.Clear();
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, targetChannels, 0);
        ma_decoder decoder;
        if (ma_decoder_init_memory(encodedBytes.Data(), encodedBytes.Size(), &config, &decoder) !=
            MA_SUCCESS)
        {
            return false;
        }
        outMetadata.channels = decoder.outputChannels;
        outMetadata.sampleRate = decoder.outputSampleRate;
        i16 chunk[4096];
        const u64 chunkFrames = 4096 / decoder.outputChannels;
        for (;;)
        {
            ma_uint64 read = 0;
            const ma_result result =
                ma_decoder_read_pcm_frames(&decoder, chunk, chunkFrames, &read);
            for (u64 i = 0; i < read * decoder.outputChannels; ++i)
            {
                outInterleavedSamples.PushBack(chunk[i]);
            }
            outMetadata.frameCount += read;
            if (result != MA_SUCCESS || read < chunkFrames)
            {
                break;
            }
        }
        ma_decoder_uninit(&decoder);
        outMetadata.durationSeconds =
            outMetadata.sampleRate > 0 ? static_cast<f32>(static_cast<f64>(outMetadata.frameCount) /
                                                          outMetadata.sampleRate)
                                       : 0.0f;
        return outMetadata.frameCount > 0;
    }

    bool EncodeWavFromPcm16(Span<const i16> interleavedSamples, u32 channels, u32 sampleRate,
                            Array<byte>& outWavBytes)
    {
        if (channels == 0 || sampleRate == 0 || interleavedSamples.Size() % channels != 0)
        {
            return false;
        }
        // A canonical 44-byte PCM16 RIFF header and the sample payload (no encoder state
        // needed: WAV is the one container we write, and it is trivial).
        const u32 dataBytes = static_cast<u32>(interleavedSamples.Size() * sizeof(i16));
        const u32 byteRate = sampleRate * channels * static_cast<u32>(sizeof(i16));
        const u16 blockAlign = static_cast<u16>(channels * sizeof(i16));

        outWavBytes.Clear();
        outWavBytes.Reserve(44 + dataBytes);
        auto pushBytes = [&](const void* source, usize size)
        {
            const byte* p = static_cast<const byte*>(source);
            for (usize i = 0; i < size; ++i)
            {
                outWavBytes.PushBack(p[i]);
            }
        };
        auto pushU32 = [&](u32 value) { pushBytes(&value, 4); };
        auto pushU16 = [&](u16 value) { pushBytes(&value, 2); };

        pushBytes("RIFF", 4);
        pushU32(36 + dataBytes);
        pushBytes("WAVE", 4);
        pushBytes("fmt ", 4);
        pushU32(16);
        pushU16(1); // PCM
        pushU16(static_cast<u16>(channels));
        pushU32(sampleRate);
        pushU32(byteRate);
        pushU16(blockAlign);
        pushU16(16); // bits per sample
        pushBytes("data", 4);
        pushU32(dataBytes);
        pushBytes(interleavedSamples.Data(), dataBytes);
        return true;
    }
    // ---- editor waveform: a pure decode into per-bucket peaks ----

    bool BuildWaveformPeaks(Span<const byte> encoded, u32 buckets, Array<f32>& outPeaks)
    {
        outPeaks.Clear();
        if (encoded.IsEmpty() || buckets == 0)
        {
            return false;
        }

        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder decoder;
        if (ma_decoder_init_memory(encoded.Data(), encoded.Size(), &config, &decoder) != MA_SUCCESS)
        {
            return false;
        }
        const u32 channels = decoder.outputChannels;

        // Total length drives the frame-to-bucket map; fall back to a growing two-pass
        // walk when the container cannot report it (some mp3s).
        ma_uint64 totalFrames = 0;
        (void)ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);

        outPeaks.Resize(buckets);
        for (u32 i = 0; i < buckets; ++i)
        {
            outPeaks[i] = 0.0f;
        }

        f32 chunk[4096];
        const u64 chunkFrames = 4096 / (channels == 0 ? 1 : channels);
        u64 frameCursor = 0;
        Array<f32> unknownLengthPeaks; // per-chunk peaks when length is unknown
        for (;;)
        {
            ma_uint64 read = 0;
            const ma_result result =
                ma_decoder_read_pcm_frames(&decoder, chunk, chunkFrames, &read);
            for (u64 frame = 0; frame < read; ++frame)
            {
                f32 peak = 0.0f;
                for (u32 c = 0; c < channels; ++c)
                {
                    const f32 magnitude = Abs(chunk[frame * channels + c]);
                    if (magnitude > peak)
                    {
                        peak = magnitude;
                    }
                }
                if (totalFrames > 0)
                {
                    const u64 bucket =
                        Min<u64>((frameCursor + frame) * buckets / totalFrames, buckets - 1);
                    if (peak > outPeaks[static_cast<usize>(bucket)])
                    {
                        outPeaks[static_cast<usize>(bucket)] = peak;
                    }
                }
                else
                {
                    unknownLengthPeaks.PushBack(peak);
                }
            }
            frameCursor += read;
            if (result != MA_SUCCESS || read < chunkFrames)
            {
                break;
            }
        }
        ma_decoder_uninit(&decoder);
        if (frameCursor == 0)
        {
            outPeaks.Clear();
            return false;
        }

        if (totalFrames == 0)
        {
            // Unknown length: rebucket the collected per-frame peaks now that the
            // total is known.
            const u64 total = unknownLengthPeaks.Size();
            for (u64 i = 0; i < total; ++i)
            {
                const u64 bucket = Min<u64>(i * buckets / total, buckets - 1);
                if (unknownLengthPeaks[static_cast<usize>(i)] >
                    outPeaks[static_cast<usize>(bucket)])
                {
                    outPeaks[static_cast<usize>(bucket)] =
                        unknownLengthPeaks[static_cast<usize>(i)];
                }
            }
        }
        for (u32 i = 0; i < buckets; ++i)
        {
            outPeaks[i] = Min(outPeaks[i], 1.0f);
        }
        return true;
    }

}
