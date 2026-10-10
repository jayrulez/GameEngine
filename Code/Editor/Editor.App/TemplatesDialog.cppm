// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :templates_dialog partition.
//
// TemplatesDialog: the export templates this editor exports with, Edit > Export Templates (and
// Export's own button, over it). They are the editor's, not a project's: every project shares them. The templates on the left, each by the icon its
// bundle carries, its name, platform, config and engine version, the host build marked as this
// editor's own; the selected one's details on the right (its id, compiler, engine version and
// whether it matches this editor's, where it lives, its player, runtime files, symbols and notes),
// with Reveal and Remove. Install from Folder copies a template bundle in; Create from Build makes
// one from a Bin/<Config> build dir. The list refreshes in place. What it needs from outside (the
// registry, the OS folder picker, opening a folder) comes through TemplatesDialogSeams, so a test
// drives it without a window.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:templates_dialog;

import foundation.core;
import foundation.ui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    /// What the templates dialog asks of the application.
    struct TemplatesDialogSeams
    {
        /// The templates root installs and removals go to (Preferences > Export, or the default).
        Function<String()> templatesRoot;
        /// Fill `registry` with the templates this host sees (the root's, then the host build).
        Function<void(editor::TemplateRegistry&)> refresh;
        /// Ask for a folder; `chosen` hears its path, or nothing when the user cancels.
        Function<void(Function<void(String)> chosen)> pickFolder;
        /// Show a folder in the OS file manager.
        Function<void(StringView)> revealFolder;
    };

    class TemplatesDialog final : public ui::Dialog
    {
        RTTI_OBJECT(TemplatesDialog, ui::Dialog)
    public:
        TemplatesDialog(editor::EditorContext& context, TemplatesDialogSeams seams);
        ~TemplatesDialog() override;

        /// Re-read the templates, keeping the selected one selected while it is there.
        void Refresh();

        /// The templates listed, in order (the registry's).
        [[nodiscard]] usize TemplateCount() const noexcept { return m_registry.Count(); }
        [[nodiscard]] const editor::ExportTemplate* TemplateAt(usize index) const
        {
            return index < m_registry.Count() ? m_registry.At(index) : nullptr;
        }
        /// The selected template, or null.
        [[nodiscard]] const editor::ExportTemplate* Selected() const;
        void Select(usize index);

        /// The details side's actions, for tests: Remove asks first (Confirm answers it).
        [[nodiscard]] ui::Button& RemoveButton() const noexcept { return *m_remove; }
        [[nodiscard]] ui::Button& RevealButton() const noexcept { return *m_reveal; }
        /// The detail value shown for a field label ("Engine", "Folder", ...), for tests.
        [[nodiscard]] StringView DetailText(StringView label) const;

        void InstallFromFolder();
        void CreateFromBuild();
        /// Remove the selected template, once the user confirms (`confirmed` skips the question).
        void RemoveSelected(bool confirmed = false);

    private:
        class Adapter;
        struct Detail
        {
            String label;
            ui::Label* value = nullptr;
        };

        void BuildDetails(ui::FlexLayout& column);
        void ShowDetails();
        /// The icon a template is shown by, made once per template id.
        [[nodiscard]] ui::DrawablePtr IconFor(const editor::ExportTemplate& tmpl);
        ui::Label* AddDetail(ui::FlexLayout& column, StringView label);
        void SetDetail(StringView label, StringView text);

        editor::EditorContext* m_context;
        TemplatesDialogSeams m_seams;
        editor::TemplateRegistry m_registry;
        UniquePtr<Adapter> m_adapter;
        RefPtr<ui::ListView> m_list;
        HashMap<String, ui::DrawablePtr> m_icons; // by template id (and directory: a reinstall)
        ui::DrawableView* m_detailIcon = nullptr;
        ui::Label* m_detailName = nullptr;
        ui::Label* m_detailId = nullptr;
        ui::DrawableView* m_mismatchIcon = nullptr;
        ui::Label* m_emptyNote = nullptr;
        ui::FlexLayout* m_detailBody = nullptr;
        Array<Detail> m_details;
        ui::Button* m_reveal = nullptr;
        ui::Button* m_remove = nullptr;
    };

    RTTI_DEFINE_OBJECT(TemplatesDialog, "rtti::editor::editor::app")
}
