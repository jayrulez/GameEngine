// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :image_file_preview partition
//
// The import window's preview of a picture file (Specs/import-window.md W2): the image pages'
// ImagePreview over a bar of its own (fit, 1:1, the zoom, the channels, the pixel under the
// pointer), with the facts the file's header gives: its size, its channels and depth as stored,
// and whether its alpha is used. A domain whose importer makes an asset from a picture registers
// it for that asset type (RegisterImageFilePreview).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:image_file_preview;

import foundation.core;
import foundation.gui;
import editor.core;
import :image_inspect_view;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class ImageFilePreview final : public editor::IImportPreview
    {
    public:
        /// Reads and shows the picture at `path`; a file that does not decode shows why, and no facts.
        ImageFilePreview(StringView path, IAllocator& allocator);
        ImageFilePreview(const ImageFilePreview&) = delete;
        ImageFilePreview& operator=(const ImageFilePreview&) = delete;

        [[nodiscard]] gui::Node& View() override { return *m_root; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }

        // === For tests ===
        [[nodiscard]] ImagePreview& Picture() noexcept { return *m_picture; }

    private:
        RefPtr<gui::Node> m_root;
        UniquePtr<ImagePreview> m_picture;
        Array<String> m_facts;
    };

    /// Previews the picture files an importer lists by one asset type.
    class ImageFilePreviewFactory final : public editor::IImportPreviewFactory
    {
    public:
        explicit ImageFilePreviewFactory(const TypeInfo& type) : m_type(&type) {}
        [[nodiscard]] const TypeInfo* AssetType() const override { return m_type; }
        [[nodiscard]] UniquePtr<editor::IImportPreview> Create(StringView path, IAllocator& allocator) override;

    private:
        const TypeInfo* m_type;
    };

    /// Registers the picture preview for the files listed by `type`.
    void RegisterImageFilePreview(editor::EditorContext& context, const TypeInfo& type);
}
