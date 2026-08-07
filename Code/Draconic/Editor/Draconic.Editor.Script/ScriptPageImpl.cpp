// Draconic::EditorScript - the `draconic.editor.script` module.
//
// ScriptEditorPage implementation: Save (write source + recook + notify), the debounced
// compile-check, and the error surface - status line, error list, and CodeEditView Error
// markers on the offending lines.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.script;

import draconic.foundation;
import draconic.content;
import draconic.runtime.client;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.script;
import draconic.script.editor;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    const TypeInfo* ScriptClassPageFactory::PrimaryType() const
    {
        return &draconic::script::ScriptClassAsset::StaticType();
    }

    UniquePtr<EditorPage> ScriptClassPageFactory::CreatePage(EditorContext& context,
                                                             content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<ScriptEditorPage>(context, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }

    Status ScriptEditorPage::Save()
    {
        const Status written = m_doc.Save();
        if (written.IsOk())
        {
            ClearDirty();
            // Reuse the external-edit path: the incremental cook rebuilds this asset's
            // product and the app hot-reloads it (ScriptSceneSystem re-instantiates live
            // behaviors + re-applies overrides). A failing cook keeps the last-good product.
            m_context->RequestCook(false);
            // Compile-check now for the inline error surface (the async cook only logs).
            RefreshCompileStatus();
            if (m_doc.LastCompileOk())
            {
                m_context->Notify(NoticeKind::Success, u8"Script saved - recooking");
            }
            else
            {
                m_context->Notify(NoticeKind::Warning,
                                  u8"Script saved with compile errors - last good kept");
            }
        }
        return written;
    }

    void ScriptEditorPage::OnUpdate(draconic::runtime::IApplicationHost&, f32 dt)
    {
        m_apiBrowser.Update(); // deferred tree rebuild (filter edits only mark dirty)

        if (m_validateDelay > 0.0f)
        {
            m_validateDelay -= dt;
            if (m_validateDelay <= 0.0f)
            {
                RefreshCompileStatus();
            }
        }

        // Paused-debugger location -> the editor's ExecutionLine marker (version-polled;
        // a Game run's debugger listener writes the shared point on break/step/resume).
        if (m_executionVersionSeen != m_context->ScriptExecutionVersion())
        {
            m_executionVersionSeen = m_context->ScriptExecutionVersion();
            const EditorContext::ScriptExecutionPoint& point = m_context->ScriptExecution();
            if (point.active && point.file.AsView() == m_doc.FileName() && point.line >= 1)
            {
                m_editor->Document().SetExecutionLine(point.line - 1);
                m_editor->ScrollToLine(point.line - 1);
            }
            else
            {
                m_editor->Document().SetExecutionLine(-1);
                m_editor->Invalidate();
            }
        }
    }

    void ScriptEditorPage::RefreshCompileStatus()
    {
        const bool ok = m_doc.Validate();
        Span<const draconic::script::ScriptSourceDocument::CompileError> errors = m_doc.Errors();

        // Project errors onto the buffer: one Error marker (with the message as the row's
        // diagnostic) per line. Wholesale replace per validation run.
        Array<ui::toolkit::CodeDiagnostic> diagnostics;
        for (const draconic::script::ScriptSourceDocument::CompileError& e : errors)
        {
            ui::toolkit::CodeDiagnostic diagnostic;
            diagnostic.isError = true; // the compile-check path only reports compile errors
            diagnostic.line = e.line > 0 ? e.line - 1 : 0;
            diagnostic.message = String(e.message.AsView());
            diagnostics.PushBack(Move(diagnostic));
        }
        m_editor->Document().SetDiagnostics(Move(diagnostics));
        m_editor->Invalidate();

        if (ok)
        {
            String line(u8"Compiled OK");
            if (!m_doc.ClassName().IsEmpty())
            {
                line.Append(u8" - class ");
                line.Append(m_doc.ClassName());
            }
            m_status->SetText(line.AsView());
            m_errorView->SetText(StringView(u8""));
            return;
        }
        String summary;
        AppendCount(summary, errors.Size());
        summary.Append(errors.Size() == 1 ? u8" compile error" : u8" compile errors");
        m_status->SetText(summary.AsView());

        String detail;
        for (const draconic::script::ScriptSourceDocument::CompileError& e : errors)
        {
            if (!detail.IsEmpty())
            {
                detail.PushBack(utf8char('\n'));
            }
            detail.Append(e.module.IsEmpty() ? m_doc.FileName() : e.module.AsView());
            if (e.line > 0)
            {
                detail.PushBack(utf8char(':'));
                AppendCount(detail, static_cast<usize>(e.line));
            }
            detail.Append(u8": ");
            detail.Append(e.message.AsView());
        }
        m_errorView->SetText(detail.AsView());
    }

    void ScriptEditorPage::AppendCount(String& out, usize value)
    {
        utf8char digits[24];
        i32 n = 0;
        usize v = value;
        do
        {
            digits[n++] = static_cast<utf8char>('0' + v % 10);
            v /= 10;
        } while (v > 0 && n < 24);
        while (n > 0)
        {
            out.PushBack(digits[--n]);
        }
    }
}
