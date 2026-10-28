// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Audio - :thumbnail_generator partition
//
// The audio-domain thumbnail generator: a waveform strip. Prepare (main thread) reads the
// clip's encoded source bytes from the Sources/ mount; Generate (light worker) buckets peak
// magnitudes per column via the engine-free decode helper (streams in chunks - full-PCM
// decode of a long clip would balloon) and draws symmetric bars in the clip page's teal over
// the tile ground.

module;
#include "Core/Prelude.h"

export module editor.audio:thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.vfs;
import foundation.audio;
import editor.core;
import audio.pipeline;

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;
namespace audio = foundation::audio;

export namespace editor
{
    class AudioClipThumbnailGenerator final : public editor::IThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"AudioClipAsset"};
            return Span<const StringView>(kTypes, 1);
        }

        [[nodiscard]] Status Prepare(content::Instance& instance,
                                     foundation::vfs::IFileSystem& sources,
                                     editor::ThumbnailPrepared& out) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::AudioClipAsset>(object.Get());
            if (asset == nullptr || asset->fileName.IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<IStream> stream = sources.Open(asset->fileName.View(), FileMode::Read);
            if (stream.Get() == nullptr || !stream->IsValid() || stream->Size() <= 0)
            {
                return Status{ErrorCode::NotFound};
            }
            out.stream = Move(stream); // the worker reads the file
            return Status{};
        }

        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            constexpr u32 kTile = editor::kThumbnailServiceThumbnailSize;
            Array<f32> peaks;
            if (!audio::BuildWaveformPeaks(Span<const byte>(payload.Data(), payload.Size()),
                                           kTile, peaks) ||
                peaks.Size() != kTile)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            constexpr u8 kGround[3] = {26, 28, 33};
            constexpr u8 kBar[3] = {64, 200, 190}; // the clip page's waveform teal
            out = image::Image(kTile, kTile, image::PixelFormat::RGBA8);
            Span<u8> dst = out.PixelDataMut();
            for (usize i = 0; i < dst.Size(); i += 4)
            {
                dst[i + 0] = kGround[0];
                dst[i + 1] = kGround[1];
                dst[i + 2] = kGround[2];
                dst[i + 3] = 255;
            }

            constexpr i32 kMid = static_cast<i32>(kTile) / 2;
            for (u32 x = 0; x < kTile; ++x)
            {
                // A 1px midline survives silence so an empty-sounding clip still reads as audio.
                const i32 half =
                    Max(1, static_cast<i32>(peaks[x] * (kMid - 2)));
                for (i32 y = kMid - half; y < kMid + half; ++y)
                {
                    u8* texel = dst.Data() + (static_cast<usize>(y) * kTile + x) * 4;
                    texel[0] = kBar[0];
                    texel[1] = kBar[1];
                    texel[2] = kBar[2];
                }
            }
            return Status{};
        }
    };

    /// Registered by RegisterAudioClipEditor (the domain's one composition entry point).
    inline void RegisterAudioThumbnailGenerator(editor::ThumbnailService& service)
    {
        service.RegisterGenerator(MakeUnique<AudioClipThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
