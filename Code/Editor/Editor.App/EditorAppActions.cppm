// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :app_actions partition.
//
// The editor-wide actions (File, Edit, Project, Build, Game, View, Help), out of the application
// (Documentation/Specs/editor-gui.md, 5.1, layer 3): one registration per menu, each declaration
// as the menu bar, the shortcuts, the palette and the MCP bridge read it. Registered in menu
// order (the bar lists menus as their names first appear). An action runs what the application
// gives it; one it gives nothing for (a dialog not on the GUI yet) is left out until it is, so a
// surface never offers what cannot run.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:app_actions;

import foundation.core;
import editor.core;
import :playback_actions;

using namespace foundation::core;

export namespace editor::app
{
    namespace detail
    {
        inline EditorActionDeclaration Declare(StringView id, StringView label, StringView description, StringView menuPath, i32 order)
        {
            EditorActionDeclaration d;
            d.id = String(id);
            d.label = String(label);
            d.description = String(description);
            d.menuPath = String(menuPath);
            d.menuOrder = order;
            return d;
        }
        // Registered when it can run: `run` given.
        inline void Add(EditorActionRegistry& actions, EditorActionDeclaration d, Function<void()> run)
        {
            if (!run)
                return;
            d.execute = [run = Move(run)](EditorPage*) { run(); };
            (void)actions.Register(Move(d));
        }
    }

    /// What File's actions run.
    struct FileActions
    {
        Function<void(EditorPage&)> save;
        Function<void(EditorPage&)> saveAs;
        Function<void()> saveLayout;
        Function<void()> closeProject; // only when the manager launched the editor
        Function<void()> exit;
    };
    inline void RegisterFileActions(EditorActionRegistry& actions, FileActions run)
    {
        if (run.save)
        {
            EditorActionDeclaration d = detail::Declare(u8"file.save", u8"Save", u8"Save the active page to its source asset", u8"File/Save", 100);
            d.enabled = [](EditorPage* page) { return page != nullptr && page->IsDirty(); };
            d.shortcut = EditorShortcut::Of(u8"ctrl-s");
            d.execute = [save = Move(run.save)](EditorPage* page) { save(*page); };
            (void)actions.Register(Move(d));
        }
        if (run.saveAs)
        {
            EditorActionDeclaration d = detail::Declare(u8"file.saveAs", u8"Save As...", u8"Save the active page as a new source asset",
                                                        u8"File/Save As...", 101);
            d.enabled = [](EditorPage* page) { return page != nullptr; };
            d.execute = [saveAs = Move(run.saveAs)](EditorPage* page) { saveAs(*page); };
            (void)actions.Register(Move(d));
        }
        detail::Add(actions,
                    detail::Declare(u8"file.saveLayout", u8"Save Layout", u8"Save the panel layout as the default for this editor",
                                    u8"File/Save Layout", 200),
                    Move(run.saveLayout));
        detail::Add(actions,
                    detail::Declare(u8"file.closeProject", u8"Close Project", u8"Close the project and return to the project manager",
                                    u8"File/Close Project", 300),
                    Move(run.closeProject));
        detail::Add(actions, detail::Declare(u8"file.exit", u8"Exit", u8"Exit the editor", u8"File/Exit", 301), Move(run.exit));
    }

    /// What Edit's actions run (undo, redo and discard work on the subject page itself).
    struct EditActions
    {
        Function<void()> preferences;
        Function<void()> exportTemplates;
    };
    inline void RegisterEditActions(EditorActionRegistry& actions, EditActions run)
    {
        {
            EditorActionDeclaration d = detail::Declare(u8"edit.undo", u8"Undo", u8"Undo the active page's last edit", u8"Edit/Undo", 100);
            d.enabled = [](EditorPage* page) { return page != nullptr && page->Commands().CanUndo(); };
            d.shortcut = EditorShortcut::Of(u8"ctrl-z");
            d.execute = [](EditorPage* page) { page->Commands().Undo(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = detail::Declare(u8"edit.redo", u8"Redo", u8"Redo the active page's last undone edit", u8"Edit/Redo", 101);
            d.enabled = [](EditorPage* page) { return page != nullptr && page->Commands().CanRedo(); };
            d.shortcut = EditorShortcut::Of(u8"ctrl-shift-z");
            d.alternateShortcut = EditorShortcut::Of(u8"ctrl-y");
            d.execute = [](EditorPage* page) { page->Commands().Redo(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = detail::Declare(u8"page.discardChanges", u8"Discard Changes", u8"Revert the page's unsaved edits", u8"", 0);
            d.enabled = [](EditorPage* page) { return page != nullptr && page->IsDirty(); };
            d.execute = [](EditorPage* page) { page->DiscardChanges(); };
            (void)actions.Register(Move(d));
        }
        RegisterPlaybackActions(actions);
        detail::Add(actions,
                    detail::Declare(u8"edit.preferences", u8"Preferences...", u8"Open the per-user editor preferences", u8"Edit/Preferences...", 200),
                    Move(run.preferences));
        detail::Add(actions,
                    detail::Declare(u8"edit.exportTemplates", u8"Export Templates...",
                                    u8"Install, inspect and remove the export templates this editor exports with", u8"Edit/Export Templates...",
                                    201), // the editor's, beside Preferences: every project shares them
                    Move(run.exportTemplates));
    }

    namespace detail
    {
        // Enabled while `context` has a project open.
        inline EditorActionDeclaration NeedsProject(const EditorContext* context, EditorActionDeclaration d)
        {
            d.enabled = [context](EditorPage*) { return context != nullptr && context->Project() != nullptr; };
            return d;
        }
    }

    /// What Project's actions run; most need a project open in `context`.
    struct ProjectActions
    {
        const EditorContext* context = nullptr;
        Function<void()> settings;
        Function<void()> addNativeCode;
        Function<void()> buildNativeModule;
        Function<void()> reloadNativeModule;
        Function<void()> exportPresets;
        Function<void()> reportResourceMemory;
    };
    inline void RegisterProjectActions(EditorActionRegistry& actions, ProjectActions run)
    {
        const EditorContext* context = run.context;
        detail::Add(actions,
                    detail::NeedsProject(context, detail::Declare(u8"project.settings", u8"Project Settings...", u8"Open the project settings",
                                                                  u8"Project/Project Settings...", 100)),
                    Move(run.settings));
        detail::Add(actions,
                    detail::NeedsProject(context, detail::Declare(u8"project.addNativeCode", u8"Add Native Code...",
                                                                  u8"Scaffold a native code module under Native/", u8"Project/Add Native Code...", 200)),
                    Move(run.addNativeCode));
        detail::Add(actions,
                    detail::NeedsProject(context, detail::Declare(u8"project.buildNativeModule", u8"Build Native Module",
                                                                  u8"Build the project's native module on the job service",
                                                                  u8"Project/Build Native Module", 201)),
                    Move(run.buildNativeModule));
        detail::Add(actions,
                    detail::NeedsProject(context, detail::Declare(u8"project.reloadNativeModule", u8"Reload Native Module",
                                                                  u8"Reload the project's built native module", u8"Project/Reload Native Module", 202)),
                    Move(run.reloadNativeModule));
        detail::Add(actions,
                    detail::NeedsProject(context, detail::Declare(u8"project.export", u8"Export...", u8"Open the export presets panel",
                                                                  u8"Project/Export...", 300)),
                    Move(run.exportPresets));
        if (run.reportResourceMemory)
        {
            EditorActionDeclaration d =
                detail::Declare(u8"project.reportResourceMemory", u8"Report Resource Memory",
                                u8"Log the resident resources by type (unreferenced = purge candidates)", u8"Project/Report Resource Memory", 400);
            d.readOnly = true;
            detail::Add(actions, Move(d), Move(run.reportResourceMemory));
        }
    }

    /// What Build's actions run; they need a project open in `context`.
    struct BuildActions
    {
        const EditorContext* context = nullptr;
        Function<void()> cookAll;
        Function<void()> rebuildAll;
        /// The page's asset and what it needs cooked (rebuild: again, whether changed or not),
        /// its unsaved changes saved first so the cook reads them.
        Function<void(EditorPage& page)> cookPage;
        Function<void(EditorPage& page)> rebuildPage;
    };
    inline void RegisterBuildActions(EditorActionRegistry& actions, BuildActions run)
    {
        detail::Add(actions,
                    detail::NeedsProject(run.context, detail::Declare(u8"build.cookAll", u8"Cook All", u8"Cook the dirty assets into the cooked database",
                                                                      u8"Build/Cook All", 100)),
                    Move(run.cookAll));
        detail::Add(actions,
                    detail::NeedsProject(run.context, detail::Declare(u8"build.rebuildAll", u8"Rebuild All", u8"Cook every asset again",
                                                                      u8"Build/Rebuild All", 101)),
                    Move(run.rebuildAll));
        // An asset page's: on its bar, and in Build's menu over the active page. Where the old
        // collision page had its own Cook now, every asset page has these.
        const EditorContext* context = run.context;
        const auto pageAction = [&actions, context](StringView id, StringView label, StringView description, StringView menuPath, i32 order,
                                                    Function<void(EditorPage&)> cook)
        {
            if (!cook)
                return;
            EditorActionDeclaration d = detail::Declare(id, label, description, menuPath, order);
            d.enabled = [context](EditorPage* page) { return context != nullptr && context->Project() != nullptr && page != nullptr && !page->InstanceId().IsNil(); };
            d.disabledReason = [context](EditorPage* page)
            {
                if (context == nullptr || context->Project() == nullptr)
                    return String(u8"Open a project first");
                return page == nullptr ? String(u8"No page is active") : String(u8"This page shows no asset");
            };
            d.execute = [cook = Move(cook)](EditorPage* page)
            {
                if (page != nullptr)
                    cook(*page);
            };
            (void)actions.Register(Move(d));
        };
        pageAction(u8"build.cookPage", u8"Cook This Asset", u8"Save the page's changes, then cook its asset and what it needs", u8"Build/Cook This Asset", 200,
                   Move(run.cookPage));
        pageAction(u8"build.rebuildPage", u8"Rebuild This Asset", u8"Save the page's changes, then cook its asset and what it needs again, changed or not",
                   u8"Build/Rebuild This Asset", 201, Move(run.rebuildPage));
    }

    /// What Game's actions run; they need a project open in `context`.
    struct GameActions
    {
        const EditorContext* context = nullptr;
        Function<void()> play;
        Function<void()> playNewInstance;
    };
    inline void RegisterGameActions(EditorActionRegistry& actions, GameActions run)
    {
        detail::Add(actions,
                    detail::NeedsProject(run.context, detail::Declare(u8"game.play", u8"Play", u8"Play the project in the Game page", u8"Game/Play", 100)),
                    Move(run.play));
        detail::Add(actions,
                    detail::NeedsProject(run.context, detail::Declare(u8"game.playNewInstance", u8"Play New Instance",
                                                                      u8"Play the project in a fresh Game page", u8"Game/Play New Instance", 101)),
                    Move(run.playNewInstance));
    }

    /// What View's actions run.
    struct ViewActions
    {
        Function<void()> resetLayout;
        Function<void()> profiler;
        Function<bool()> profilerRecording;
        Function<void()> profilerRecord;
        Function<void()> screenshot;
        Function<void()> commandPalette;
    };
    inline void RegisterViewActions(EditorActionRegistry& actions, ViewActions run)
    {
        detail::Add(actions, detail::Declare(u8"view.resetLayout", u8"Reset Layout", u8"Reset the panel layout to the default", u8"View/Reset Layout", 100),
                    Move(run.resetLayout));
        if (run.profiler)
        {
            EditorActionDeclaration d = detail::Declare(
                u8"view.profiler", u8"Profiler",
                u8"Show the Profiler page: record the editor process, open a player's capture", u8"View/Profiler", 200);
            d.readOnly = true; // shows a panel; nothing in the project changes
            detail::Add(actions, Move(d), Move(run.profiler));
        }
        if (run.profilerRecord)
        {
            EditorActionDeclaration d = detail::Declare(
                u8"profiler.record", u8"Profiler Record",
                u8"Record the editor process in the Profiler page (every page, preview and Play-in-editor run), or stop", u8"View/Profiler Record", 201);
            d.kind = EditorActionKind::Toggle;
            d.readOnly = true; // the profiler's state, not the project's
            d.checked = [recording = Move(run.profilerRecording)](EditorPage*) { return recording && recording(); };
            detail::Add(actions, Move(d), Move(run.profilerRecord));
        }
        if (run.screenshot)
        {
            EditorActionDeclaration d = detail::Declare(
                u8"view.screenshot", u8"Screenshot Editor",
                u8"Write the editor's main window, as it looks now, to a PNG under <user-data>/screenshots", u8"View/Screenshot Editor", 300);
            d.shortcut = EditorShortcut::Of(u8"f12");
            d.readOnly = true; // a file outside the project; nothing in it changes
            detail::Add(actions, Move(d), Move(run.screenshot));
        }
        if (run.commandPalette)
        {
            EditorActionDeclaration d = detail::Declare(u8"view.commandPalette", u8"Command Palette...",
                                                        u8"Every action by name: type to filter, Enter runs it", u8"View/Command Palette...", 200);
            d.shortcut = EditorShortcut::Of(u8"ctrl-shift-p");
            d.readOnly = true; // the palette changes nothing itself
            detail::Add(actions, Move(d), Move(run.commandPalette));
        }
    }

    /// What Help's actions run.
    struct HelpActions
    {
        Function<void()> about;
    };
    inline void RegisterHelpActions(EditorActionRegistry& actions, HelpActions run)
    {
        if (!run.about)
            return;
        EditorActionDeclaration d = detail::Declare(u8"help.about", u8"About", u8"The editor's version", u8"Help/About", 100);
        d.readOnly = true;
        detail::Add(actions, Move(d), Move(run.about));
    }
}
