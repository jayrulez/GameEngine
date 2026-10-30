// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Audio - the `foundation.audio.editor` module (tooling).
//
// Source-side audio authoring + cook:
//   * AudioClipAsset (pipeline::Asset): references the copied source file + import
//     settings (stream / force-mono / loop+points / trim / normalize / gain).
//   * AudioClipAssetBuilder: transcode-free write-through v1 - validate + probe via the
//     foundation.audio codec helpers, and write the ORIGINAL container bytes as the "data"
//     stream. When a destructive option is on (force mono / trim trailing silence /
//     normalize) the source is decoded, processed, and re-encoded as WAV (the one
//     container we write); untouched sources cook byte-identical.
//   * AudioFileImporter (wav/ogg/mp3/flac) with AudioImportOptions: `Stream` auto-computes
//     at import (> 10 s or > 2 MB pre-checks the asset; the dialog toggle FORCES it),
//     WAV `smpl` loop points are detected and enable looping.
//
// Never linked by the runtime. miniaudio itself never appears here - the codec seam in
// foundation.audio keeps this module decoder-free.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

export module audio.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.content;
import foundation.audio;
import foundation.audio.resource;

using namespace foundation::core;
using namespace foundation::audio;

export namespace pipeline{
    namespace content = foundation::content;

    // Source asset: the audio file + how it should cook.
    class AudioClipAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(AudioClipAsset, pipeline::Asset)
    public:
        bool stream = false;         // decode on the fly at runtime (music/ambience)
        bool keepCompressed = false; // in-memory clips: decode on play, not on load
        bool forceMono = false;      // downmix at cook (the 3D-intent default)
        bool loop = false;
        u64 loopStartFrame = 0; // loopEndFrame 0 = clip end
        u64 loopEndFrame = 0;
        bool trimTrailingSilence = false; // Traktor trick: drop the silent tail
        bool normalize = false;           // peak-normalize to -1 dBFS
        f32 gain = 1.0f;                  // authored gain (runtime fold, not baked)

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "stream", stream);
            foundation::core::Serialize(ar, "keepCompressed", keepCompressed);
            foundation::core::Serialize(ar, "forceMono", forceMono);
            foundation::core::Serialize(ar, "loop", loop);
            foundation::core::Serialize(ar, "loopStartFrame", loopStartFrame);
            foundation::core::Serialize(ar, "loopEndFrame", loopEndFrame);
            foundation::core::Serialize(ar, "trimTrailingSilence", trimTrailingSilence);
            foundation::core::Serialize(ar, "normalize", normalize);
            foundation::core::Serialize(ar, "gain", gain);
        }
    };

    // ---- pure import helpers (unit-testable without a project) ----

    /// The Stream auto-default: long or large sources stream, small SFX stay
    /// in-memory. Computed at import; the asset field stays editable afterwards.
    [[nodiscard]] inline bool ShouldStreamAudioByDefault(f32 durationSeconds, usize sizeBytes)
    {
        return durationSeconds > 10.0f || sizeBytes > usize(2) * 1024 * 1024;
    }

    /// Scans a RIFF/WAVE container for the first `smpl` sampler loop. True when found,
    /// with the loop's start/end sample frames. Pure byte walk - no decoder involved.
    [[nodiscard]] inline bool ParseWavSampleLoop(Span<const byte> wavBytes, u64& outLoopStartFrame,
                                                 u64& outLoopEndFrame)
    {
        auto readU32 = [&](usize offset) -> u32
        {
            return static_cast<u32>(static_cast<u8>(wavBytes[offset])) |
                   static_cast<u32>(static_cast<u8>(wavBytes[offset + 1])) << 8 |
                   static_cast<u32>(static_cast<u8>(wavBytes[offset + 2])) << 16 |
                   static_cast<u32>(static_cast<u8>(wavBytes[offset + 3])) << 24;
        };
        auto tagIs = [&](usize offset, const char* tag) -> bool
        {
            return static_cast<char>(wavBytes[offset]) == tag[0] &&
                   static_cast<char>(wavBytes[offset + 1]) == tag[1] &&
                   static_cast<char>(wavBytes[offset + 2]) == tag[2] &&
                   static_cast<char>(wavBytes[offset + 3]) == tag[3];
        };
        if (wavBytes.Size() < 12 || !tagIs(0, "RIFF") || !tagIs(8, "WAVE"))
        {
            return false;
        }
        usize cursor = 12;
        while (cursor + 8 <= wavBytes.Size())
        {
            const u32 chunkSize = readU32(cursor + 4);
            if (tagIs(cursor, "smpl"))
            {
                // smpl layout: 36 bytes of sampler fields (numLoops at +28), then
                // 24-byte loop records (start at +8, end at +12 within the record).
                const usize body = cursor + 8;
                if (chunkSize >= 36 + 24 && body + 36 + 24 <= wavBytes.Size() &&
                    readU32(body + 28) >= 1)
                {
                    outLoopStartFrame = readU32(body + 36 + 8);
                    outLoopEndFrame = readU32(body + 36 + 12);
                    return outLoopEndFrame > outLoopStartFrame;
                }
                return false;
            }
            cursor += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
        }
        return false;
    }

    // Cooks an AudioClipAsset -> AudioClipSource (+ "data" container-byte stream).
    class AudioClipAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &AudioClipAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioClipSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 1; }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const AudioClipAsset& audioAsset = static_cast<const AudioClipAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            Result<Array<byte>> bytes = ReadSourceBytes(ctx, audioAsset.fileName.View());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            Array<byte>& container = bytes.Value();

            // Validate: undecodable sources fail the cook, they never reach runtime.
            AudioClipMetadata metadata;
            if (!ProbeAudioClipMetadata(Span<const byte>(container.Data(), container.Size()),
                                        metadata))
            {
                LOG_ERROR(u8"Audio", u8"'{}' is not decodable audio - cook failed",
                                   audioAsset.fileName.View());
                return Status{ErrorCode::InvalidArgument};
            }

            String extension = pipeline::FileExtensionLower(audioAsset.fileName.View());

            // Destructive options re-encode (decode -> process -> WAV). Everything else
            // writes the original container bytes through untouched.
            const bool transform =
                audioAsset.forceMono || audioAsset.trimTrailingSilence || audioAsset.normalize;
            if (transform)
            {
                const Status processed = ApplyTransforms(audioAsset, container, metadata);
                if (!processed.IsOk())
                {
                    return processed;
                }
                extension = String(u8"wav");
            }

            AudioClipSource cooked;
            cooked.channels = metadata.channels;
            cooked.sampleRate = metadata.sampleRate;
            cooked.frameCount = metadata.frameCount;
            cooked.durationSeconds = metadata.durationSeconds;
            cooked.gain = audioAsset.gain;
            cooked.loop = audioAsset.loop;
            cooked.loopStartFrame = audioAsset.loopStartFrame;
            cooked.loopEndFrame = audioAsset.loopEndFrame;
            cooked.stream = audioAsset.stream;
            cooked.keepCompressed = audioAsset.keepCompressed;
            cooked.containerExtension = extension;

            const Status wrote = ctx.output->WriteObject(cooked);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return ctx.output->WriteData(u8"data",
                                         Span<const byte>(container.Data(), container.Size()));
        }

    private:
        [[nodiscard]] static Status ApplyTransforms(const AudioClipAsset& asset,
                                                    Array<byte>& container,
                                                    AudioClipMetadata& metadata)
        {
            Array<i16> samples;
            AudioClipMetadata decoded;
            if (!DecodeAudioClipToPcm16(Span<const byte>(container.Data(), container.Size()),
                                        asset.forceMono ? 1u : 0u, samples, decoded))
            {
                return Status{ErrorCode::InvalidArgument};
            }

            if (asset.trimTrailingSilence)
            {
                // Drop the tail below ~-60 dBFS, keeping a 10 ms pad (Traktor trick).
                constexpr i16 kSilenceThreshold = 33;
                usize lastAudibleFrame = 0;
                const usize frameCount = samples.Size() / decoded.channels;
                for (usize frame = frameCount; frame > 0; --frame)
                {
                    bool audible = false;
                    for (u32 channel = 0; channel < decoded.channels; ++channel)
                    {
                        const i16 sample = samples[(frame - 1) * decoded.channels + channel];
                        const i16 magnitude = sample < 0 ? static_cast<i16>(-sample) : sample;
                        if (magnitude > kSilenceThreshold)
                        {
                            audible = true;
                            break;
                        }
                    }
                    if (audible)
                    {
                        lastAudibleFrame = frame;
                        break;
                    }
                }
                const usize pad = decoded.sampleRate / 100;
                usize keepFrames = lastAudibleFrame + pad;
                if (keepFrames > frameCount)
                {
                    keepFrames = frameCount;
                }
                samples.Resize(keepFrames * decoded.channels);
                decoded.frameCount = keepFrames;
                decoded.durationSeconds =
                    decoded.sampleRate > 0
                        ? static_cast<f32>(static_cast<f64>(keepFrames) / decoded.sampleRate)
                        : 0.0f;
            }

            if (asset.normalize && !samples.IsEmpty())
            {
                i32 peak = 0;
                for (i16 sample : samples)
                {
                    const i32 magnitude = sample < 0 ? -static_cast<i32>(sample) : sample;
                    if (magnitude > peak)
                    {
                        peak = magnitude;
                    }
                }
                if (peak > 0)
                {
                    const f32 target = 0.891f * 32767.0f; // -1 dBFS headroom
                    const f32 scale = target / static_cast<f32>(peak);
                    for (i16& sample : samples)
                    {
                        f32 scaled = static_cast<f32>(sample) * scale;
                        if (scaled > 32767.0f)
                        {
                            scaled = 32767.0f;
                        }
                        if (scaled < -32768.0f)
                        {
                            scaled = -32768.0f;
                        }
                        sample = static_cast<i16>(scaled);
                    }
                }
            }

            Array<byte> wav;
            if (!EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()),
                                    decoded.channels, decoded.sampleRate, wav))
            {
                return Status{ErrorCode::InvalidArgument};
            }
            container = Move(wav);
            metadata = decoded;
            return Status{};
        }
    };

    // Import-dialog options (all toggles; the Stream auto-default is computed from the
    // probed file at import, so the checkbox here is a FORCE, not the whole story).
    class AudioImportOptions final : public pipeline::ImportOptions
    {
        RTTI_OBJECT(AudioImportOptions, pipeline::ImportOptions)
    public:
        bool stream = false;
        bool forceMono = false;
        bool loop = false;
        bool trimTrailingSilence = false;
        bool normalize = false;

        [[nodiscard]] Array<Toggle> Toggles() override
        {
            Array<Toggle> toggles;
            toggles.PushBack(
                Toggle{u8"Stream",
                       u8"Decode on the fly at runtime (auto-enabled for sources over 10 s / 2 MB)",
                       &stream});
            toggles.PushBack(
                Toggle{u8"Force mono",
                       u8"Downmix to one channel at cook (recommended for 3D-positioned sounds)",
                       &forceMono});
            toggles.PushBack(Toggle{
                u8"Loop",
                u8"Loop by default when played (WAV smpl loop points are detected automatically)",
                &loop});
            toggles.PushBack(Toggle{u8"Trim trailing silence", u8"Drop the silent tail at cook",
                                    &trimTrailingSilence});
            toggles.PushBack(
                Toggle{u8"Normalize", u8"Peak-normalize to -1 dBFS at cook", &normalize});
            return toggles;
        }
    };

    // OS-file importer (editor drag-drop): copies the audio file into Sources/ and
    // creates an AudioClipAsset named after the file stem.
    class AudioFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Audio"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView candidate : {u8"wav", u8"ogg", u8"mp3", u8"flac"})
            {
                if (extension == candidate)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"AudioClipAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"AudioClipAsset");
        }

        [[nodiscard]] RefPtr<pipeline::ImportOptions>
        CreateOptions(IAllocator& allocator) const override
        {
            return MakeRef<AudioImportOptions>(allocator);
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            Result<Array<byte>> bytes = ReadFile(sourcePath);
            if (!bytes.HasValue())
            {
                return Err(bytes.Error());
            }

            AudioClipMetadata metadata;
            if (!ProbeAudioClipMetadata(
                    Span<const byte>(bytes.Value().Data(), bytes.Value().Size()), metadata))
            {
                LOG_ERROR(u8"Audio", u8"'{}' is not decodable audio - import refused",
                                   sourcePath);
                return Err(ErrorCode::InvalidArgument);
            }

            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, AudioClipAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();

            const auto* audioOptions = static_cast<const AudioImportOptions*>(options);
            AudioClipAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.stream =
                (audioOptions != nullptr && audioOptions->stream) ||
                ShouldStreamAudioByDefault(metadata.durationSeconds, bytes.Value().Size());
            if (audioOptions != nullptr)
            {
                asset.forceMono = audioOptions->forceMono;
                asset.loop = audioOptions->loop;
                asset.trimTrailingSilence = audioOptions->trimTrailingSilence;
                asset.normalize = audioOptions->normalize;
            }

            // WAV smpl loop points: authored loops win over the checkbox default.
            u64 loopStart = 0;
            u64 loopEnd = 0;
            if (pipeline::FileExtensionLower(sourcePath) == u8"wav" &&
                ParseWavSampleLoop(Span<const byte>(bytes.Value().Data(), bytes.Value().Size()),
                                   loopStart, loopEnd))
            {
                asset.loop = true;
                asset.loopStartFrame = loopStart;
                asset.loopEndFrame = loopEnd;
            }

            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // ---- bus layout asset: the mixer edited in the inspector ----
    // FLAT per-bus fields so the reflection inspector edits it without an array
    // editor: per bus - volume, mute, and three effect slots (0 disables each). The
    // builder folds the flat fields into the generic wire chain (lowpass -> highpass ->
    // delay, in that order, when enabled). Asset data v2 adds a FIXED bank of custom-bus
    // slots (the SoundCueAsset precedent: fixed slots keep the existing editing path
    // working without an array editor): each slot = name + parent + the same flat bus
    // fields; an empty name disables the slot.

    inline constexpr usize kAudioCustomBusSlotCount = 8;

    class AudioBusLayoutAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(AudioBusLayoutAsset, pipeline::Asset)
    public:
        struct Bus
        {
            f32 volume = 1.0f;
            bool muted = false;
            f32 lowpassHz = 0.0f;    // 0 = off
            f32 highpassHz = 0.0f;   // 0 = off
            f32 delaySeconds = 0.0f; // 0 = off
            f32 delayDecay = 0.3f;
            f32 reverbWet = 0.0f; // 0 = off
            f32 reverbRoomSize = 0.6f;
            f32 reverbDamping = 0.4f;
        };
        // A named custom bus: parent = one of the four fixed bus names (case-
        // insensitive) or another slot's name; empty parent = Master. Cycles among
        // slots FAIL the cook.
        struct CustomBusSlot
        {
            String name; // empty = slot unused
            String parent;
            Bus bus;
        };
        Bus master;
        Bus effects;
        Bus music;
        Bus ui;
        CustomBusSlot custom[kAudioCustomBusSlotCount];

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            auto serializeBus = [&ar](Bus& bus)
            {
                foundation::core::Serialize(ar, "volume", bus.volume);
                foundation::core::Serialize(ar, "muted", bus.muted);
                foundation::core::Serialize(ar, "lowpassHz", bus.lowpassHz);
                foundation::core::Serialize(ar, "highpassHz", bus.highpassHz);
                foundation::core::Serialize(ar, "delaySeconds", bus.delaySeconds);
                foundation::core::Serialize(ar, "delayDecay", bus.delayDecay);
                foundation::core::Serialize(ar, "reverbWet", bus.reverbWet);
                foundation::core::Serialize(ar, "reverbRoomSize", bus.reverbRoomSize);
                foundation::core::Serialize(ar, "reverbDamping", bus.reverbDamping);
            };
            serializeBus(master);
            serializeBus(effects);
            serializeBus(music);
            serializeBus(ui);
            u32 slots = kAudioCustomBusSlotCount;
            foundation::core::Serialize(ar, "customSlots", slots);
            const u32 count = Min<u32>(slots, kAudioCustomBusSlotCount);
            for (u32 i = 0; i < count; ++i)
            {
                foundation::core::Serialize(ar, "name", custom[i].name);
                foundation::core::Serialize(ar, "parent", custom[i].parent);
                serializeBus(custom[i].bus);
            }
        }
    };

    class AudioBusLayoutAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &AudioBusLayoutAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioBusLayoutSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 2; } // v2: custom buses

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const auto& layoutAsset = static_cast<const AudioBusLayoutAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            AudioBusLayoutSource source;
            const AudioBusLayoutAsset::Bus* buses[static_cast<usize>(AudioBus::Count)] = {};
            buses[static_cast<usize>(AudioBus::Master)] = &layoutAsset.master;
            buses[static_cast<usize>(AudioBus::Effects)] = &layoutAsset.effects;
            buses[static_cast<usize>(AudioBus::Music)] = &layoutAsset.music;
            buses[static_cast<usize>(AudioBus::UI)] = &layoutAsset.ui;
            for (usize i = 0; i < static_cast<usize>(AudioBus::Count); ++i)
            {
                FoldBusSettings(*buses[i], asset.fileName.View(), source.layout.buses[i]);
            }

            // Custom-bus slots: fold used slots; validate parents. A parent CYCLE is a
            // broken mixer - reject the cook (unknown parents only warn: they fall back
            // to Master at apply).
            for (usize i = 0; i < kAudioCustomBusSlotCount; ++i)
            {
                const AudioBusLayoutAsset::CustomBusSlot& slot = layoutAsset.custom[i];
                if (slot.name.IsEmpty())
                {
                    continue;
                }
                AudioBus fixedAlias{};
                if (AudioBusFromName(slot.name.AsView(), fixedAlias))
                {
                    LOG_WARNING(
                        u8"Audio",
                        u8"bus layout '{}': custom bus '{}' shadows a fixed bus - skipped",
                        asset.fileName.View(), slot.name);
                    continue;
                }
                bool duplicate = false;
                for (const AudioNamedBus& existing : source.layout.customBuses)
                {
                    if (existing.name.AsView() == slot.name.AsView())
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                {
                    LOG_WARNING(
                        u8"Audio", u8"bus layout '{}': duplicate custom bus '{}' - slot skipped",
                        asset.fileName.View(), slot.name);
                    continue;
                }
                AudioNamedBus named;
                named.name = String(slot.name.AsView());
                named.parent = String(slot.parent.AsView());
                FoldBusSettings(slot.bus, asset.fileName.View(), named.settings);
                source.layout.customBuses.PushBack(Move(named));
            }

            // Cycle check over the folded set (parents that name other custom buses).
            for (usize i = 0; i < source.layout.customBuses.Size(); ++i)
            {
                usize cursor = i;
                usize steps = 0;
                for (;;)
                {
                    const StringView parent = source.layout.customBuses[cursor].parent.AsView();
                    AudioBus fixed{};
                    if (parent.IsEmpty() || AudioBusFromName(parent, fixed))
                    {
                        break;
                    }
                    bool found = false;
                    for (usize j = 0; j < source.layout.customBuses.Size(); ++j)
                    {
                        if (source.layout.customBuses[j].name.AsView() == parent)
                        {
                            cursor = j;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        break;
                    } // unknown parent: warned at apply, not a cycle
                    if (cursor == i || ++steps > source.layout.customBuses.Size())
                    {
                        LOG_ERROR(u8"Audio",
                                           u8"bus layout '{}': custom bus '{}' is part of a parent "
                                           u8"CYCLE - cook failed",
                                           asset.fileName.View(), source.layout.customBuses[i].name);
                        return Status{ErrorCode::InvalidArgument};
                    }
                }
            }

            const Status written = ctx.output->WriteObject(source);
            return written;
        }

    private:
        // The flat editor fields -> the generic wire chain (lowpass -> highpass ->
        // delay -> reverb, when enabled). Shared by the fixed buses and custom slots.
        static void FoldBusSettings(const AudioBusLayoutAsset::Bus& bus, StringView assetName,
                                    AudioBusSettings& out)
        {
            out.volume = Clamp(bus.volume, 0.0f, 4.0f);
            out.muted = bus.muted;
            if (bus.lowpassHz > 0.0f)
            {
                AudioBusEffectDesc effect;
                effect.kind = AudioBusEffectKind::Lowpass;
                effect.frequencyHz = bus.lowpassHz;
                out.effects.PushBack(effect);
            }
            if (bus.highpassHz > 0.0f)
            {
                AudioBusEffectDesc effect;
                effect.kind = AudioBusEffectKind::Highpass;
                effect.frequencyHz = bus.highpassHz;
                out.effects.PushBack(effect);
            }
            if (bus.delaySeconds > 0.0f)
            {
                AudioBusEffectDesc effect;
                effect.kind = AudioBusEffectKind::Delay;
                effect.delaySeconds = bus.delaySeconds;
                effect.delayDecay = Clamp(bus.delayDecay, 0.0f, 0.99f);
                if (bus.delayDecay >= 1.0f)
                {
                    LOG_WARNING(
                        u8"Audio",
                        u8"bus layout '{}': delayDecay >= 1 self-oscillates - clamped to 0.99",
                        assetName);
                }
                out.effects.PushBack(effect);
            }
            if (bus.reverbWet > 0.0f)
            {
                AudioBusEffectDesc effect;
                effect.kind = AudioBusEffectKind::Reverb;
                effect.roomSize = Clamp(bus.reverbRoomSize, 0.0f, 1.0f);
                effect.damping = Clamp(bus.reverbDamping, 0.0f, 1.0f);
                effect.wetLevel = Clamp(bus.reverbWet, 0.0f, 1.0f);
                out.effects.PushBack(effect);
            }
        }
    };

    // ---- sound cue asset: weighted clip variants, edited via SoundCuePage ----

    inline constexpr usize kSoundCueSlotCount = 8;

    class SoundCueAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(SoundCueAsset, pipeline::Asset)
    public:
        Guid clipIds[kSoundCueSlotCount]{};
        f32 weights[kSoundCueSlotCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        u8 mode = 0; // SoundCueMode
        f32 pitchMin = 1.0f;
        f32 pitchMax = 1.0f;
        f32 volumeMin = 1.0f;
        f32 volumeMax = 1.0f;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            u32 slots = kSoundCueSlotCount;
            foundation::core::Serialize(ar, "slots", slots);
            const u32 count = Min<u32>(slots, kSoundCueSlotCount);
            for (u32 i = 0; i < count; ++i)
            {
                ar.Key("clip");
                ar.GuidValue(clipIds[i]);
                foundation::core::Serialize(ar, "weight", weights[i]);
            }
            foundation::core::Serialize(ar, "mode", mode);
            foundation::core::Serialize(ar, "pitchMin", pitchMin);
            foundation::core::Serialize(ar, "pitchMax", pitchMax);
            foundation::core::Serialize(ar, "volumeMin", volumeMin);
            foundation::core::Serialize(ar, "volumeMax", volumeMax);
        }
    };

    class SoundCueAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &SoundCueAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SoundCueSource::StaticType();
        }
        // v2: an empty cue cooks to a valid zero-variant product instead of failing, so
        // draft cues re-cook into products.
        [[nodiscard]] u32 Version() const override { return 2; }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const auto& cueAsset = static_cast<const SoundCueAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            SoundCueSource source;
            for (usize i = 0; i < kSoundCueSlotCount; ++i)
            {
                if (cueAsset.clipIds[i].IsNil())
                {
                    continue;
                }
                if (cueAsset.weights[i] <= 0.0f)
                {
                    LOG_WARNING(
                        u8"Audio", u8"sound cue slot {} has a clip but weight <= 0 - slot disabled",
                        i);
                    continue;
                }
                SoundCueSource::Variant variant;
                variant.clipId = cueAsset.clipIds[i];
                variant.weight = cueAsset.weights[i];
                source.variants.PushBack(variant);
            }
            // A cue with no playable variant cooks to a valid ZERO-VARIANT product (not a failure):
            // a freshly-created / draft cue must never poison Cook All / export, and the runtime is
            // built for it (ResolveSoundCue answers variantIndex = -1, every consumer guards it -> a
            // silent no-op). The draft state is surfaced in the editor (SoundCuePage status) and
            // audited by project_health, NOT by failing the cook.
            source.mode = cueAsset.mode;
            source.pitchMin = Min(cueAsset.pitchMin, cueAsset.pitchMax);
            source.pitchMax = Max(cueAsset.pitchMin, cueAsset.pitchMax);
            source.volumeMin = Min(cueAsset.volumeMin, cueAsset.volumeMax);
            source.volumeMax = Max(cueAsset.volumeMin, cueAsset.volumeMax);
            return ctx.output->WriteObject(source);
        }
    };

    // Registers the asset type for content-DB construction + deserialization.
    inline void RegisterAudioAssets()
    {
        GlobalTypeRegistry().Register(AudioClipAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<AudioClipAsset>();
        GlobalTypeRegistry().Register(AudioBusLayoutAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<AudioBusLayoutAsset>();
        GlobalTypeRegistry().Register(SoundCueAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<SoundCueAsset>();
    }

    // AudioClipAsset's StaticType() is defined WITH reflected properties in AudioAssetImpl.cpp.
    // The remaining audio assets stay identity-only (bus layout / sound cue carry nested
    // structure that a flat property pass doesn't cover).
    RTTI_DEFINE_OBJECT(AudioImportOptions, "rtti::pipeline::audio")
    // 2: the custom-bus slot bank (see Serialize).

    /// File > New's audio creators (pipeline.registration composes every domain's).
    void RegisterAudioCreators(AssetCreatorRegistry& registry);
}
