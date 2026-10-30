// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::GUI - the GUI pages' implementation (see GuiPages.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.gui;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.settings;
import foundation.rhi;
import foundation.gui;
import foundation.gui.code;
import foundation.gui.viewport;
import gui.pipeline;
import engine.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace rhi = foundation::rhi;

namespace editor
{
    RTTI_DEFINE_OBJECT(GuiSheetPreviewSettings, "rtti::editor::gui")

    GuiAssetEditorPage::GuiAssetEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance, Kind kind)
        : app::CodeAssetPage(context, Move(services), instance,
                             kind == Kind::Document ? pipeline::GuiDocumentAsset::StaticType() : pipeline::GuiStyleSheetAsset::StaticType(), kPath),
          m_kind(kind)
    {
        m_gui = Services().host != nullptr ? Services().host->Subsystems().GetSubsystem<engine::gui::GuiSubsystem>() : nullptr;
        GuiAssetEditorPage* self = this;

        // A sheet's preview markup: the page's, kept for the sheet in the project's editor settings
        // (first from what an older page kept on the asset, else a little of everything).
        m_markup = Find<gui::CodeEditor>(u8"gui-preview-markup");
        auto* document = Find<gui::ButtonBase>(u8"gui-preview-document");
        if (m_kind == Kind::StyleSheet)
        {
            String markup;
            if (foundation::settings::Settings* store = Context().ProjectEditorSettings())
                if (const GuiSheetPreviewSettings* kept = store->Find<GuiSheetPreviewSettings>())
                    for (const GuiSheetPreview& sheet : kept->sheets)
                        if (sheet.asset == InstanceId())
                            markup = sheet.markup;
            if (markup.IsEmpty())
                if (const auto* sheet = ObjectAs<pipeline::GuiStyleSheetAsset>())
                    markup = sheet->previewMarkup;
            if (markup.IsEmpty())
                markup = String(gui_pages::kStockPreviewMarkup);
            if (m_markup != nullptr)
            {
                m_markup->SetText(markup.AsView());
                if (Services().languages != nullptr)
                    m_markup->SetLanguage(Services().languages->Find(u8"xml"));
                // Its edits are the page's preview, not the asset: checked again, never dirtying it.
                m_markupListener = m_markup->GetDocument().AddChangeListener([self](const gui::CodeChange&) { self->CheckSoon(); });
            }
            if (document != nullptr)
                document->SetOnClick([self]()
                                     {
                                         const app::ResourceRefServices* refs = self->Services().refs;
                                         if (refs == nullptr || !refs->Pick)
                                             return;
                                         const String types[] = {String(u8"GuiDocumentAsset")};
                                         refs->Pick(Span<const String>(types, 1), Guid{}, [self](const Guid& id) { self->PreviewOverDocument(id); });
                                     });
        }
        else
        {
            // A document is previewed as it is: no markup of the page's, no document to choose.
            if (auto* side = Find<gui::Node>(u8"gui-preview-side"))
                if (auto* pane = Find<gui::Node>(u8"gui-markup-pane"))
                    side->RemoveChild(pane);
            m_markup = nullptr;
            if (document != nullptr)
                document->SetDisplayed(false);
        }

        // The preview: a viewport of the page's, the runtime's GUI drawing into it each frame.
        m_viewport = MakeRef<gui::Viewport>(Allocator());
        m_viewport->AddClass(u8"preview-view"); // fills its stage, as the preview pages' viewports do
        m_viewport->ClearColor = Color{0.313f, 0.332f, 0.366f, 1.0f};
        m_viewport->OnRender = [self](gui::Viewport& view, rhi::CommandEncoder& encoder, i32 frameIndex)
        {
            // The target defined every frame (the editor samples it), then the preview over it.
            rhi::RenderPassDesc pass;
            rhi::ColorAttachment color;
            color.view = view.ColorView();
            color.loadOp = rhi::LoadOp::Clear;
            color.storeOp = rhi::StoreOp::Store;
            color.clearValue = view.LinearClearColor();
            pass.colorAttachments.Add(color);
            if (rhi::RenderPassEncoder* clear = encoder.BeginRenderPass(pass))
                clear->End();
            if (self->m_gui == nullptr || !self->m_previewRoot || view.RenderWidth() == 0 || view.RenderHeight() == 0)
                return;
            self->m_gui->RenderPreview(*self->m_previewRoot, encoder, view.ColorView(), view.ColorFormat(), view.RenderWidth(), view.RenderHeight(),
                                       static_cast<u32>(frameIndex));
        };
        if (auto* stage = Find<gui::Node>(u8"gui-preview-stage"))
            stage->AddChild(m_viewport.Get());
        if (Services().gui != nullptr)
            m_viewport->HostIn(*Services().gui);

        OpenCode(m_kind == Kind::Document ? StringView(u8"xml") : StringView(u8"css"));
    }

    GuiAssetEditorPage::~GuiAssetEditorPage()
    {
        if (m_markup != nullptr)
            m_markup->GetDocument().RemoveChangeListener(m_markupListener);
    }

    void GuiAssetEditorPage::OnClose()
    {
        m_previewRoot = nullptr; // before the runtime's GUI goes
        if (m_viewport)
            m_viewport->OnRender = nullptr;
    }

    String GuiAssetEditorPage::PreviewMarkup() const
    {
        return m_markup != nullptr ? m_markup->GetText() : String{};
    }

    Array<app::CodeProblem> GuiAssetEditorPage::CheckCode(StringView text)
    {
        // What the cook would report, on the lines it is about; a sheet's preview markup's under its name.
        Array<app::CodeProblem> problems;
        Array<gui::GuiDiagnostic> found;
        bool builds = true;
        if (m_kind == Kind::Document)
            builds = pipeline::ValidateGuiDocument(text, found, Allocator());
        else
            pipeline::ValidateGuiStyleSheet(text, found);
        gui_pages::AddProblems(Span<const gui::GuiDiagnostic>(found.Data(), found.Size()), StringView{}, problems);
        if (m_kind == Kind::StyleSheet)
        {
            const String markup = PreviewMarkup();
            Array<gui::GuiDiagnostic> markupFound;
            builds = pipeline::ValidateGuiDocument(markup.AsView(), markupFound, Allocator());
            gui_pages::AddProblems(Span<const gui::GuiDiagnostic>(markupFound.Data(), markupFound.Size()), gui_pages::kPreviewMarkupName, problems);
            // Its own lines marked in its own editor.
            if (m_markup != nullptr)
            {
                Array<gui::CodeDiagnostic> marks;
                for (const gui::GuiDiagnostic& d : markupFound)
                {
                    const u32 line = d.Line > 0 ? d.Line - 1 : 0;
                    const u32 end = line < m_markup->GetDocument().LineCount() ? static_cast<u32>(m_markup->GetDocument().LineText(line).Size()) : 0;
                    marks.PushBack(gui::CodeDiagnostic{gui::CodeRange{gui::CodePosition{line, 0}, gui::CodePosition{line, end}},
                                                       d.Level == gui::GuiDiagnostic::Severity::Error ? gui::CodeSeverity::Error : gui::CodeSeverity::Warning, d.Message});
                }
                m_markup->SetDiagnostics(Span<const gui::CodeDiagnostic>(marks.Data(), marks.Size()));
            }
            KeepPreviewMarkup();
        }
        RebuildPreview(text, builds);
        return problems;
    }

    void GuiAssetEditorPage::RebuildPreview(StringView text, bool builds)
    {
        if (!builds)
        {
            m_stale = m_previewRoot.Get() != nullptr; // the last good one stays
            ShowPreviewState();
            return;
        }
        if (m_gui != nullptr)
        {
            const String markup = m_kind == Kind::Document ? String(text) : PreviewMarkup();
            RefPtr<gui::SceneRoot> root = m_gui->CreatePreview(markup.AsView(), m_kind == Kind::StyleSheet ? text : StringView{});
            if (root)
                m_previewRoot = Move(root);
        }
        m_stale = false;
        ShowPreviewState();
    }

    void GuiAssetEditorPage::ShowPreviewState()
    {
        if (auto* stale = Find<gui::Node>(u8"gui-preview-stale"))
            stale->SetDisplayed(m_stale);
        const bool showing = m_previewRoot.Get() != nullptr;
        if (auto* stage = Find<gui::Node>(u8"gui-preview-stage"))
            stage->SetDisplayed(showing);
        if (auto* empty = Find<gui::Node>(u8"gui-preview-empty"))
            empty->SetDisplayed(!showing);
        if (!showing)
            if (auto* why = Find<gui::Label>(u8"gui-preview-empty-text"))
                why->SetText(m_gui == nullptr ? StringView(u8"The runtime has no GUI: nothing draws a preview.") : StringView(u8"It shows once the markup builds."));
    }

    void GuiAssetEditorPage::KeepPreviewMarkup()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr || m_markup == nullptr)
            return;
        GuiSheetPreviewSettings& kept = store->Section<GuiSheetPreviewSettings>();
        const String markup = PreviewMarkup();
        GuiSheetPreview* mine = nullptr;
        for (GuiSheetPreview& sheet : kept.sheets)
            if (sheet.asset == InstanceId())
                mine = &sheet;
        if (mine != nullptr && mine->markup.AsView() == markup.AsView())
            return; // unchanged: nothing to write
        if (mine != nullptr)
            mine->markup = markup;
        else
            kept.sheets.PushBack(GuiSheetPreview{InstanceId(), markup});
        store->MarkChanged<GuiSheetPreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    void GuiAssetEditorPage::PreviewOverDocument(const Guid& id)
    {
        if (m_markup == nullptr || id.IsNil() || Context().Project() == nullptr)
            return;
        foundation::content::Instance* instance = Context().Project()->SourceDb().GetInstance(id);
        RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        const auto* document = Cast<pipeline::GuiDocumentAsset>(object.Get());
        if (document == nullptr || document->fileName.IsEmpty())
            return; // an unlinked document has no markup to preview over
        Result<Array<byte>> bytes = ReadFile(PathJoin(Context().Project()->SourcesRoot().AsView(), document->fileName.View()).AsView());
        if (!bytes.HasValue())
            return;
        // Copied once: the page's to change from now on (the document is not touched).
        m_markup->SetText(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        CheckNow();
    }

    const TypeInfo* GuiAssetPageFactory::PrimaryType() const
    {
        return m_kind == GuiAssetEditorPage::Kind::Document ? &pipeline::GuiDocumentAsset::StaticType() : &pipeline::GuiStyleSheetAsset::StaticType();
    }

    UniquePtr<EditorPage> GuiAssetPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<GuiAssetEditorPage>(context, m_services->Make(), instance, m_kind);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
