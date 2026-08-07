// Draconic::Audio - :clip partition.
//
// AudioClip: the RUNTIME clip object (docs/design/audio.md §4) - metadata plus the
// ORIGINAL compressed container bytes (wav/ogg/mp3/flac; Traktor's compressed-in-memory
// model, never PCM sidecars). Streamed clips carry a re-openable byte source instead of
// bytes, so the engine can page-decode them straight out of the cooked content mount
// (pak included) through the ma_vfs bridge. The cooked record + factory live in
// draconic.audio.resource; this type stays content-DB-agnostic.
//
// The codec helpers (probe/decode/encode) are the editor/builder seam: declared here,
// implemented in EngineImpl.cpp (the module implementation unit that talks to miniaudio),
// so tooling modules never include miniaudio themselves.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.audio:clip;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::audio
{
    // Probed shape of an encoded audio payload.
    struct AudioClipMetadata
    {
        u32 channels = 0;
        u32 sampleRate = 0;
        u64 frameCount = 0;
        f32 durationSeconds = 0.0f;
    };

    /// A re-openable byte source for a STREAMED clip. Each open returns an independent
    /// seekable stream over the same encoded container bytes (miniaudio's resource
    /// manager opens one per playing stream voice and pages through it on job threads).
    class IAudioStreamSource
    {
    public:
        virtual ~IAudioStreamSource() = default;
        [[nodiscard]] virtual UniquePtr<IStream> OpenStream() = 0;
    };

    // The runtime clip. In-memory clips hold the encoded container bytes; streamed clips
    // hold a stream source. `keepCompressed` (open question 2): in-memory clips decode
    // once at first play by default (decode-on-load); flagged clips stay compressed in
    // memory and decode on the fly while playing (large-but-latency-tolerant sounds).
    class AudioClip : public Object
    {
        DRACONIC_OBJECT(AudioClip, Object)
    public:
        u32 channels = 0;
        u32 sampleRate = 0;
        u64 frameCount = 0;
        f32 durationSeconds = 0.0f;
        f32 gain = 1.0f;        // authored gain, folded into every voice's volume
        bool loop = false;      // default loop intent (a play request may override)
        u64 loopStartFrame = 0; // loop points (loopEndFrame 0 = clip end)
        u64 loopEndFrame = 0;
        bool stream = false; // decode on the fly from `streamSource`
        bool keepCompressed = false;
        Array<byte> encodedData;                    // original container bytes (!stream)
        UniquePtr<IAudioStreamSource> streamSource; // re-openable source (stream)

        [[nodiscard]] Span<const byte> EncodedBytes() const noexcept
        {
            return Span<const byte>(encodedData.Data(), encodedData.Size());
        }
    };

    // ---- codec helpers (implemented over miniaudio in EngineImpl.cpp) ----

    /// Probes channels/rate/length of an encoded container (wav/ogg/mp3/flac).
    /// False when the bytes are not decodable audio.
    [[nodiscard]] bool ProbeAudioClipMetadata(Span<const byte> encodedBytes,
                                              AudioClipMetadata& outMetadata);

    /// Decodes an encoded container to interleaved 16-bit PCM. `targetChannels` 0 keeps
    /// the source channel count; 1 downmixes to mono (the force-mono import path).
    [[nodiscard]] bool DecodeAudioClipToPcm16(Span<const byte> encodedBytes, u32 targetChannels,
                                              Array<i16>& outInterleavedSamples,
                                              AudioClipMetadata& outMetadata);

    /// Encodes interleaved 16-bit PCM as a WAV container (the write-back half of the
    /// import transforms - trim/normalize/force-mono re-encode losslessly to WAV).
    [[nodiscard]] bool EncodeWavFromPcm16(Span<const i16> interleavedSamples, u32 channels,
                                          u32 sampleRate, Array<byte>& outWavBytes);

    DRACONIC_DEFINE_OBJECT(AudioClip, "draconic::audio")
}
