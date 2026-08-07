// Draconic::Audio - AudioEngine implementation (module IMPLEMENTATION unit).
//
// ALL miniaudio contact lives here: the interface stays ma_*-free (API hygiene + GCC's
// module serializer must never see the 4 MB header in an interface unit's global
// fragment). The MINIAUDIO_IMPLEMENTATION translation unit is MiniaudioImpl.cpp; this
// file only consumes the declarations.
//
// Layout of the miniaudio object graph:
//   ma_engine (device or headless mixer + resource manager with our ma_vfs bridge)
//     Master group <- Effects / Music / UI groups (the fixed P1 bus layout)
//       per-scene child groups under each bus (lazy; scene pause stops the group node)
//         ma_sound voices from the fixed pool
//           [P2: an ma_lpf node per 3D voice for distance low-pass - the slot is
//            reserved on VoiceSlot (`lowpassNode`) and in the attach path below]

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

#include "miniaudio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

module draconic.audio;

import draconic.foundation;
import draconic.vfs;

using namespace draconic::foundation;

namespace draconic::audio
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
        // manager does NOT copy - the registry owns the decoded frames / keeps the
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
        };

        void FormatClipName(char* buffer, usize size, const char* prefix, const void* key)
        {
            std::snprintf(buffer, size, "%s%016llx", prefix,
                          static_cast<unsigned long long>(reinterpret_cast<uptr>(key)));
        }

        // ---- the Freeverb ma_node wrapper (P3): pure DSP lives in :reverb ----
        // CONTINUOUS processing so the tail keeps ringing after inputs stop.
        struct ReverbNode
        {
            ma_node_base base; // FIRST: a ReverbNode* is a valid ma_node*
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

        [[nodiscard]] ReverbNode* CreateReverbNode(ma_engine& engine,
                                                   const AudioReverbParams& params)
        {
            auto* node = DefaultAllocator().New<ReverbNode>();
            node->channels = ma_engine_get_channels(&engine);
            node->state = DefaultAllocator().New<FreeverbState>();
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
                DefaultAllocator().Delete(node->state);
                DefaultAllocator().Delete(node);
                return nullptr;
            }
            return node;
        }

        void DestroyReverbNode(ReverbNode* node)
        {
            if (node == nullptr)
            {
                return;
            }
            ma_node_uninit(&node->base, nullptr);
            DefaultAllocator().Delete(node->state);
            DefaultAllocator().Delete(node);
        }
    }

    // Per-bus effect chain node: typed so teardown calls the right uninit. Shared by
    // the fixed buses and the named custom buses.
    struct BusEffectNode
    {
        AudioBusEffectKind kind = AudioBusEffectKind::None;
        void* node = nullptr;
    };

    // A named custom bus (audio.md: the additive topology freedom): one extra
    // ma_sound_group, parented to a fixed bus or another custom bus per the layout.
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
    };

    struct VoiceSlot
    {
        // Arena-backed (Impl::soundArena): the ma_sound must survive its slot on a
        // faded steal - the pointer HANDS OVER to the dying side list while the slot
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
        AudioBus bus = AudioBus::Effects;
        u64 sceneGroup = 0;
        RefPtr<AudioClip> clip;
        // Per-voice reverb send: a splitter at the END of the voice chain - out 0 is
        // the dry path to the group, out 1 (volume = reverbSend) feeds the scene's
        // wet-only SEND reverb in parallel. nullptr = played with reverbSend 0.
        ma_splitter_node* splitterNode = nullptr;
        f32 reverbSend = 0.0f;
        // Distance low-pass (P2): an ma_lpf node between the sound and its group; the
        // cutoff glides open->floor across [min, max] distance every Update.
        ma_lpf_node* lowpassNode = nullptr;
        f32 lowpassFloorHz = 0.0f;
        f32 lowpassMinDistance = 1.0f;
        f32 lowpassMaxDistance = 100.0f;
        f32 lowpassCutoffHz = 0.0f;
    };

    // Per-scene child groups, one under each bus the scene actually uses (lazy).
    struct SceneGroupData
    {
        ma_sound_group group[static_cast<usize>(AudioBus::Count)]{};
        bool initialized[static_cast<usize>(AudioBus::Count)] = {};
        bool paused = false;
        ReverbNode* reverb = nullptr; // zone reverb on the Effects child group
        f32 reverbWet = 0.0f;
        // The per-voice send target: a WET-ONLY Freeverb (dry pinned to 0) fed by
        // voice splitters, straight into the Effects bus. Zones retune roomSize/
        // damping through SetSceneReverb; sends stay audible regardless of zone
        // occupancy (the send level is the VOICE's own knob).
        ReverbNode* sendReverb = nullptr;
    };

    struct AudioEngine::Impl
    {
        // ---- ma_vfs bridge: miniaudio streams straight out of draconic.vfs ----
        // First member so a pointer to Bridge is a valid ma_vfs*.
        struct Bridge
        {
            ma_vfs_callbacks callbacks{};
            Impl* impl = nullptr;
        };

        AudioEngineSettings settings;
        bool headless = false;
        bool engineInitialized = false;
        ma_engine engine{};
        Bridge bridge{};

        ma_sound_group busGroups[static_cast<usize>(AudioBus::Count)]{};
        bool busGroupInitialized[static_cast<usize>(AudioBus::Count)] = {};
        f32 busVolume[static_cast<usize>(AudioBus::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool busMuted[static_cast<usize>(AudioBus::Count)] = {};

        Array<VoiceSlot> voices; // [0, voiceCount) in-memory, then stream slots
        u32 voiceCount = 0;
        u32 streamVoiceCount = 0;

        // Fixed ma_sound storage (pool + dying headroom; never resized after init -
        // pointers into it are stable) + the free-index stack.
        Array<ma_sound> soundArena;
        Array<u32> freeSounds;

        // Faded steal (audio.md parked item): the victim's ma_sound fades here (~30 ms)
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

        HashMap<u64, SceneGroupData*> sceneGroups; // owned via DefaultAllocator New/Delete
        u64 nextSceneGroupId = 1;

        HashMap<void*, RegisteredClip> registeredClips; // key = AudioClip*
        HashMap<void*, RefPtr<AudioClip>> streamClips;  // ma_vfs name -> clip keep-alive
        HashMap<void*, DedupeEntry> recentPlays;        // key = AudioClip*

        Float3 listenerPosition{0.0f, 0.0f, 0.0f};
        VoiceHandle musicVoice; // the PlayMusic cross-fade tracks ONE music voice

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
            auto* file = DefaultAllocator().New<BridgedFile>();
            file->stream = Move(stream);
            *outFile = reinterpret_cast<ma_vfs_file>(file);
            return MA_SUCCESS;
        }
        static ma_result VfsClose(ma_vfs*, ma_vfs_file file)
        {
            auto* bridged = reinterpret_cast<BridgedFile*>(file);
            DefaultAllocator().Delete(bridged);
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

        explicit Impl(const AudioEngineSettings& engineSettings) : settings(engineSettings)
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
                DRACONIC_LOG_WARNING(
                    u8"Audio",
                    u8"no playback device available - running headless (Null mode: voices "
                    u8"advance silently, handles stay valid)");
                headless = true;
                (void)InitializeEngine(true);
            }
            if (!engineInitialized)
            {
                DRACONIC_LOG_ERROR(u8"Audio",
                                   u8"audio engine failed to initialize - audio disabled");
                return;
            }

            InitializeBusGroups();

            voiceCount = settings.voiceCount;
            streamVoiceCount = settings.streamVoiceCount;
            voices.Resize(static_cast<usize>(voiceCount) + streamVoiceCount);
            // Storage for every pool slot PLUS the dying headroom (a steal briefly
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
            return engineInitialized;
        }

        // The node a bus's output feeds when it has NO effects: Master for the leaves,
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
                    DefaultAllocator().Delete(static_cast<ma_lpf_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Highpass:
                    ma_hpf_node_uninit(static_cast<ma_hpf_node*>(effect.node), nullptr);
                    DefaultAllocator().Delete(static_cast<ma_hpf_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Delay:
                    ma_delay_node_uninit(static_cast<ma_delay_node*>(effect.node), nullptr);
                    DefaultAllocator().Delete(static_cast<ma_delay_node*>(effect.node));
                    break;
                case AudioBusEffectKind::Reverb:
                    DestroyReverbNode(static_cast<ReverbNode*>(effect.node));
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

        // Rebuild one bus's chain: group -> e0 -> e1 -> ... -> parent.
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

        // Generic splice: source -> e0 -> e1 -> ... -> parent (shared by fixed and
        // named custom buses). `chain` must be empty on entry.
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
                    auto* node = DefaultAllocator().New<ma_lpf_node>();
                    ma_lpf_node_config config = ma_lpf_node_config_init(
                        channels, sampleRate, Max(desc.frequencyHz, 10.0f), kLowpassOrder);
                    if (ma_lpf_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        DefaultAllocator().Delete(node);
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::Highpass:
                {
                    auto* node = DefaultAllocator().New<ma_hpf_node>();
                    ma_hpf_node_config config = ma_hpf_node_config_init(
                        channels, sampleRate, Max(desc.frequencyHz, 10.0f), kLowpassOrder);
                    if (ma_hpf_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        DefaultAllocator().Delete(node);
                        continue;
                    }
                    effect.node = node;
                    break;
                }
                case AudioBusEffectKind::Delay:
                {
                    auto* node = DefaultAllocator().New<ma_delay_node>();
                    const u32 delayFrames = static_cast<u32>(Max(desc.delaySeconds, 0.001f) *
                                                             static_cast<f32>(sampleRate));
                    ma_delay_node_config config = ma_delay_node_config_init(
                        channels, sampleRate, delayFrames, Clamp(desc.delayDecay, 0.0f, 0.99f));
                    if (ma_delay_node_init(graph, &config, nullptr, node) != MA_SUCCESS)
                    {
                        DefaultAllocator().Delete(node);
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
                    ReverbNode* node = CreateReverbNode(engine, params);
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
        // (voices SURVIVE a layout rebuild; only the routing changes).
        void DetachVoicesFromCustomBus(CustomBusData& bus)
        {
            for (VoiceSlot& slot : voices)
            {
                if (slot.state == VoiceState::Free ||
                    slot.customBusName.AsView() != bus.name.AsView())
                {
                    continue;
                }
                if (ma_sound_group* fallback = GroupFor(slot.sceneGroup, slot.bus))
                {
                    (void)ma_node_attach_output_bus(VoiceOutputNode(slot), 0, fallback, 0);
                }
                slot.customBusName = String{};
            }
        }

        // The voice chain's LAST node (what attaches to the group): splitter (dry out
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
                params.wet = 1.0f; // full tail; the SEND level is the voice's knob
                params.dry = 0.0f; // wet-only: the dry path already reaches the bus
                data.sendReverb = CreateReverbNode(engine, params);
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
            ClearEffectChain(bus->effects);
            if (bus->initialized)
            {
                ma_sound_group_uninit(&bus->group);
            }
            DefaultAllocator().Delete(bus);
        }

        // Reconcile the live custom-bus set with the layout BY NAME: kept buses update
        // in place (voices keep playing through them), removed buses fall their voices
        // back, new buses splice in. Cycles are defused to Master (cook rejects them;
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
                    DRACONIC_LOG_WARNING(
                        u8"Audio", u8"bus layout: duplicate custom bus '{}' ignored", named.name);
                    continue;
                }
                AudioBus fixedAlias{};
                if (AudioBusFromName(named.name.AsView(), fixedAlias))
                {
                    DRACONIC_LOG_WARNING(
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
                auto* bus = DefaultAllocator().New<CustomBusData>();
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
                        DRACONIC_LOG_WARNING(
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
                        DRACONIC_LOG_WARNING(
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

            // Apply settings + rebuild each bus's effect chain into its parent.
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
        }

        void InitializeBusGroups()
        {
            // Master first; the leaf buses parent to it (the fixed P1 layout).
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
                    // Decode-on-load (open question 2's default for SFX).
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
                    DRACONIC_LOG_WARNING(u8"Audio",
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
                DefaultAllocator().Delete(slot.lowpassNode);
                slot.lowpassNode = nullptr;
            }
            if (slot.splitterNode != nullptr)
            {
                ma_splitter_node_uninit(slot.splitterNode, nullptr);
                DefaultAllocator().Delete(slot.splitterNode);
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
            slot.customBusName = String{};
            ++slot.generation;
        }

        // Faded steal: the victim's handle dies NOW (the slot is reused at once), but
        // its ma_sound fades on the dying list instead of clicking off. Silent victims
        // (paused) release immediately - nothing audible to protect.
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
                ReapDyingVoice(0); // full: hard-cut the OLDEST tail
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
                DefaultAllocator().Delete(dying.lowpass);
            }
            if (dying.splitter != nullptr)
            {
                ma_splitter_node_uninit(dying.splitter, nullptr);
                DefaultAllocator().Delete(dying.splitter);
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

        // Traktor policy: free slot -> lowest priority strictly below the new voice ->
        // farthest same-priority. Returns the pool size when nothing may be taken.
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
                // No lower-priority victim; try the farthest voice of EQUAL priority.
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
                // Faded steal: the victim's slot frees NOW, its audio tail fades on
                // the dying list (~stealFadeSeconds) instead of clicking off.
                StealSlot(voices[victim]);
                return victim;
            }
            return voices.Size();
        }

        // ---------------- distance low-pass ----------------

        // Cutoff mapping: fully open inside minDistance, gliding linearly down to the
        // voice's floor at maxDistance. Reinit only on audible change (>1%) - filter
        // reconfiguration is not free.
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

        [[nodiscard]] ma_sound_group* GroupFor(u64 sceneGroup, AudioBus bus)
        {
            const usize busIndex = static_cast<usize>(bus) < static_cast<usize>(AudioBus::Count)
                                       ? static_cast<usize>(bus)
                                       : static_cast<usize>(AudioBus::Effects);
            if (sceneGroup != 0)
            {
                if (SceneGroupData** data = sceneGroups.Find(sceneGroup))
                {
                    SceneGroupData& groups = **data;
                    if (!groups.initialized[busIndex])
                    {
                        groups.initialized[busIndex] =
                            ma_sound_group_init(&engine, 0, &busGroups[busIndex],
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
            return busGroupInitialized[busIndex] ? &busGroups[busIndex] : nullptr;
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
            DestroyReverbNode((*data)->reverb);
            DestroyReverbNode((*data)->sendReverb);
            for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
            {
                if ((*data)->initialized[bus])
                {
                    ma_sound_group_uninit(&(*data)->group[bus]);
                }
            }
            DefaultAllocator().Delete(*data);
            sceneGroups.Remove(sceneGroup);
        }

        [[nodiscard]] u64 FadeMilliseconds() const
        {
            const f32 seconds = settings.stopFadeSeconds > 0.0f ? settings.stopFadeSeconds : 0.0f;
            return static_cast<u64>(seconds * 1000.0f + 0.5f);
        }
    };

    // ---------------- public surface ----------------

    AudioEngine::AudioEngine(const AudioEngineSettings& settings)
        : m_impl(MakeUnique<Impl>(DefaultAllocator(), settings))
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

        // Dying voices (faded steal): reap the ones whose fade landed.
        for (usize i = impl.dyingVoices.Size(); i > 0; --i)
        {
            if (ma_sound_is_playing(impl.dyingVoices[i - 1].sound) == MA_FALSE)
            {
                impl.ReapDyingVoice(i - 1);
            }
        }

        // Reap: fades that landed, one-shots that reached their end. The same walk
        // glides every filtered voice's distance low-pass (covers one-shots and a
        // moving listener - voices the scene sync never repositions).
        for (VoiceSlot& slot : impl.voices)
        {
            if (slot.state != VoiceState::Free)
            {
                impl.UpdateVoiceLowpass(slot);
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
        // window - a persistent voice must not swallow later legitimate one-shots.
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
            // Runtime mono-guard (Lumix): 3D wants mono - warn once, downmix in-memory
            // clips for real; streamed stereo spatializes as-is (reimport force-mono).
            if (clipPtr->stream)
            {
                if (!impl.warnedStreamStereoSpatial)
                {
                    impl.warnedStreamStereoSpatial = true;
                    DRACONIC_LOG_WARNING(
                        u8"Audio", u8"spatializing a STREAMED multi-channel clip - reimport with "
                                   u8"force-mono for correct 3D imaging (warned once)");
                }
            }
            else if (!impl.warnedMonoDownmix)
            {
                impl.warnedMonoDownmix = true;
                DRACONIC_LOG_WARNING(
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
        } // unreachable: arena covers pool + dying

        // Named-bus addressing: a known custom bus overrides the enum bus. Unknown
        // names warn once and fall back - content typos never silence a game.
        ma_sound_group* group = impl.GroupFor(params.sceneGroup, params.bus);
        i32 customBusIndex = -1;
        if (!params.busName.IsEmpty())
        {
            customBusIndex = impl.FindCustomBus(params.busName.AsView());
            if (customBusIndex >= 0 &&
                impl.customBuses[static_cast<usize>(customBusIndex)]->initialized)
            {
                group = &impl.customBuses[static_cast<usize>(customBusIndex)]->group;
            }
            else
            {
                customBusIndex = -1;
                if (!impl.warnedUnknownBusName)
                {
                    impl.warnedUnknownBusName = true;
                    DRACONIC_LOG_WARNING(
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
            DRACONIC_LOG_WARNING(u8"Audio", u8"voice init failed for clip");
            return {};
        }
        slot.soundInitialized = true;
        slot.state = params.startPaused ? VoiceState::Paused : VoiceState::Playing;
        slot.priority = params.priority;
        slot.spatial = params.spatial;
        slot.position = params.position;
        slot.volume = params.volume;
        slot.pitch = params.pitch;
        slot.bus = params.bus;
        slot.customBusName = customBusIndex >= 0 ? String(params.busName.AsView()) : String{};
        slot.sceneGroup = params.sceneGroup;
        slot.clip = clip;
        slot.looping = params.loop || clipPtr->loop;

        // Distance low-pass: splice an ma_lpf between the sound and its group. Wholly
        // per-voice; 0 Hz = no node (the chain stays sound->group).
        if (params.spatial && params.distanceLowpassHz > 0.0f && group != nullptr)
        {
            auto* node = DefaultAllocator().New<ma_lpf_node>();
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
                DefaultAllocator().Delete(node);
            }
        }

        // Per-voice reverb send: splice a splitter at the END of the chain - out 0
        // stays the dry route into the group, out 1 (volume = reverbSend) feeds the
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
                auto* splitter = DefaultAllocator().New<ma_splitter_node>();
                ma_splitter_node_config config =
                    ma_splitter_node_config_init(ma_engine_get_channels(&impl.engine));
                if (ma_splitter_node_init(ma_engine_get_node_graph(&impl.engine), &config, nullptr,
                                          splitter) != MA_SUCCESS)
                {
                    DefaultAllocator().Delete(splitter);
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
                    DefaultAllocator().Delete(splitter);
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

        // A custom-bus voice bypasses the scene child groups, so a paused scene group
        // must freeze it explicitly (fixed-bus voices inherit the halted group node).
        bool sceneFrozen = false;
        if (customBusIndex >= 0 && params.sceneGroup != 0)
        {
            if (SceneGroupData** sceneData = impl.sceneGroups.Find(params.sceneGroup))
            {
                sceneFrozen = (*sceneData)->paused;
            }
        }
        if (!params.startPaused && !sceneFrozen)
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
        VoiceSlot* slot = m_impl->Resolve(handle);
        if (slot == nullptr)
        {
            return;
        }
        if (slot->state == VoiceState::Paused)
        {
            m_impl->ReleaseSlot(*slot); // silent already - no fade needed
            return;
        }
        if (slot->state != VoiceState::Stopping)
        {
            (void)ma_sound_stop_with_fade_in_milliseconds(slot->sound, m_impl->FadeMilliseconds());
            slot->state = VoiceState::Stopping;
        }
    }

    // ---- music (P2): one tracked voice on the Music bus, cross-faded ----

    VoiceHandle AudioEngine::PlayMusic(const RefPtr<AudioClip>& clip, f32 crossFadeSeconds,
                                       f32 volume)
    {
        Impl& impl = *m_impl;
        const u64 fadeMs = static_cast<u64>(Max(crossFadeSeconds, 0.0f) * 1000.0f + 0.5f);

        // Fade the incumbent out over the SAME window the newcomer fades in.
        if (VoiceSlot* current = impl.Resolve(impl.musicVoice))
        {
            if (current->state != VoiceState::Stopping)
            {
                (void)ma_sound_stop_with_fade_in_milliseconds(current->sound, fadeMs);
                current->state = VoiceState::Stopping;
            }
        }
        impl.musicVoice = VoiceHandle{};

        AudioPlayParams params;
        params.bus = AudioBus::Music;
        params.loop = true; // music loops unless the clip says otherwise anyway
        params.volume = volume;
        params.allowDedupe = false; // replaying the same track restarts it
        const VoiceHandle handle = Play(clip, params);
        if (VoiceSlot* slot = impl.Resolve(handle); slot != nullptr && fadeMs > 0)
        {
            ma_sound_set_fade_in_milliseconds(slot->sound, 0.0f, 1.0f, fadeMs);
        }
        impl.musicVoice = handle;
        return handle;
    }

    void AudioEngine::StopMusic(f32 fadeSeconds)
    {
        Impl& impl = *m_impl;
        if (VoiceSlot* slot = impl.Resolve(impl.musicVoice))
        {
            if (slot->state != VoiceState::Stopping)
            {
                const u64 fadeMs = static_cast<u64>(Max(fadeSeconds, 0.0f) * 1000.0f + 0.5f);
                (void)ma_sound_stop_with_fade_in_milliseconds(slot->sound, fadeMs);
                slot->state = VoiceState::Stopping;
            }
        }
        impl.musicVoice = VoiceHandle{};
    }

    VoiceHandle AudioEngine::MusicVoice() const { return m_impl->musicVoice; }

    // ---- bus layout (P2) ----

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

    void AudioEngine::SetVoiceVolume(VoiceHandle handle, f32 volume)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
            slot->volume = volume;
            const AudioClip* clip = slot->clip.Get();
            ma_sound_set_volume(slot->sound, volume * (clip != nullptr ? clip->gain : 1.0f));
        }
    }

    void AudioEngine::SetVoicePitch(VoiceHandle handle, f32 pitch)
    {
        if (VoiceSlot* slot = m_impl->Resolve(handle))
        {
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
        // The steal-farthest heuristic + distance low-pass track listener 0 (primary).
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

    u64 AudioEngine::CreateSceneGroup()
    {
        Impl& impl = *m_impl;
        if (!impl.engineInitialized)
        {
            return 0;
        }
        const u64 id = impl.nextSceneGroupId++;
        impl.sceneGroups.InsertOrAssign(id, DefaultAllocator().New<SceneGroupData>());
        return id;
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
        // Custom-bus voices route OUTSIDE the scene's child groups (the named tree is
        // engine-global) - freeze/resume them individually. Their VoiceState stays
        // Playing, mirroring the group behavior; user-paused voices are untouched.
        for (VoiceSlot& slot : impl.voices)
        {
            if (slot.sceneGroup != sceneGroup || slot.customBusName.IsEmpty() ||
                slot.state != VoiceState::Playing)
            {
                continue;
            }
            if (paused)
            {
                (void)ma_sound_stop_with_fade_in_milliseconds(slot.sound, impl.FadeMilliseconds());
            }
            else
            {
                ma_sound_reset_stop_time_and_fade(slot.sound);
                ma_sound_set_fade_in_milliseconds(slot.sound, 0.0f, 1.0f, impl.FadeMilliseconds());
                (void)ma_sound_start(slot.sound);
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
            // Splice on the scene's Effects child group: group -> reverb -> Effects bus.
            ma_sound_group* group = impl.GroupFor(sceneGroup, AudioBus::Effects);
            if (group == nullptr || group == &impl.busGroups[static_cast<usize>(AudioBus::Effects)])
            {
                return; // no per-scene child group available
            }
            data.reverb = CreateReverbNode(impl.engine, params);
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
        // The per-voice SEND reverb shares the room character but stays wet-only and
        // fully open - the send level lives on each voice's splitter.
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
        // A canonical 44-byte PCM16 RIFF header + the sample payload (no encoder state
        // needed - WAV is the one container we write, and it is trivial).
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
    // ---- editor waveform (P2): pure decode -> per-bucket peaks ----

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

        // Total length drives the frame->bucket map; fall back to a growing two-pass
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
        Array<f32> unknownLengthPeaks; // per-CHUNK peaks when length is unknown
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
