// Draconic::EditorTexture - the `draconic.editor.texture` module.
//
// TextureEditorPage (editor-pages-gap.md, bespoke pass #1): the import-settings surface over
// TextureAsset. A CPU preview of the source image on the left (decoded through the same
// stb path the cook rides, so what you see is what gets cooked) and the GPU-texture intent
// on the right - color space, shape, sampler filters/wraps, mipmaps, anisotropy - plus the
// Sedulous presets (UI / Sprite / 3D / skybox). The authored TextureAsset IS the cook input,
// so Save writes the object back and requests a re-cook; every live proxy bound to the cooked
// product then hot-swaps (the same reload path material/mesh edits ride).
//
// Edits are BLOB-SNAPSHOT commands (the whole serialized TextureAsset before/after - it is
// tiny), giving exact undo without per-field command code and letting a preset (which moves
// many fields at once) collapse to one undo entry.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.texture;

import draconic.foundation;
import draconic.content;
import draconic.image;
import draconic.image.io;
import draconic.texture;
import draconic.texture.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace image = draconic::image;
    namespace texture = draconic::texture;

    // Import-settings + preview page for a TextureAsset.
    class TextureEditorPage final : public app::UIEditorPage
    {
    public:
        TextureEditorPage(EditorContext& context, draconic::content::Instance& instance);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] Status Save() override;

    private:
        // Decode the source image (external file, or the embedded "pixels" stream) into an
        // RGBA8 CPU buffer kept alive for the ImageView. Leaves m_preview null on failure.
        void LoadPreview(draconic::content::Instance& instance);

        // Build the property rows (color space / shape / sampler / mips / anisotropy + presets).
        void BuildGrid();

        // Refresh the source-facts label from the decoded preview + the asset's color space.
        void RefreshInfo();

        // === undo: whole-asset blob snapshots ===

        // Run one edit as an undoable command: snapshot -> mutate -> snapshot -> push.
        void ApplyEdit(StringView mergeKey, Function<void(texture::TextureAsset&)> mutate);
        [[nodiscard]] Array<byte> Snapshot() const;
        void ApplyBlob(const Array<byte>& blob);

        // Add an editor row plus a refresher that re-pulls its value (unless the user is
        // mid-edit) - so undo/redo and presets reflow every row without rebuilding the grid.
        void AddEditor(ui::toolkit::PropertyEditor* editor, Function<void()> refresher);

        // Whole-asset snapshot command: before/after blobs + a merge key (consecutive edits
        // of the same field collapse; the first command keeps the original `before`).
        class EditTextureCommand final : public IEditorCommand
        {
        public:
            EditTextureCommand(TextureEditorPage& page, StringView mergeKey, Array<byte> before,
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
            [[nodiscard]] StringView TypeId() const override { return u8"edit_texture"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditTextureCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            TextureEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        EditorContext* m_context = nullptr;
        String m_title;
        RefPtr<texture::TextureAsset> m_asset;
        UniquePtr<image::OwnedImageData> m_preview; // kept alive for the ImageView (borrowed ptr)
        image::PixelFormat m_sourceFormat = image::PixelFormat::RGBA8; // pre-preview source format

        RefPtr<ui::View> m_content;
        RefPtr<ui::ImageView> m_image;
        RefPtr<ui::Label> m_info;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        Array<Function<void()>> m_refreshers;
    };

    class TextureEditorPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;
    };

    inline void RegisterTextureEditor(EditorContext& context)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<TextureEditorPageFactory>(), DefaultAllocator()));
    }
}
