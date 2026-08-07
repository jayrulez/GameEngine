// Draconic::EditorImage - the `draconic.editor.image` module.
//
// ImageEditorPage (editor-pages-gap.md, the Texture page's source-side sibling): the inspect +
// intent surface over an ImageAsset - the raw source image that Texture assets (and future
// consumers) build on. A CPU preview of the decoded file on the left (the same decode the cook
// rides, converted to RGBA8 for display) and the small authored surface on the right: the
// COLOR-SPACE intent, plus read-only source facts (file, dimensions, pixel format, data size).
// Edits are whole-asset blob-snapshot undo commands (the asset is tiny); Save writes the object
// back and requests a re-cook so bound products hot-swap.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.image;

import draconic.foundation;
import draconic.content;
import draconic.image;
import draconic.image.io;
import draconic.image.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace image = draconic::image;

    // Inspect + color-space page for an ImageAsset.
    class ImageEditorPage final : public app::UIEditorPage
    {
    public:
        ImageEditorPage(EditorContext& context, draconic::content::Instance& instance);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] Status Save() override;

    private:
        // Decode the source file into an RGBA8 CPU buffer kept alive for the ImageView.
        void LoadPreview();
        void BuildGrid();
        void RefreshInfo();

        [[nodiscard]] Array<byte> Snapshot() const;
        void ApplyBlob(const Array<byte>& blob);

        class EditImageCommand final : public IEditorCommand
        {
        public:
            EditImageCommand(ImageEditorPage& page, StringView mergeKey, Array<byte> before,
                             Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_image"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditImageCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            ImageEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        EditorContext* m_context = nullptr;
        String m_title;
        RefPtr<image::ImageAsset> m_asset;
        UniquePtr<image::OwnedImageData> m_preview; // kept alive for the ImageView (borrowed ptr)
        image::PixelFormat m_sourceFormat = image::PixelFormat::RGBA8;
        usize m_sourceBytes = 0;

        RefPtr<ui::View> m_content;
        RefPtr<ui::ImageView> m_image;
        RefPtr<ui::Label> m_info;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        ui::toolkit::EnumEditor* m_colorSpaceRow = nullptr; // borrowed (grid owns)
        Array<byte> m_undoBaseline;
    };

    class ImageEditorPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;
    };

    inline void RegisterImageEditor(EditorContext& context)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<ImageEditorPageFactory>(), DefaultAllocator()));
    }
}
