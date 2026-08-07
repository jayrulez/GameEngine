// Draconic::EditorGameUI - the `draconic.editor.gameui` module.
//
// UIDocumentPage (game-ui.md P2): text editing + LIVE PREVIEW for UIDocumentAssets.
// The preview renders through the RUNTIME CONTEXT's UISubsystem - the GAME's context,
// fonts, GameTheme, and VG path - into this page's offscreen target (a dedicated
// preview RootView; it can never leak into game targets). What you see IS the game's
// renderer looking at your document; drift is impossible by construction. The text
// pane is ui::toolkit::CodeEditView (monospace, virtualized, document-word completion;
// the XML lexer + structured line diagnostics arrive with code-editor P2/P3); edits
// rebuild the preview after a short debounce, parse failures keep the last good
// preview with inline status, Save writes the asset + nudges the validating recook.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.gameui;

import draconic.foundation;
import draconic.content;
import draconic.runtime.client;
import draconic.graphics;
import draconic.rhi;
import draconic.vg.renderer;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.resource;
import draconic.ui.editor;
import draconic.ui.runtime;
import draconic.engine.ui;
import draconic.ui.viewport;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace vg = draconic::vg;
    namespace rhi = draconic::rhi;

    class UIDocumentEditorPage final : public app::UIEditorPage
    {
    public:
        UIDocumentEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                             ui::runtime::UIHost& uiHost, draconic::content::Instance& instance)
            : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
        {
            m_ui = host.Ctx().GetSubsystem<ui::UISubsystem>();
            SetInstanceId(instance.Id());
            RefPtr<ISerializable> object = instance.ReadObject();
            if (auto* asset = Cast<ui::UIDocumentAsset>(object.Get()))
            {
                m_markup = String(asset->markup.AsView());
            }

            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8.0f;

            // Left: the text pane (CodeEditView - gutter, monospace, native undo).
            m_editor = MakeRef<ui::toolkit::CodeEditView>(DefaultAllocator());
            m_editor->AllowBreakpoints = false; // markup has no debugger; keep the margin quiet
            // XML is a generic format the toolkit lexes natively - constructed directly, no
            // registry indirection needed (that seam is for dynamic language ids).
            m_editor->SetLexer(UniquePtr<ui::toolkit::ICodeLexer>(
                DefaultAllocator().New<ui::toolkit::XmlLexer>(), DefaultAllocator()));
            m_editor->CompletionTriggerCharacters = String(u8"<"); // tags open the popup
            m_editor->AddCompletionProvider(&m_markupProvider);
            m_editor->SetText(m_markup.AsView());
            UIDocumentEditorPage* self = this;
            m_editor->OnTextChanged.Add(
                [self]()
                {
                    self->m_markup = self->m_editor->Text();
                    self->MarkDirty();
                    self->m_previewDelay = 0.35f; // debounce: rebuild shortly after typing stops
                });
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Height = ui::SizeSpec::Match();
                row->AddView(m_editor.Get(), lp);
            }

            // Right: inline status over the live preview.
            auto right = MakeRef<ui::FlexLayout>(DefaultAllocator());
            right->Direction = ui::Orientation::Vertical;
            right->Spacing = 4.0f;
            m_status = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
            m_status->FontSize.SetValue(12.0f);
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                right->AddView(m_status.Get(), lp);
            }
            // The preview surface: an offscreen target the RUNTIME UI subsystem renders
            // into (the editor UI just displays the texture).
            m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
            m_viewport->ClearColor = rhi::ClearColor{0.08f, 0.09f, 0.11f, 1.0f};
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                right->AddView(m_viewport.Get(), lp);
            }
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Height = ui::SizeSpec::Match();
                row->AddView(right.Get(), lp);
            }
            m_content = row;
            RebuildPreview();
        }

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }

        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;

        void OnAfterSceneRender(runtime::IApplicationHost&,
                                draconic::graphics::FrameContext& frame) override;

        void OnClose() override;

    private:
        // Same lazy dance as ScenePage: the DISPLAY side of the offscreen target needs
        // the page's hosting window + its VG renderer.
        void EnsureViewportBound();

        void RebuildPreview();

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        ui::UISubsystem* m_ui = nullptr; // the RUNTIME context's subsystem
        draconic::graphics::RenderWindow* m_hostWindow = nullptr;
        String m_title;
        String m_markup;
        f32 m_previewDelay = 0.0f;
        ui::toolkit::MarkupCompletionProvider m_markupProvider; // borrowed by the editor
        RefPtr<ui::View> m_content;
        RefPtr<ui::toolkit::CodeEditView> m_editor;
        RefPtr<ui::Label> m_status;
        RefPtr<ui::viewport::ViewportView> m_viewport;
        RefPtr<ui::RootView> m_previewRoot; // lives in the RUNTIME context
    };

    class UIDocumentPageFactory final : public IEditorPageFactory
    {
    public:
        UIDocumentPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    /// The editor executable's entry point for the game-UI plugin.
    inline void RegisterGameUIEditor(EditorContext& context, runtime::IApplicationHost& host,
                                     ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<UIDocumentPageFactory>(host, uiHost), DefaultAllocator()));
    }
}
