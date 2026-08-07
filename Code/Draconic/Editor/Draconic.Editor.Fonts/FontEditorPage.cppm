// Draconic::EditorFonts - the `draconic.editor.fonts` module.
//
// FontEditorPage: the bespoke authoring page for FontAsset (the fonts-triad source asset).
// Left: a live CPU bake preview - the atlas the cook would produce at the preview size
// (coverage expands to RGBA8; MSDF shows the raw field channels), rebaked on every option
// commit - plus source facts (file, family, glyph count, atlas occupancy). Right: the
// authored surface in a PropertyGrid - family, bake mode, the size ramp (comma-separated),
// MSDF size, codepoint range, atlas dimensions. Edits are whole-asset blob-snapshot undo
// commands; Save writes the object back and requests a re-cook so bound products hot-swap.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.fonts;

import draconic.foundation;
import draconic.content;
import draconic.image;
import draconic.fonts;
import draconic.fonts.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace image = draconic::image;
    namespace fonts = draconic::fonts;

    // Authoring page for a FontAsset (bake intent + live atlas preview).
    class FontEditorPage final : public app::UIEditorPage
    {
    public:
        FontEditorPage(EditorContext& context, draconic::content::Instance& instance);

        ~FontEditorPage() override;

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] Status Save() override;

    private:
        // The preview bake runs OFF the UI thread (msdfgen over the full codepoint range
        // takes long enough to hitch a mode switch), on the EditorJobService LIGHT lane
        // (concurrent with the build lane; never trips the cook mutation lock). The work
        // closure writes a heap slot; the completion fires on the main thread and applies
        // it. LATEST-WINS: edits during a bake stash the newest request; a stale outcome
        // is discarded and the stash re-bakes. The old preview stays on screen until its
        // replacement is ready. No job service (tests) = synchronous fallback.
        struct BakeRequest
        {
            bool valid = false;
            String path;
            bool distanceField = false;
            f32 size = 0.0f;
            i32 firstCodepoint = 0;
            i32 lastCodepoint = 0;
            u32 atlasWidth = 0;
            u32 atlasHeight = 0;
            u64 generation = 0;
        };
        struct BakeOutcome
        {
            UniquePtr<image::OwnedImageData> image;
            usize glyphs = 0;
            f32 size = 0.0f;
            u64 generation = 0;
        };
        // Owned by the in-flight submission's closures; `pageAlive` is the page-lifetime
        // guard (page dtor clears it; both dtor and completion run on the main thread, so
        // this is ordering, not a race). The completion closure always deletes the slot.
        struct BakeSlot
        {
            BakeOutcome outcome;
            bool pageAlive = true;
        };

        void RebakePreview();
        void StartBake(BakeRequest request);
        void ApplyBakeOutcome(BakeOutcome outcome);
        [[nodiscard]] BakeRequest CaptureBakeRequest();
        static void RunBake(const BakeRequest& request, BakeOutcome& outcome);
        void BuildGrid();
        // Sections are mode-dependent (raster ramp vs MSDF size): a mode change rebuilds the
        // grid, DEFERRED through the UI mutation queue (the change fires from a grid editor -
        // rebuilding mid-dispatch would destroy the dispatching control).
        void QueueGridRebuild();
        void CommitEdit(StringView mergeKey);

        [[nodiscard]] Array<byte> Snapshot() const;
        void ApplyBlob(const Array<byte>& blob);
        void RefreshRows(); // re-pull row values after undo/redo (never rebuilds the grid)

        class EditFontCommand final : public IEditorCommand
        {
        public:
            EditFontCommand(FontEditorPage& page, StringView mergeKey, Array<byte> before,
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
            [[nodiscard]] StringView TypeId() const override { return u8"edit_font"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditFontCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            FontEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        EditorContext* m_context = nullptr;
        String m_title;
        RefPtr<fonts::FontAsset> m_asset;
        UniquePtr<image::OwnedImageData> m_preview; // kept alive for the ImageView (borrowed ptr)
        usize m_previewGlyphs = 0;
        f32 m_previewSize = 0.0f;

        // Async bake state (main-thread only; the worker touches only its slot).
        bool m_bakeBusy = false;
        u64 m_bakeGeneration = 0;
        BakeRequest m_pendingRequest; // newest request stashed while a bake is in flight
        bool m_pendingValid = false;
        BakeSlot* m_activeSlot = nullptr; // borrowed view of the in-flight slot

        RefPtr<ui::View> m_content;
        RefPtr<ui::ImageView> m_image;
        RefPtr<ui::Label> m_info;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        // Borrowed rows (grid owns) - re-pulled after undo/redo.
        ui::toolkit::StringEditor* m_familyRow = nullptr;
        ui::toolkit::EnumEditor* m_modeRow = nullptr;
        ui::toolkit::StringEditor* m_sizesRow = nullptr;
        ui::toolkit::FloatEditor* m_dfSizeRow = nullptr;
        ui::toolkit::IntEditor* m_firstRow = nullptr;
        ui::toolkit::IntEditor* m_lastRow = nullptr;
        ui::toolkit::IntEditor* m_atlasWidthRow = nullptr;
        ui::toolkit::IntEditor* m_atlasHeightRow = nullptr;
        ui::toolkit::StringEditor* m_fileRow = nullptr;
        fonts::FontBakeMode m_gridMode = fonts::FontBakeMode::RasterRamp; // mode the grid was built for
        Array<byte> m_undoBaseline;
    };

    class FontEditorPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;
    };

    inline void RegisterFontEditor(EditorContext& context)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<FontEditorPageFactory>(), DefaultAllocator()));
    }
}
