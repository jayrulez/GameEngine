// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Audio.Resource - the `foundation.audio.resource` module.
//
// Cooked audio content:
//   * AudioClipSource: the cooked record, holding probed metadata and import intent, with
//     the original compressed container bytes in the instance's "data" stream (never decoded
//     PCM sidecars; the record keeps the container extension as a decoder hint).
//   * AudioClipFactory: builds the runtime foundation::audio::AudioClip a component's
//     Ref<> binds. Non-streamed clips load the container bytes into memory; streamed
//     clips get a ContentInstanceStreamSource so miniaudio pages the bytes on demand
//     straight out of the content mount (pak included, since Instance::ReadData seeks).
//
// AudioBusLayoutSource and AudioBusLayoutResource: the mixer as data, with per-bus volume,
// mute and effect chains over the four fixed buses (the AudioBus enum stays the addressing
// model), plus named custom buses. SoundCueSource: a cooked weighted-variant cue.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.audio.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.audio;

using namespace foundation::core;
namespace resource = foundation::resource;

export namespace foundation::audio
{
    // Cooked clip record. The container bytes live in the "data" stream beside it.
    class AudioClipSource : public ISerializable
    {
        RTTI_OBJECT(AudioClipSource, ISerializable)
    public:
        u32 channels = 0;
        u32 sampleRate = 0;
        u64 frameCount = 0;
        f32 durationSeconds = 0.0f;
        f32 gain = 1.0f;
        bool loop = false;
        u64 loopStartFrame = 0;
        u64 loopEndFrame = 0; // 0 = clip end
        bool stream = false;
        bool keepCompressed = false;
        String containerExtension; // recorded decoder hint ("wav"/"ogg"/"mp3"/"flac")

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "channels", channels);
            foundation::core::Serialize(ar, "sampleRate", sampleRate);
            foundation::core::Serialize(ar, "frameCount", frameCount);
            foundation::core::Serialize(ar, "durationSeconds", durationSeconds);
            foundation::core::Serialize(ar, "gain", gain);
            foundation::core::Serialize(ar, "loop", loop);
            foundation::core::Serialize(ar, "loopStartFrame", loopStartFrame);
            foundation::core::Serialize(ar, "loopEndFrame", loopEndFrame);
            foundation::core::Serialize(ar, "stream", stream);
            foundation::core::Serialize(ar, "keepCompressed", keepCompressed);
            foundation::core::Serialize(ar, "containerExtension", containerExtension);
        }
    };

    // Re-openable stream source over a cooked instance's "data" stream: each play opens
    // an independent seekable stream through the content mount (native dir or pak).
    // The content database owns the instance and outlives every resource product.
    class ContentInstanceStreamSource final : public IAudioStreamSource
    {
    public:
        explicit ContentInstanceStreamSource(foundation::content::Instance& instance) noexcept
            : m_instance(&instance)
        {
        }

        [[nodiscard]] UniquePtr<IStream> OpenStream() override
        {
            return m_instance->ReadData(u8"data");
        }

    private:
        foundation::content::Instance* m_instance;
    };

    class AudioClipFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required: the
        // application that registers the factory decides).
        explicit AudioClipFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioClip::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &AudioClipSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            return BuildClip(instance);
        }

        // Async: the AudioClip is a pure-CPU product (encoded bytes in memory, or a lazy content
        // stream that pages on demand), so the whole build runs on a worker and FinalizeStage is
        // a no-op. This is safe because content-DB reads open independent streams, the type and
        // serializable registries are read-only during load, and AudioClip's type is
        // force-initialized on the main thread (see RegisterAudioResource). The instance outlives
        // the product, so a streamed clip's ContentInstanceStreamSource stays valid.
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override
        {
            return BuildClip(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(resource::ResourceManager&,
                                                   RefPtr<Object> decoded) override
        {
            return decoded; // nothing is main-thread-specific for audio
        }

    private:
        [[nodiscard]] RefPtr<Object> BuildClip(foundation::content::Instance& instance) const
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            AudioClipSource* source = Cast<AudioClipSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }

            RefPtr<AudioClip> clip = MakeRef<AudioClip>(*m_allocator);
            clip->channels = source->channels;
            clip->sampleRate = source->sampleRate;
            clip->frameCount = source->frameCount;
            clip->durationSeconds = source->durationSeconds;
            clip->gain = source->gain;
            clip->loop = source->loop;
            clip->loopStartFrame = source->loopStartFrame;
            clip->loopEndFrame = source->loopEndFrame;
            clip->stream = source->stream;
            clip->keepCompressed = source->keepCompressed;

            if (source->stream)
            {
                clip->streamSource = UniquePtr<IAudioStreamSource>(
                    m_allocator->New<ContentInstanceStreamSource>(instance),
                    *m_allocator);
            }
            else
            {
                UniquePtr<IStream> data = instance.ReadData(u8"data");
                if (data.Get() == nullptr)
                {
                    return RefPtr<Object>{};
                }
                const i64 size = data->Size();
                if (size <= 0)
                {
                    return RefPtr<Object>{};
                }
                clip->encodedData.Resize(static_cast<usize>(size));
                if (data->Read(clip->encodedData.Data(), static_cast<u64>(size)) !=
                    static_cast<u64>(size))
                {
                    return RefPtr<Object>{};
                }
            }
            return clip;
        }

    private:
        IAllocator* m_allocator;
    };

    // ---- bus layout: cooked mixer data ----

    // The wire shape is generic (per-bus effect arrays) even though the editor asset is
    // flat, so a richer chain editor needs no wire change. It also carries the named
    // custom-bus array (name, parent, settings).
    class AudioBusLayoutSource : public ISerializable
    {
        RTTI_OBJECT(AudioBusLayoutSource, ISerializable)
    public:
        AudioBusLayout layout;

        void Serialize(ISerializer& ar) override
        {
            auto serializeSettings = [&ar](AudioBusSettings& settings)
            {
                foundation::core::Serialize(ar, "volume", settings.volume);
                foundation::core::Serialize(ar, "muted", settings.muted);
                u32 effectCount = static_cast<u32>(settings.effects.Size());
                foundation::core::Serialize(ar, "effectCount", effectCount);
                if (ar.Mode() == SerializeMode::Read)
                {
                    settings.effects.Resize(effectCount);
                }
                for (u32 i = 0; i < effectCount; ++i)
                {
                    AudioBusEffectDesc& effect = settings.effects[i];
                    u8 kind = static_cast<u8>(effect.kind);
                    foundation::core::Serialize(ar, "kind", kind);
                    effect.kind = static_cast<AudioBusEffectKind>(kind);
                    foundation::core::Serialize(ar, "frequencyHz", effect.frequencyHz);
                    foundation::core::Serialize(ar, "delaySeconds", effect.delaySeconds);
                    foundation::core::Serialize(ar, "delayDecay", effect.delayDecay);
                    foundation::core::Serialize(ar, "roomSize", effect.roomSize);
                    foundation::core::Serialize(ar, "damping", effect.damping);
                    foundation::core::Serialize(ar, "wetLevel", effect.wetLevel);
                }
            };

            u32 busCount = static_cast<u32>(AudioBus::Count);
            foundation::core::Serialize(ar, "busCount", busCount);
            const u32 buses = Min(busCount, static_cast<u32>(AudioBus::Count));
            for (u32 bus = 0; bus < buses; ++bus)
            {
                serializeSettings(layout.buses[bus]);
            }

            u32 customCount = static_cast<u32>(layout.customBuses.Size());
            foundation::core::Serialize(ar, "customBusCount", customCount);
            if (ar.Mode() == SerializeMode::Read)
            {
                layout.customBuses.Resize(customCount);
            }
            for (u32 i = 0; i < customCount; ++i)
            {
                AudioNamedBus& named = layout.customBuses[i];
                foundation::core::Serialize(ar, "name", named.name);
                foundation::core::Serialize(ar, "parent", named.parent);
                serializeSettings(named.settings);
            }
        }
    };

    // Runtime product a project's defaultBusLayoutId resolves to.
    class AudioBusLayoutResource final : public Object
    {
        RTTI_OBJECT(AudioBusLayoutResource, Object)
    public:
        AudioBusLayout layout;
    };

    class AudioBusLayoutFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required: the
        // application that registers the factory decides).
        explicit AudioBusLayoutFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioBusLayoutResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &AudioBusLayoutSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            AudioBusLayoutSource* source = Cast<AudioBusLayoutSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AudioBusLayoutResource> resource =
                MakeRef<AudioBusLayoutResource>(*m_allocator);
            resource->layout = source->layout;
            return resource;
        }

    private:
        IAllocator* m_allocator;
    };

    // ---- sound cue: weighted-variant container, cooked ----

    class SoundCueSource : public ISerializable
    {
        RTTI_OBJECT(SoundCueSource, ISerializable)
    public:
        struct Variant
        {
            Guid clipId;
            f32 weight = 1.0f;
        };
        Array<Variant> variants;
        u8 mode = 0; // SoundCueMode
        f32 pitchMin = 1.0f;
        f32 pitchMax = 1.0f;
        f32 volumeMin = 1.0f;
        f32 volumeMax = 1.0f;

        void Serialize(ISerializer& ar) override
        {
            u32 count = static_cast<u32>(variants.Size());
            foundation::core::Serialize(ar, "variantCount", count);
            if (ar.Mode() == SerializeMode::Read)
            {
                variants.Resize(count);
            }
            for (u32 i = 0; i < count; ++i)
            {
                ar.Key("clip");
                ar.GuidValue(variants[i].clipId);
                foundation::core::Serialize(ar, "weight", variants[i].weight);
            }
            foundation::core::Serialize(ar, "mode", mode);
            foundation::core::Serialize(ar, "pitchMin", pitchMin);
            foundation::core::Serialize(ar, "pitchMax", pitchMax);
            foundation::core::Serialize(ar, "volumeMin", volumeMin);
            foundation::core::Serialize(ar, "volumeMax", volumeMax);
        }

    };

    class SoundCueFactory final : public resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required: the
        // application that registers the factory decides).
        explicit SoundCueFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SoundCue::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &SoundCueSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            SoundCueSource* source = Cast<SoundCueSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<SoundCue> cue = MakeRef<SoundCue>(*m_allocator);
            cue->mode = static_cast<SoundCueMode>(source->mode);
            cue->pitchMin = source->pitchMin;
            cue->pitchMax = source->pitchMax;
            cue->volumeMin = source->volumeMin;
            cue->volumeMax = source->volumeMax;
            for (const SoundCueSource::Variant& variant : source->variants)
            {
                SoundCueVariant out;
                if (!variant.clipId.IsNil())
                {
                    out.clip = RefPtr<AudioClip>(manager.Bind<AudioClip>(variant.clipId).Get());
                }
                out.weight = variant.weight;
                cue->variants.PushBack(Move(out));
            }
            return cue;
        }

        IAllocator* m_allocator;
    };

    // Registers the cooked record and product types (content-DB construction by type name).
    inline void RegisterAudioResource()
    {
        GlobalTypeRegistry().Register(AudioClipSource::StaticType());
        RegisterSerializable<AudioClipSource>();
        GlobalTypeRegistry().Register(AudioClip::StaticType());
        GlobalTypeRegistry().Register(AudioBusLayoutSource::StaticType());
        RegisterSerializable<AudioBusLayoutSource>();
        GlobalTypeRegistry().Register(AudioBusLayoutResource::StaticType());
        GlobalTypeRegistry().Register(SoundCueSource::StaticType());
        RegisterSerializable<SoundCueSource>();
        GlobalTypeRegistry().Register(SoundCue::StaticType());
    }

    RTTI_DEFINE_OBJECT(AudioClipSource, "rtti::audio")
    // 2: the named custom-bus section (see Serialize).
    RTTI_DEFINE_OBJECT_VERSIONED(AudioBusLayoutSource, "rtti::audio", 2)
    RTTI_DEFINE_OBJECT(AudioBusLayoutResource, "rtti::audio")
    RTTI_DEFINE_OBJECT(SoundCueSource, "rtti::audio")
}

export namespace foundation::audio
{
    /// The audio resource module: the engine composition composes this library's factories
    /// from it.
    inline constexpr foundation::resource::ResourceFactoryDesc kAudioResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<AudioClip, AudioClipSource, AudioClipFactory>(),
        foundation::resource::FactoryWithAllocator<AudioBusLayoutResource, AudioBusLayoutSource, AudioBusLayoutFactory>(),
        foundation::resource::FactoryWithAllocator<SoundCue, SoundCueSource, SoundCueFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kAudioResourceModule{
        u8"audio", &RegisterAudioResource, kAudioResourceFactories,
        sizeof(kAudioResourceFactories) / sizeof(kAudioResourceFactories[0])};
}
