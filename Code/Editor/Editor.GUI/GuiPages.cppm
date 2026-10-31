// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::GUI - the `editor.gui` module: the editor's pages for the engine GUI's assets.
//
// GuiAssetEditorPage edits a `.xml` document or a `.css` sheet, built from its markup
// (Data/Editor/GuiPage.xml) on the code pages' base (the source file, the shared XML or CSS
// colours, one history, the problems list). Its check reports what the cook would report (the
// loader's and the parser's diagnostics). Beside the code, the runtime context's GuiSubsystem
// draws a live preview into a viewport on the page (GuiSubsystem::CreatePreview and
// RenderPreview) with the game's fonts, theme and sheets, so the preview matches what the game
// shows. A sheet is previewed over markup that belongs to the page: it is edited above the
// preview, can start as a copy of a document's markup ("Preview Over a Document"), and is kept
// for the sheet in the project's editor settings. It is never written into the asset, and its
// problems are listed under their own name. Markup that does not build keeps the last good
// preview and says so.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.gui;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.graphics;
import foundation.rhi;
import foundation.gui;
import foundation.gui.code;
import foundation.gui.viewport;
import gui.pipeline;
import engine.gui;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace gui_pages
    {
        /// What a sheet is previewed over with nothing chosen yet: a little of everything a sheet
        /// styles.
        inline constexpr StringView kStockPreviewMarkup = u8"<Panel class=\"page\" flex-direction=\"column\" gap=\"8\" padding=\"16\">\n"
                                                          u8"    <Label class=\"title\" text=\"Title\"/>\n"
                                                          u8"    <Label text=\"Body text.\"/>\n"
                                                          u8"    <Button text=\"Button\"/>\n"
                                                          u8"    <CheckBox text=\"Check box\"/>\n"
                                                          u8"    <Slider/>\n"
                                                          u8"</Panel>\n";
        /// How the problems list names the preview markup's problems (a sheet's page).
        inline constexpr StringView kPreviewMarkupName = u8"preview markup";

        /// Adds the cook's diagnostics to the page's problems; `file` names the code they are in
        /// (empty for the page's own code).
        inline void AddProblems(Span<const foundation::gui::GuiDiagnostic> found, StringView file, Array<app::CodeProblem>& out)
        {
            for (const foundation::gui::GuiDiagnostic& d : found)
            {
                app::CodeProblem p;
                p.severity = d.Level == foundation::gui::GuiDiagnostic::Severity::Error ? foundation::gui::CodeSeverity::Error : foundation::gui::CodeSeverity::Warning;
                p.message = d.Message;
                p.file = String(file);
                p.line = d.Line > 0 ? d.Line - 1 : 0;
                p.atColumn = false; // the cook names lines
                out.PushBack(Move(p));
            }
        }
    }

    /// A sheet's preview markup, kept per sheet in the project's editor settings (never cooked).
    struct GuiSheetPreview
    {
        Guid asset;
        String markup;

        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            foundation::core::Serialize(ar, "markup", markup);
        }
    };
    inline void Serialize(ISerializer& ar, GuiSheetPreview& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }
    class GuiSheetPreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(GuiSheetPreviewSettings, ISerializable)
    public:
        Array<GuiSheetPreview> sheets;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "sheets", sheets); }
    };

    class GuiAssetEditorPage final : public app::CodeAssetPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/GuiPage.xml";

        /// What the page edits: a document (its markup, previewed as it is) or a sheet (its CSS,
        /// previewed over markup the page keeps).
        enum class Kind : u8
        {
            Document,
            StyleSheet,
        };

        GuiAssetEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance, Kind kind);
        ~GuiAssetEditorPage() override;

        void OnClose() override;

        /// Previews a sheet over a copy of document `id`'s markup, which the page then keeps and
        /// edits as its own.
        void PreviewOverDocument(const Guid& id);

        // === For tests ===
        [[nodiscard]] Kind GetKind() const noexcept { return m_kind; }
        /// What the preview shows now (null before one built, or without the runtime's GUI).
        [[nodiscard]] foundation::gui::SceneRoot* PreviewRoot() const noexcept { return m_previewRoot.Get(); }
        /// Whether the preview shows an older version than the code (the code does not build).
        [[nodiscard]] bool PreviewStale() const noexcept { return m_stale; }
        /// A sheet's preview markup (empty on a document's page).
        [[nodiscard]] String PreviewMarkup() const;
        [[nodiscard]] foundation::gui::CodeEditor* PreviewMarkupEditor() const noexcept { return m_markup; }

    protected:
        [[nodiscard]] Array<app::CodeProblem> CheckCode(StringView text) override;

    private:
        void RebuildPreview(StringView text, bool builds);
        void ShowPreviewState();
        void KeepPreviewMarkup();

        Kind m_kind;
        engine::gui::GuiSubsystem* m_gui = nullptr; // the runtime context's GUI
        RefPtr<foundation::gui::Viewport> m_viewport;
        RefPtr<foundation::gui::SceneRoot> m_previewRoot; // in the runtime context's GUI
        foundation::gui::CodeEditor* m_markup = nullptr;  // a sheet's preview markup (owned by the page markup)
        u32 m_markupListener = 0;
        bool m_stale = false;
    };

    /// The page for one of the GUI's asset types.
    class GuiAssetPageFactory final : public IEditorPageFactory
    {
    public:
        GuiAssetPageFactory(const app::AssetPageServicesSource& services, GuiAssetEditorPage::Kind kind) : m_services(&services), m_kind(kind) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
        GuiAssetEditorPage::Kind m_kind;
    };

    /// Registers the GUI's pages (called from the editor's assembly) and the settings section that
    /// keeps a sheet's preview markup (registered before the app loads the per-project store).
    inline void RegisterGuiEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        GlobalTypeRegistry().Register(GuiSheetPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<GuiSheetPreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<GuiAssetPageFactory>(services, GuiAssetEditorPage::Kind::Document), editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<GuiAssetPageFactory>(services, GuiAssetEditorPage::Kind::StyleSheet), editor::EditorRootAllocator()));
    }
}
