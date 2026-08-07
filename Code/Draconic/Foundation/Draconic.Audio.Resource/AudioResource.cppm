// Draconic::AudioResource - the `draconic.audio.resource` module.
//
// Cooked audio content (docs/design/audio.md §4):
//   * AudioClipSource - the cooked record: probed metadata + import intent, with the
//     ORIGINAL compressed container bytes in the instance's "data" stream (Traktor's
//     compressed-cook model - never PCM sidecars; miniaudio records/sniffs the decoder).
//   * AudioClipFactory - builds the runtime draconic::audio::AudioClip a component's
//     Ref<> binds. Non-streamed clips load the container bytes into memory; streamed
//     clips get a ContentInstanceStreamSource so miniaudio pages the bytes on demand
//     straight out of the content mount (pak included - Instance::ReadData seeks).
//
// AudioBusLayoutSource/Resource (P2): the mixer as data - per-bus volume/mute/effect
// chains over the FIXED four-bus topology (the AudioBus enum stays the addressing
// model; free-form named trees are a later migration).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.audio.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;
import draconic.audio;

using namespace draconic::foundation;
namespace resource = draconic::resource;

export namespace draconic::audio
{
    // Cooked clip record. The container bytes live in the "data" stream beside it.
    class AudioClipSource : public ISerializable
    {
        DRACONIC_OBJECT(AudioClipSource, ISerializable)
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
            draconic::foundation::Serialize(ar, "channels", channels);
            draconic::foundation::Serialize(ar, "sampleRate", sampleRate);
            draconic::foundation::Serialize(ar, "frameCount", frameCount);
            draconic::foundation::Serialize(ar, "durationSeconds", durationSeconds);
            draconic::foundation::Serialize(ar, "gain", gain);
            draconic::foundation::Serialize(ar, "loop", loop);
            draconic::foundation::Serialize(ar, "loopStartFrame", loopStartFrame);
            draconic::foundation::Serialize(ar, "loopEndFrame", loopEndFrame);
            draconic::foundation::Serialize(ar, "stream", stream);
            draconic::foundation::Serialize(ar, "keepCompressed", keepCompressed);
            draconic::foundation::Serialize(ar, "containerExtension", containerExtension);
        }
    };

    // Re-openable stream source over a cooked instance's "data" stream: each play opens
    // an independent seekable stream through the content mount (native dir or pak).
    // The content database owns the instance and outlives every resource product.
    class ContentInstanceStreamSource final : public IAudioStreamSource
    {
    public:
        explicit ContentInstanceStreamSource(draconic::content::Instance& instance) noexcept
            : m_instance(&instance)
        {
        }

        [[nodiscard]] UniquePtr<IStream> OpenStream() override
        {
            return m_instance->ReadData(u8"data");
        }

    private:
        draconic::content::Instance* m_instance;
    };

    class AudioClipFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioClip::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            return BuildClip(instance);
        }

        // Async (task #123): the AudioClip is a pure-CPU product (encoded bytes in memory, or a
        // lazy content stream that pages on demand), so the entire build runs on a worker and
        // FinalizeStage is a no-op. Safe: content-DB reads open independent streams and the type/
        // serializable registries are read-only during load; AudioClip's type is force-initialized
        // on the main thread (see RegisterAudioResource). The instance outlives the product, so a
        // streamed clip's ContentInstanceStreamSource stays valid.
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            return BuildClip(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(resource::ResourceManager&,
                                                   RefPtr<Object> decoded) override
        {
            return decoded; // nothing is main-thread-specific for audio
        }

    private:
        [[nodiscard]] static RefPtr<Object> BuildClip(draconic::content::Instance& instance)
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            AudioClipSource* source = Cast<AudioClipSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }

            RefPtr<AudioClip> clip = MakeRef<AudioClip>(DefaultAllocator());
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
                    DefaultAllocator().New<ContentInstanceStreamSource>(instance),
                    DefaultAllocator());
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
    };

    // ---- bus layout (P2): cooked mixer data ----

    // Wire shape stays GENERIC (per-bus effect arrays) even though the editor asset is
    // flat v1 - a richer chain editor later needs no wire change. Data v2 appends the
    // NAMED custom-bus array (name/parent/settings); v0/v1 cooks simply have none -
    // the ar.Version() gate keeps old cooks loading.
    class AudioBusLayoutSource : public ISerializable
    {
        DRACONIC_OBJECT(AudioBusLayoutSource, ISerializable)
    public:
        AudioBusLayout layout;

        void Serialize(ISerializer& ar) override
        {
            auto serializeSettings = [&ar](AudioBusSettings& settings)
            {
                draconic::foundation::Serialize(ar, "volume", settings.volume);
                draconic::foundation::Serialize(ar, "muted", settings.muted);
                u32 effectCount = static_cast<u32>(settings.effects.Size());
                draconic::foundation::Serialize(ar, "effectCount", effectCount);
                if (ar.Mode() == SerializeMode::Read)
                {
                    settings.effects.Resize(effectCount);
                }
                for (u32 i = 0; i < effectCount; ++i)
                {
                    AudioBusEffectDesc& effect = settings.effects[i];
                    u8 kind = static_cast<u8>(effect.kind);
                    draconic::foundation::Serialize(ar, "kind", kind);
                    effect.kind = static_cast<AudioBusEffectKind>(kind);
                    draconic::foundation::Serialize(ar, "frequencyHz", effect.frequencyHz);
                    draconic::foundation::Serialize(ar, "delaySeconds", effect.delaySeconds);
                    draconic::foundation::Serialize(ar, "delayDecay", effect.delayDecay);
                    draconic::foundation::Serialize(ar, "roomSize", effect.roomSize);
                    draconic::foundation::Serialize(ar, "damping", effect.damping);
                    draconic::foundation::Serialize(ar, "wetLevel", effect.wetLevel);
                }
            };

            u32 busCount = static_cast<u32>(AudioBus::Count);
            draconic::foundation::Serialize(ar, "busCount", busCount);
            const u32 buses = Min(busCount, static_cast<u32>(AudioBus::Count));
            for (u32 bus = 0; bus < buses; ++bus)
            {
                serializeSettings(layout.buses[bus]);
            }

            if (ar.Version() >= 2) // v2: named custom buses (generic, growable)
            {
                u32 customCount = static_cast<u32>(layout.customBuses.Size());
                draconic::foundation::Serialize(ar, "customBusCount", customCount);
                if (ar.Mode() == SerializeMode::Read)
                {
                    layout.customBuses.Resize(customCount);
                }
                for (u32 i = 0; i < customCount; ++i)
                {
                    AudioNamedBus& named = layout.customBuses[i];
                    draconic::foundation::Serialize(ar, "name", named.name);
                    draconic::foundation::Serialize(ar, "parent", named.parent);
                    serializeSettings(named.settings);
                }
            }
        }
    };

    // Runtime product a project's defaultBusLayoutId resolves to.
    class AudioBusLayoutResource final : public Object
    {
        DRACONIC_OBJECT(AudioBusLayoutResource, Object)
    public:
        AudioBusLayout layout;
    };

    class AudioBusLayoutFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AudioBusLayoutResource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            AudioBusLayoutSource* source = Cast<AudioBusLayoutSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AudioBusLayoutResource> resource =
                MakeRef<AudioBusLayoutResource>(DefaultAllocator());
            resource->layout = source->layout;
            return resource;
        }
    };

    // ---- sound cue (P3): weighted-variant container, cooked ----

    class SoundCueSource : public ISerializable
    {
        DRACONIC_OBJECT(SoundCueSource, ISerializable)
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
            draconic::foundation::Serialize(ar, "variantCount", count);
            if (ar.Mode() == SerializeMode::Read)
            {
                variants.Resize(count);
            }
            for (u32 i = 0; i < count; ++i)
            {
                ar.Key("clip");
                ar.GuidValue(variants[i].clipId);
                draconic::foundation::Serialize(ar, "weight", variants[i].weight);
            }
            draconic::foundation::Serialize(ar, "mode", mode);
            draconic::foundation::Serialize(ar, "pitchMin", pitchMin);
            draconic::foundation::Serialize(ar, "pitchMax", pitchMax);
            draconic::foundation::Serialize(ar, "volumeMin", volumeMin);
            draconic::foundation::Serialize(ar, "volumeMax", volumeMax);
        }
    };

    class SoundCueFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SoundCue::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            draconic::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            SoundCueSource* source = Cast<SoundCueSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<SoundCue> cue = MakeRef<SoundCue>(DefaultAllocator());
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
    };

    // Registers the cooked record + product types (content-DB construction by type name).
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

    DRACONIC_DEFINE_OBJECT(AudioClipSource, "draconic::audio")
    // v2: the named custom-bus section (see Serialize) - old cooks read as version 0.
    DRACONIC_DEFINE_OBJECT_VERSIONED(AudioBusLayoutSource, "draconic::audio", 2)
    DRACONIC_DEFINE_OBJECT(AudioBusLayoutResource, "draconic::audio")
    DRACONIC_DEFINE_OBJECT(SoundCueSource, "draconic::audio")
}
