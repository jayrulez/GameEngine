// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Editor's entry. Load adds an editor action, Project > Native Sample Notes, that says how
// many notes (the Pipeline part's asset type) the open project has; Unload removes it, since the
// action registry is not one the recording reverses.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.content; // Instance (a note)
import editor.core;
import editor.project; // EditorProject (its source database)
import nativesample.pipeline;

namespace
{
    constexpr foundation::core::StringView kAction = u8"nativesample.notes";

    void Load(foundation::runtime::PluginContext& context)
    {
        editor::EditorContext* editor = context.Find<editor::EditorContext>();
        if (editor == nullptr)
        {
            return;
        }
        editor::EditorActionDeclaration action;
        action.id = foundation::core::String(kAction);
        action.label = foundation::core::String(u8"Native Sample Notes");
        action.description = foundation::core::String(u8"Say how many notes the project has (NativeSample's own asset type)");
        action.menuPath = foundation::core::String(u8"Project/Native Sample Notes");
        action.menuOrder = 900;
        action.readOnly = true;
        action.execute = [editor](editor::EditorPage*)
        {
            if (editor->Project() == nullptr)
            {
                return;
            }
            foundation::core::usize count = 0;
            nativesample::ForEachNote(*editor->Project()->SourceDb().RootGroup(), [&count](foundation::content::Instance&) { ++count; });
            editor->Notify(editor::NoticeKind::Info,
                           foundation::core::Format(u8"The project has {} note{}.", count, count == 1 ? u8"" : u8"s").AsView());
        };
        (void)editor->Actions().Register(foundation::core::Move(action));
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        if (editor::EditorContext* editor = context.Find<editor::EditorContext>())
        {
            (void)editor->Actions().Unregister(kAction);
        }
    }
}

PLUGIN_PART(&Load, &Unload)
