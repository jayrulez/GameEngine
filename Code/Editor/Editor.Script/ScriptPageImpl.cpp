// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - the script page's implementation (see ScriptPage.cppm).

module;
#include "Core/Prelude.h"

module editor.script;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.script;
import foundation.gui;
import foundation.gui.code;
import script.pipeline;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace editor
{
    ScriptEditorPage::ScriptEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::CodeAssetPage(context, Move(services), instance, pipeline::ScriptClassAsset::StaticType(), kPath)
    {
        HideAssetRows(); // the code is the page: no grid
        if (const auto* asset = ObjectAs<pipeline::ScriptClassAsset>())
            m_doc.Bind(context.Project() != nullptr ? context.Project()->SourcesRoot().AsView() : StringView{}, asset->fileName.View(), asset->language.AsView());
        m_surface.SetLanguage(m_doc.Language());
        const gui::CodeLanguage* language = Services().languages != nullptr ? Services().languages->Find(m_doc.Language()) : nullptr;
        m_service = MakeUnique<ScriptLanguageService>(Allocator(), m_surface, language);
        ScriptEditorPage* self = this;
        // While a run is paused in this script, a name's value on hover.
        m_service->SetValueProbe([self](StringView name) -> String
                                 {
                                     const EditorContext::ScriptExecutionPoint& point = self->Context().ScriptExecution();
                                     if (!point.active || point.file.AsView() != self->m_doc.FileName() || !self->Context().ScriptValueProbe)
                                         return String{};
                                     return self->Context().ScriptValueProbe(name);
                                 });
        if (gui::CodeEditor* code = Code())
        {
            code->SetLanguageService(m_service.Get());
            // The breakpoints are the editor's (a Game run applies them; the store is 1-based).
            for (const EditorContext::ScriptBreakpoint& breakpoint : context.Breakpoints())
                if (breakpoint.file.AsView() == m_doc.FileName() && breakpoint.line >= 1)
                    m_breakpoints.PushBack(code->AddMarker(static_cast<u32>(breakpoint.line - 1), u8"breakpoint"));
            code->SetOnGutterClick([self](u32 line) { self->ToggleBreakpoint(line); });
        }

        // The API browser: its parts found while it is in the page, then out of the splitter
        // until it is opened.
        m_body = Find<gui::Splitter>(u8"script-body");
        m_apiPane = RefPtr<gui::Node>(Find<gui::Node>(u8"script-api-pane"));
        m_apiView = Find<gui::TreeView>(u8"script-api-tree");
        auto* apiSearch = Find<gui::TextField>(u8"script-api-search");
        if (m_apiView != nullptr)
        {
            m_apiModel = MakeUnique<gui::TreeModel>(Allocator());
            m_apiView->SetModel(m_apiModel.Get());
            m_apiView->SetOnActivated([self](gui::ModelIndex index)
                                      {
                                          if (index.IsValid())
                                              self->InsertApi(static_cast<i32>(index.InternalId));
                                      });
        }
        if (apiSearch != nullptr)
            apiSearch->SetOnTextChanging([self](StringView text) { self->SearchApi(text); });
        if (m_body != nullptr && m_apiPane)
            m_body->RemoveChild(m_apiPane.Get());
        m_apiToggle = Find<gui::ButtonBase>(u8"script-api");
        if (m_apiToggle != nullptr)
            m_apiToggle->SetOnClick([self]() { self->ShowApi(!self->m_apiShown); });

        OpenCode(m_doc.Language());
    }

    ScriptEditorPage::~ScriptEditorPage()
    {
        if (m_apiView != nullptr)
            m_apiView->SetModel(nullptr);
        if (gui::CodeEditor* code = Code())
            code->SetLanguageService(nullptr);
    }

    Array<app::CodeProblem> ScriptEditorPage::CheckCode(StringView text)
    {
        // Compiled as the cook compiles it, none of it run.
        m_doc.SetSource(text);
        (void)m_doc.Validate();
        Array<app::CodeProblem> problems;
        for (const pipeline::ScriptSourceDocument::CompileError& e : m_doc.Errors())
        {
            app::CodeProblem p;
            p.severity = gui::CodeSeverity::Error;
            p.message = e.message;
            p.file = e.module.IsEmpty() || e.module.AsView() == m_doc.FileName() ? String{} : e.module;
            p.line = e.line > 0 ? static_cast<u32>(e.line - 1) : 0;
            p.atColumn = e.column > 0;
            p.column = e.column > 0 ? static_cast<u32>(e.column - 1) : 0;
            problems.PushBack(Move(p));
        }
        return problems;
    }

    String ScriptEditorPage::CleanSummary() const
    {
        return m_doc.ClassName().IsEmpty() ? String{} : Format(u8"No problems: compiles class {}", m_doc.ClassName());
    }

    void ScriptEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::CodeAssetPage::OnUpdate(host, dt);
        if (m_executionSeen != Context().ScriptExecutionVersion())
        {
            m_executionSeen = Context().ScriptExecutionVersion();
            ShowExecution();
        }
    }

    // A Game run paused in this script: its line marked and brought into view; else no mark.
    void ScriptEditorPage::ShowExecution()
    {
        gui::CodeEditor* code = Code();
        if (code == nullptr)
            return;
        if (m_executionMark != 0)
        {
            code->RemoveMarker(m_executionMark);
            m_executionMark = 0;
        }
        const EditorContext::ScriptExecutionPoint& point = Context().ScriptExecution();
        if (!point.active || point.file.AsView() != m_doc.FileName() || point.line < 1)
            return;
        const u32 line = static_cast<u32>(point.line - 1);
        m_executionMark = code->AddMarker(line, u8"execution");
        code->SetCaret(gui::CodePosition{line, 0});
        code->ScrollToCaret();
    }

    void ScriptEditorPage::ToggleBreakpoint(u32 line)
    {
        gui::CodeEditor* code = Code();
        if (code == nullptr || m_doc.FileName().IsEmpty())
            return;
        Context().ToggleBreakpoint(m_doc.FileName(), static_cast<i32>(line) + 1);
        for (usize i = 0; i < m_breakpoints.Size(); ++i)
            if (code->MarkerLine(m_breakpoints[i]) == line)
            {
                code->RemoveMarker(m_breakpoints[i]);
                m_breakpoints.RemoveAt(i);
                return;
            }
        m_breakpoints.PushBack(code->AddMarker(line, u8"breakpoint"));
    }

    bool ScriptEditorPage::HasBreakpointMark(u32 line) const
    {
        return Code() != nullptr && Code()->HasMarker(line, u8"breakpoint");
    }

    void ScriptEditorPage::ShowApi(bool show)
    {
        m_apiShown = show;
        if (m_apiToggle != nullptr)
            m_apiToggle->SetChecked(show);
        if (m_body == nullptr || !m_apiPane)
            return;
        const bool in = m_apiPane->GetParent() == m_body;
        if (show && !in)
            m_body->AddChild(m_apiPane.Get());
        else if (!show && in)
            m_body->RemoveChild(m_apiPane.Get());
        // Built the first time it opens: the API is described by a script manager made for it.
        if (show && !m_apiBuilt)
        {
            m_apiBuilt = true;
            ShowApiTree();
        }
    }

    void ScriptEditorPage::SearchApi(StringView text)
    {
        m_apiSearch = String(Trim(text));
        if (m_apiBuilt)
            ShowApiTree();
    }

    void ScriptEditorPage::ShowApiTree()
    {
        m_apiTree = BuildScriptApiTree(m_surface.Types(), m_apiSearch.AsView());
        if (!m_apiModel)
            return;
        // The model's ids are the tree's node indices: nodes are added in their index order.
        m_apiModel->Clear();
        Array<i32> modelOf;
        for (usize i = 0; i < m_apiTree.nodes.Size(); ++i)
            modelOf.PushBack(-1);
        for (const i32 root : m_apiTree.roots)
        {
            const ScriptApiTreeNode& type = m_apiTree.nodes[static_cast<usize>(root)];
            modelOf[static_cast<usize>(root)] = m_apiModel->AddNode(gui::TreeModel::kRoot, type.label.AsView());
            for (const i32 child : type.children)
                modelOf[static_cast<usize>(child)] = m_apiModel->AddNode(modelOf[static_cast<usize>(root)], m_apiTree.nodes[static_cast<usize>(child)].label.AsView());
        }
        m_apiNodes.Clear();
        for (usize i = 0; i < m_apiTree.nodes.Size(); ++i)
            m_apiNodes.PushBack(-1);
        for (usize i = 0; i < modelOf.Size(); ++i)
            if (modelOf[i] >= 0)
                m_apiNodes[static_cast<usize>(modelOf[i])] = static_cast<i32>(i);
        // A search opens what it kept.
        if (m_apiView != nullptr && !m_apiSearch.IsEmpty())
            for (const i32 root : m_apiTree.roots)
                m_apiView->SetIdExpanded(static_cast<gui::ItemId>(modelOf[static_cast<usize>(root)]), true);
    }

    void ScriptEditorPage::InsertApi(i32 modelNode)
    {
        if (modelNode < 0 || static_cast<usize>(modelNode) >= m_apiNodes.Size())
            return;
        const i32 node = m_apiNodes[static_cast<usize>(modelNode)];
        gui::CodeEditor* code = Code();
        if (node < 0 || code == nullptr || code->IsReadOnly())
            return;
        code->Type(m_apiTree.nodes[static_cast<usize>(node)].insertText.AsView());
        code->RequestFocus();
    }

    const TypeInfo* ScriptClassPageFactory::PrimaryType() const
    {
        return &pipeline::ScriptClassAsset::StaticType();
    }

    UniquePtr<EditorPage> ScriptClassPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<ScriptEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
