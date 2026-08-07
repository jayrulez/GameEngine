// Draconic::EditorGameUI - the `draconic.editor.gameui` module.
//
// UIDocumentPage (game-ui.md P2): text editing + LIVE PREVIEW for UIDocumentAssets.
// The preview renders through the RUNTIME CONTEXT's UISubsystem - the GAME's context,
// fonts, GameTheme, and VG path - into this page's offscreen target (a dedicated
// preview RootView; it can never leak into game targets). What you see IS the game's
// renderer looking at your document; drift is impossible by construction. The text
// pane is the existing multi-line EditText (honest v1: no code-editor control yet);
// edits rebuild the preview after a short debounce, parse failures keep the last good
// preview with inline status, Save writes the asset + nudges the validating recook.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.gameui;

import draconic.foundation;
import draconic.content;
import draconic.runtime.client;
import draconic.graphics;
import draconic.rhi;
import draconic.vg.renderer;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.xml;
import draconic.ui.resource;
import draconic.ui.editor;
import draconic.ui.runtime;
import draconic.engine.ui;
import draconic.ui.viewport;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    Status UIDocumentEditorPage::Save()
    {
        draconic::content::Instance* instance =
            (m_context->Project() != nullptr)
                ? m_context->Project()->SourceDb().GetInstance(InstanceId())
                : nullptr;
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        ui::UIDocumentAsset asset;
        asset.markup = String(m_markup.AsView());
        const Status written = instance->WriteObject(asset);
        if (written.IsOk())
        {
            ClearDirty();
            // The VALIDATING cook reports errors/warnings; hot reload updates canvases.
            if (m_context->OnCookRequested)
            {
                m_context->OnCookRequested(false);
            }
        }
        return written;
    }

    void UIDocumentEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        EnsureViewportBound();
        if (m_previewDelay > 0.0f)
        {
            m_previewDelay -= dt;
            if (m_previewDelay <= 0.0f)
            {
                RebuildPreview();
            }
        }
    }

    void UIDocumentEditorPage::OnAfterSceneRender(runtime::IApplicationHost&,
                                                  draconic::graphics::FrameContext& frame)
    {
        if (!m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        // Always define the target's layout (the editor UI samples it every frame).
        m_viewport->ClearContent(*frame.encoder);
        if (m_ui == nullptr || m_previewRoot.Get() == nullptr)
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0)
        {
            return;
        }
        frame.encoder->TransitionTexture(m_viewport->ColorTexture(), m_viewport->ColorState(),
                                         rhi::ResourceState::RenderTarget);
        m_ui->RenderPreview(*m_previewRoot, *frame.encoder, m_viewport->ColorTargetView(),
                            m_viewport->ColorFormat(), w, h, frame.frameIndex);
        frame.encoder->TransitionTexture(m_viewport->ColorTexture(),
                                         rhi::ResourceState::RenderTarget,
                                         rhi::ResourceState::ShaderRead);
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
    }

    void UIDocumentEditorPage::OnClose()
    {
        if (m_ui != nullptr && m_previewRoot.Get() != nullptr)
        {
            m_ui->DestroyPreview(m_previewRoot.Get());
        }
        m_previewRoot = nullptr;
        m_viewport->Shutdown();
    }

    void UIDocumentEditorPage::EnsureViewportBound()
    {
        ui::RootView* root = m_viewport->Root();
        if (root == nullptr)
        {
            return;
        }
        draconic::graphics::RenderWindow* window = m_uiHost->WindowForRoot(root);
        if (window == nullptr || window == m_hostWindow)
        {
            return;
        }
        vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(window);
        if (renderer == nullptr)
        {
            return;
        }
        if (m_hostWindow == nullptr)
        {
            m_viewport->Initialize(m_host->Graphics()->Raw(), renderer, m_host->Shell()->Input(),
                                   window->Window().Id());
        }
        else
        {
            m_viewport->AttachToWindow(renderer, window->Window().Id());
        }
        m_hostWindow = window;
    }

    void UIDocumentEditorPage::RebuildPreview()
    {
        // Validation pass for the inline status (warnings + parse result). A direct
        // XmlDocument parse first: MarkupLoader swallows the error position, and the code
        // editor wants the failing LINE as an Error marker (P3 diagnostics).
        ui::MarkupLoader::Initialize();
        {
            draconic::xml::XmlDocument probe;
            const draconic::xml::XmlResult result = probe.Parse(m_markup.AsView());
            Array<ui::toolkit::CodeDiagnostic> diagnostics;
            if (draconic::xml::IsError(result))
            {
                ui::toolkit::CodeDiagnostic diagnostic;
                diagnostic.isError = true;
                diagnostic.line = probe.ErrorLine() - 1; // 1-based -> buffer lines
                diagnostic.message = String(draconic::xml::Describe(result));
                diagnostics.PushBack(Move(diagnostic));
            }
            m_editor->Document().SetDiagnostics(Move(diagnostics));
            m_editor->Invalidate();
        }
        Array<String> warnings;
        RefPtr<ui::View> parsed =
            ui::MarkupLoader::LoadFromString(m_markup.AsView(), nullptr, &warnings);
        if (parsed.Get() == nullptr)
        {
            m_status->SetText(u8"Parse FAILED - showing the last good preview.");
            return;
        }
        // ...then the REAL preview instantiates in the runtime context (game fonts,
        // GameTheme, style resolution - the subsystem's CreatePreview).
        if (m_ui == nullptr)
        {
            m_status->SetText(u8"No runtime UI subsystem - preview unavailable.");
            return;
        }
        ui::UIDocument document;
        document.markup = String(m_markup.AsView());
        RefPtr<ui::RootView> fresh = m_ui->CreatePreview(document);
        if (fresh.Get() == nullptr)
        {
            m_status->SetText(u8"Parse FAILED - showing the last good preview.");
            return;
        }
        if (m_previewRoot.Get() != nullptr)
        {
            m_ui->DestroyPreview(m_previewRoot.Get());
        }
        m_previewRoot = fresh;
        if (warnings.IsEmpty())
        {
            m_status->SetText(u8"OK");
        }
        else
        {
            String text(u8"Warnings: ");
            text.Append(warnings[0].AsView());
            if (warnings.Size() > 1)
            {
                text.Append(u8" (+more)");
            }
            m_status->SetText(text.AsView());
        }
    }
    const TypeInfo* UIDocumentPageFactory::PrimaryType() const
    {
        return &ui::UIDocumentAsset::StaticType();
    }

    UniquePtr<EditorPage> UIDocumentPageFactory::CreatePage(EditorContext& context,
                                                            draconic::content::Instance& instance)
    {
        auto* page =
            DefaultAllocator().New<UIDocumentEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}
