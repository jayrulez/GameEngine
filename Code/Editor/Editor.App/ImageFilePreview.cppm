// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :image_file_preview partition
//
// The import window's preview of a picture file (Specs/import-window.md W2): the image pages'
// ImagePreview over a bar of its own (fit, 1:1, the zoom, the channels, the pixel under the
// pointer), with the facts the file's header gives: its size, its channels and depth as stored,
// and whether its alpha is used. It decodes the file as a task on the editor's pool, saying it
// is reading meanwhile. A domain whose importer makes an asset from a picture registers it for that
// asset type (RegisterImageFilePreview).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:image_file_preview;

import foundation.core;
import foundation.image;
import foundation.gui;
import editor.core;
import pipeline.importer;
import :image_inspect_view;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace image = foundation::image;

    /// The picture's view: its bar (Fit, 1:1, the zoom, the channel, the pixel under the pointer)
    /// and the stage it shows in, with the ids an ImagePreview binds; for a preview that shows a
    /// picture (a picture file's, a model's texture).
    [[nodiscard]] RefPtr<gui::Node> BuildImageView(IAllocator& allocator);

    class ImageFilePreview final : public editor::IImportPreview
    {
    public:
        /// Shows that it is reading `path`, and starts the read on `tasks`; the picture and its
        /// facts show when it comes back, or why it did not decode.
        ImageFilePreview(StringView path, TaskPool& tasks, IAllocator& allocator);
        ImageFilePreview(const ImageFilePreview&) = delete;
        ImageFilePreview& operator=(const ImageFilePreview&) = delete;
        ~ImageFilePreview() override;

        [[nodiscard]] gui::Node& View() override { return *m_root; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }

        // === For tests ===
        [[nodiscard]] ImagePreview& Picture() noexcept { return *m_picture; }

    private:
        // What the worker reads, for the main thread to show.
        struct Decoded : RefCounted
        {
            image::Image pixels; // RGBA8, shown as is
            Array<String> facts;
            bool ok = false;
        };
        void Show(Decoded& decoded);

        RefPtr<gui::Node> m_root;
        UniquePtr<ImagePreview> m_picture;
        Array<String> m_facts;
        RefPtr<TaskHandle> m_read;
    };

    /// Previews the picture files an importer lists by one asset type.
    class ImageFilePreviewFactory final : public editor::IImportPreviewFactory
    {
    public:
        explicit ImageFilePreviewFactory(const TypeInfo& type) : m_type(&type) {}
        [[nodiscard]] const TypeInfo* AssetType() const override { return m_type; }
        [[nodiscard]] UniquePtr<editor::IImportPreview> Create(const editor::ImportPreviewSource& file, TaskPool& tasks, IAllocator& allocator) override;

    private:
        const TypeInfo* m_type;
    };

    /// Registers the picture preview for the files listed by `type`.
    void RegisterImageFilePreview(editor::EditorContext& context, const TypeInfo& type);
}
