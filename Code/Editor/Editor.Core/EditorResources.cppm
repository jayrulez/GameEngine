// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core - :editor_resources partition.
//
// The editor's own files: its sheets, markup and icons under Data/Editor and the fonts they name,
// given to the GUI as its resource provider and reloaded as they change on disk, which makes the
// editor's development loop. Text (a sheet, a page's markup) is read when it is asked for, so a
// second read gets the file as it is now. Images and SVGs are kept once loaded, because the GUI
// borrows them; one whose file changed is set aside (still alive for whatever draws it) and loaded
// afresh when next asked for.
//
// Poll sweeps the files for changes (the caller throttles it, since a sweep walks the folder). A
// change notifies TrackChanges' observable, so a sheet or a page that read through it in a GUI
// Effect reloads itself; LastChanged says which files changed.
module;
#include "Core/Prelude.h"

export module editor.core:editor_resources;

import foundation.core;
import foundation.image;
import foundation.image.io;
import foundation.vfs;
import foundation.vg.svg;
import foundation.gui; // IResourceProvider, Observable

using namespace foundation::core;
namespace gui = foundation::gui;
namespace image = foundation::image;
namespace svg = foundation::vg::svg;

export namespace editor
{
    class EditorResources final : public gui::IResourceProvider
    {
    public:
        /// Over the folder `root` (the data root, which holds Editor/); paths are relative to it
        /// ("Editor/Icons/save.svg").
        EditorResources(IAllocator& allocator, StringView root)
            : IResourceProvider(allocator), m_allocator(allocator), m_fs(root, allocator), m_images(allocator), m_svgs(allocator),
              m_setAside(allocator), m_lastChanged(allocator)
        {
            if (foundation::vfs::IChangeSource* source = m_fs.ChangeSource())
                source->Track(StringView{});
        }

        // === IResourceProvider ===
        const image::ImageData* LoadImage(StringView path) override
        {
            for (const ImageEntry& e : m_images)
                if (e.Path.AsView() == path)
                    return e.Pixels.Get();
            Array<byte> bytes;
            if (!ReadBytes(path, bytes))
                return nullptr; // not cached: a later request tries again, since the file may have arrived
            image::Image decoded;
            if (!image::io::LoadImageFromMemory(Span<const u8>(reinterpret_cast<const u8*>(bytes.Data()), bytes.Size()), decoded).IsOk())
                return nullptr;
            UniquePtr<image::OwnedImageData> owned =
                MakeUnique<image::OwnedImageData>(m_allocator, decoded.Width(), decoded.Height(), decoded.Format(), decoded.PixelData());
            image::OwnedImageData* raw = owned.Get();
            m_images.PushBack(ImageEntry{String(path), Move(owned)});
            return raw;
        }
        bool LoadText(StringView path, String& out) override
        {
            Array<byte> bytes;
            if (!ReadBytes(path, bytes))
                return false;
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()));
            return true;
        }
        const svg::SVGDocument* LoadSvg(StringView path) override
        {
            for (const SvgEntry& e : m_svgs)
                if (e.Path.AsView() == path)
                    return e.Document.Get();
            String text;
            if (!LoadText(path, text))
                return nullptr;
            Result<svg::SVGDocument> parsed = svg::SVGLoader::Load(text.AsView());
            if (!parsed.HasValue())
                return nullptr;
            m_svgs.PushBack(SvgEntry{String(path), MakeUnique<svg::SVGDocument>(m_allocator, Move(parsed.Value()))});
            return m_svgs.Back().Document.Get();
        }

        /// Whether a file is there (an icon a type may or may not have).
        [[nodiscard]] bool Exists(StringView path) { return m_fs.Exists(path); }

        // === Reload ===
        /// Sweeps the files for changes since the last sweep. Returns true when any changed; the
        /// images and SVGs loaded from them are then set aside, LastChanged names them, and
        /// whatever tracks the changes runs again.
        bool Poll()
        {
            foundation::vfs::IChangeSource* source = m_fs.ChangeSource();
            if (source == nullptr)
                return false;
            Array<String> changed(m_allocator);
            if (!source->Poll(changed) || changed.IsEmpty())
                return false;
            for (const String& path : changed)
                Forget(path.AsView());
            m_lastChanged = Move(changed);
            m_changed.Notify();
            return true;
        }
        /// Records a read of the files for the GUI's reactive evaluation in progress: a change
        /// found by Poll runs it again (a sheet is reapplied, a page's markup is loaded again).
        void TrackChanges() const { m_changed.Track(); }
        /// The files the last change touched, relative to the root.
        [[nodiscard]] Span<const String> LastChanged() const noexcept { return Span<const String>(m_lastChanged.Data(), m_lastChanged.Size()); }

    private:
        struct ImageEntry
        {
            String Path;
            UniquePtr<image::OwnedImageData> Pixels;
        };
        struct SvgEntry
        {
            String Path;
            UniquePtr<svg::SVGDocument> Document;
        };

        bool ReadBytes(StringView path, Array<byte>& out)
        {
            UniquePtr<IStream> stream = m_fs.Open(path, FileMode::Read);
            if (!stream)
                return false;
            const i64 length = stream->Size();
            if (length < 0)
                return false;
            out.Resize(static_cast<usize>(length));
            const u64 read = stream->Read(out.Data(), static_cast<u64>(length));
            out.Resize(static_cast<usize>(read));
            return true;
        }
        // Sets aside what was loaded from `path`: it stays alive for whatever still draws it, and
        // is loaded afresh when next asked for.
        void Forget(StringView path)
        {
            for (usize i = m_images.Size(); i-- > 0;)
                if (m_images[i].Path.AsView() == path)
                {
                    m_setAside.PushBack(SetAside{Move(m_images[i].Pixels), {}});
                    m_images.RemoveAt(i);
                }
            for (usize i = m_svgs.Size(); i-- > 0;)
                if (m_svgs[i].Path.AsView() == path)
                {
                    m_setAside.PushBack(SetAside{{}, Move(m_svgs[i].Document)});
                    m_svgs.RemoveAt(i);
                }
        }
        struct SetAside
        {
            UniquePtr<image::OwnedImageData> Pixels;
            UniquePtr<svg::SVGDocument> Document;
        };

        IAllocator& m_allocator;
        foundation::vfs::NativeFileSystem m_fs;
        Array<ImageEntry> m_images;
        Array<SvgEntry> m_svgs;
        Array<SetAside> m_setAside; // replaced while the GUI may still borrow them
        Array<String> m_lastChanged;
        mutable gui::Observable m_changed;
    };
}
