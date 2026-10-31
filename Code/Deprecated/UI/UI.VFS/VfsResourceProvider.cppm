// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - `foundation.ui.vfs`: an IResourceProvider backed by a VFS filesystem.
//
// The IO model that backs the UI's resource provider: StyleSheetLoader (@import / @icon SVG text) and
// the image and nine-slice drawable factories resolve external files through this. It is a separate
// module so the core foundation.ui stays free of a VFS dependency; the app wires this provider into
// StyleSheetLoader. The filesystem is borrowed (the caller owns it); the provider owns the decoded
// images it returns, in m_images.

module;
#include "Core/Prelude.h"

export module foundation.ui.vfs;

import foundation.core;     // IStream, FileMode, SeekOrigin, Array, String, Span, UniquePtr
import foundation.image;    // ImageData, OwnedImageData
import foundation.image.io; // LoadImageFromMemory
import foundation.vfs;      // IFileSystem
import foundation.ui;       // IResourceProvider

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui::vfs
{
    /// IResourceProvider that loads resources from a VFS filesystem. The caller owns the filesystem;
    /// this provider does not delete it.
    class VfsResourceProvider final : public IResourceProvider
    {
    public:
        explicit VfsResourceProvider(foundation::vfs::IFileSystem* fs) noexcept : m_fs(fs) {}

        /// Load text content from a path relative to the filesystem root. Empty files succeed.
        bool LoadText(StringView path, String& outText) override
        {
            if (m_fs == nullptr)
            {
                return false;
            }
            UniquePtr<IStream> stream = m_fs->Open(path, FileMode::Read);
            if (!stream)
            {
                return false;
            }

            const i64 length = stream->Size();
            if (length <= 0)
            {
                return true;
            }

            Array<u8> buf;
            buf.Resize(static_cast<usize>(length));
            const u64 read = stream->Read(buf.Data(), static_cast<u64>(length));
            outText.Append(reinterpret_cast<const char8_t*>(buf.Data()), static_cast<usize>(read));
            return true;
        }

        /// Load and decode image data from a path. Returns a borrowed pointer owned by this provider
        /// (cached in m_images), or null if the file is missing or fails to decode.
        const image::ImageData* LoadImage(StringView path) override
        {
            if (m_fs == nullptr)
            {
                return nullptr;
            }
            UniquePtr<IStream> stream = m_fs->Open(path, FileMode::Read);
            if (!stream)
            {
                return nullptr;
            }

            const i64 length = stream->Size();
            if (length <= 0)
            {
                return nullptr;
            }

            Array<u8> buf;
            buf.Resize(static_cast<usize>(length));
            const u64 read = stream->Read(buf.Data(), static_cast<u64>(length));

            image::Image decoded;
            if (!image::io::LoadImageFromMemory(
                     Span<const u8>(buf.Data(), static_cast<usize>(read)), decoded)
                     .IsOk())
            {
                return nullptr;
            }

            UniquePtr<image::OwnedImageData> owned = MakeUnique<image::OwnedImageData>(
                DefaultAllocator(), decoded.Width(), decoded.Height(), decoded.Format(),
                decoded.PixelData());
            image::OwnedImageData* raw = owned.Get();
            m_images.PushBack(Move(owned));
            return raw;
        }

    private:
        foundation::vfs::IFileSystem* m_fs;
        Array<UniquePtr<image::OwnedImageData>> m_images; // decoded images owned by this provider
    };
}
