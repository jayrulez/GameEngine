// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - the `editor.script` module.
//
// ScriptEditorPage: a behaviour script's code on the GUI, built from its markup
// (Data/Editor/ScriptPage.xml) on the code pages' base: the source file, its language's colours,
// a check shortly after each edit that fills the problems list, one undo history, and Save
// followed by a cook. The check compiles the code as the cook does without running any of it, so
// a failing check leaves the last good class running. Completion and hover come from what the
// runtime binds for the language (:script_language_service), which the API browser, opened from
// the page's bar, shows as a tree beside the code. The gutter's breakpoints belong to the editor
// (a Game run applies them), and while a run is paused in this script its line is marked and
// hovering a name shows its value.
//
// The page is backend-neutral and never names a language: its cook, API and colours are found by
// the asset's language id.

module;
#include "Core/Prelude.h"

export module editor.script;

export import :script_api;
export import :script_language_service;

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

export namespace editor
{
    class ScriptEditorPage final : public app::CodeAssetPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ScriptPage.xml";

        ScriptEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~ScriptEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;

        /// Opens the API browser beside the code, or closes it.
        void ShowApi(bool show);
        [[nodiscard]] bool ApiShown() const noexcept { return m_apiShown; }
        /// Keeps the API rows whose type or member contains `text` (all rows when empty).
        void SearchApi(StringView text);
        /// Inserts the name of the API browser's row `modelNode` (its tree model's id) at the
        /// caret.
        void InsertApi(i32 modelNode);

        // === For tests ===
        [[nodiscard]] StringView Language() const noexcept { return m_doc.Language(); }
        [[nodiscard]] StringView ClassName() const noexcept { return m_doc.ClassName(); }
        [[nodiscard]] const ScriptApiTree& ApiTree() const noexcept { return m_apiTree; }
        [[nodiscard]] foundation::gui::TreeModel* ApiModel() const noexcept { return m_apiModel.Get(); }
        [[nodiscard]] ScriptLanguageService* Service() const noexcept { return m_service.Get(); }
        /// Sets or clears the gutter's breakpoint on `line` (0-based).
        void ToggleBreakpoint(u32 line);
        [[nodiscard]] bool HasBreakpointMark(u32 line) const;

    protected:
        [[nodiscard]] Array<app::CodeProblem> CheckCode(StringView text) override;
        /// "No problems: compiles class Player", naming the class the check compiled.
        [[nodiscard]] String CleanSummary() const override;

    private:
        void ShowApiTree();
        void ShowExecution();

        pipeline::ScriptSourceDocument m_doc; // runs the compile check the cook runs
        ScriptApiSurface m_surface;
        UniquePtr<ScriptLanguageService> m_service;
        ScriptApiTree m_apiTree;
        String m_apiSearch;
        bool m_apiShown = false;
        bool m_apiBuilt = false;
        RefPtr<foundation::gui::Node> m_apiPane; // the browser; removed from the splitter while closed
        foundation::gui::Splitter* m_body = nullptr;
        UniquePtr<foundation::gui::TreeModel> m_apiModel;
        Array<i32> m_apiNodes; // the tree node of each model node, indexed by the model's id
        foundation::gui::TreeView* m_apiView = nullptr;
        foundation::gui::ButtonBase* m_apiToggle = nullptr;
        Array<u32> m_breakpoints; // the gutter's marks
        u32 m_executionMark = 0;  // the paused line's mark (0 when none)
        u64 m_executionSeen = ~u64(0);
    };

    class ScriptClassPageFactory final : public IEditorPageFactory
    {
    public:
        explicit ScriptClassPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the script page; called from the editor's assembly. The New Asset creators (one
    /// per tier for each backend with a cook) are registered by the script pipeline
    /// (pipeline::RegisterScriptCreators).
    inline void RegisterScriptEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<ScriptClassPageFactory>(services), editor::EditorRootAllocator()));
    }
}
