// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - the `editor.script` module.
//
// ScriptApiSurface implementation: the single bound-API build for a page. It replays the
// runtime's registration sequence (as ScriptSubsystem runs it) against a throwaway manager. The
// registries are idempotent and the manager is destroyed right after DescribeBoundApi; the result
// is cached for the page's lifetime. Completion and the API browser both read it.

module;
#include "Core/Prelude.h"

module editor.script;

import foundation.core;
import foundation.script;
import foundation.script.facades;

using namespace foundation::core;
namespace core = foundation::core;

namespace editor
{
    namespace script = foundation::script;

    const Array<script::ScriptApiType>& ScriptApiSurface::Types() const
    {
        if (m_built || m_language.IsEmpty())
        {
            return m_types;
        }
        m_built = true; // one attempt; a language without a backend stays empty

        core::RegisterCoreTypes();
        foundation::script::RegisterScriptFacadeReflection();
        RefPtr<script::IScriptManager> manager =
            script::CreateScriptManagerForLanguage(m_language.AsView(), editor::EditorRootAllocator());
        if (manager.Get() == nullptr)
        {
            return m_types;
        }
        foundation::script::RegisterReflectedTypes(*manager);
        m_types = manager->DescribeBoundApi();
        return m_types;
    }
}
