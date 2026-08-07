// Draconic::EditorScript - the `draconic.editor.script` module.
//
// ScriptApiSurface implementation: the ONE bound-API build for a page. Replays the
// runtime's exact registration sequence (ScriptSubsystem) against a throwaway manager -
// idempotent registries + a manager that dies right after DescribeBoundApi - and caches
// the result for the page's lifetime. Completion and the API browser both read this.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.editor.script;

import draconic.foundation;
import draconic.script;
import draconic.script.facades;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace script = draconic::script;

    const Array<script::ScriptApiType>& ScriptApiSurface::Types() const
    {
        if (m_built || m_language.IsEmpty())
        {
            return m_types;
        }
        m_built = true; // one attempt; a language without a backend just stays empty

        foundation::RegisterFoundationTypes();
        script::RegisterScriptFacadeReflection();
        RefPtr<script::IScriptManager> manager =
            script::CreateScriptManagerForLanguage(m_language.AsView());
        if (manager.Get() == nullptr)
        {
            return m_types;
        }
        script::RegisterReflectedTypes(*manager);
        m_types = manager->DescribeBoundApi();
        return m_types;
    }
}
