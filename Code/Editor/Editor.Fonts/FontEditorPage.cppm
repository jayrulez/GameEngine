// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Fonts - the `editor.fonts` module.
//
// FontEditorPage: the bespoke authoring page for FontAsset (the fonts-triad source asset). On the
// GUI from the image pages' markup (Data/Editor/ImagePage.gml; Documentation/Specs/editor-gui.md,
// 5.4, layer 2): the atlas the cook would produce at the preview size, baked live (coverage as
// white over alpha; a distance field decoded at its edge, where its raw channels read as noise),
// rebaked on every edit and its undo, to zoom, pan and read in; beside the authored surface:
// family, bake mode, the distance field's size and range, the codepoint range, the atlas size, and
// the raster ramp's sizes, a row of the page's own ("12, 16, 24") as reflection does not show
// them. Its facts read only: the glyphs baked and the atlas. Each edit is one undo step;
// Save writes the asset back and asks for a cook so bound products hot-swap.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.fonts;

export import :thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.fonts;
import fonts.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class FontEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ImagePage.gml";

        FontEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~FontEditorPage() override;

        /// Sets the raster ramp's sizes from text ("10, 12.5 24"), one undo step; false, with the
        /// row put back and a notice saying why, when it is not a list of sizes from 1 to 512.
        bool SetSizes(StringView text);
        /// A font's source relinks to another font file of the project's (Browse on its row).
        [[nodiscard]] Span<const String> SourceExtensions() const override;

        // === For tests ===
        [[nodiscard]] app::ImagePreview* Preview() const noexcept { return m_preview.Get(); }
        [[nodiscard]] foundation::gui::PropertyBag& Ramp() noexcept { return *m_ramp; }
        [[nodiscard]] usize PreviewGlyphs() const noexcept { return m_previewGlyphs; }

    protected:
        void OnObjectChanged() override;

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
            f32 distanceFieldRange = 4.0f;
            i32 firstCodepoint = 0;
            i32 lastCodepoint = 0;
            u32 atlasWidth = 0;
            u32 atlasHeight = 0;
            u64 generation = 0;
        };
        struct BakeOutcome
        {
            foundation::image::Image image; // empty: nothing baked
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
        // The ramp's row as the asset has it, shown only for a raster ramp.
        void ShowRamp();

        UniquePtr<app::ImagePreview> m_preview;
        UniquePtr<foundation::gui::PropertyBag> m_ramp; // the page's own rows: the sizes
        usize m_previewGlyphs = 0;
        f32 m_previewSize = 0.0f;

        // Async bake state (main-thread only; the worker touches only its slot).
        bool m_bakeBusy = false;
        u64 m_bakeGeneration = 0;
        BakeRequest m_pendingRequest; // newest request stashed while a bake is in flight
        bool m_pendingValid = false;
        BakeSlot* m_activeSlot = nullptr; // borrowed view of the in-flight slot
    };

    class FontEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit FontEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The page (asking for its services when it is made) and the font thumbnails.
    inline void RegisterFontEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<FontEditorPageFactory>(services), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
        {
            RegisterFontThumbnailGenerator(*context.Thumbnails());
        }
    }
}
