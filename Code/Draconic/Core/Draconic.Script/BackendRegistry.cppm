// Draconic::Script - the `:backend_registry` partition.
//
// The backend registry (scripting.md B1): a language backend is a LIBRARY that
// registers itself here - {languageId, extensions, factory} - and every consumer
// resolves through the registry by language or by the script file's extension. No
// consumer names a backend type: adding AngelScript = link its library + one
// registration call, exactly how the Wren backend is added.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.script:backend_registry;

import draconic.foundation;
import :script_manager;

using namespace draconic::foundation;

export namespace draconic::script
{
    struct ScriptBackendDesc
    {
        String languageId;            // canonical lowercase id: u8"wren", u8"angelscript"
        String displayName;           // editor-facing: u8"Wren"
        Array<String> fileExtensions; // lowercase, no dot: { u8"wren" }
        Function<RefPtr<IScriptManager>()> create;
    };

    class ScriptBackendRegistry
    {
    public:
        [[nodiscard]] static ScriptBackendRegistry& Get()
        {
            static ScriptBackendRegistry instance;
            return instance;
        }

        /// Idempotent by languageId (a re-register replaces - hot-reload friendly).
        void Register(ScriptBackendDesc desc)
        {
            for (ScriptBackendDesc& existing : m_backends)
            {
                if (existing.languageId == desc.languageId)
                {
                    existing = Move(desc);
                    return;
                }
            }
            m_backends.PushBack(Move(desc));
        }

        [[nodiscard]] const ScriptBackendDesc* FindByLanguage(StringView languageId) const
        {
            for (const ScriptBackendDesc& backend : m_backends)
            {
                if (backend.languageId == languageId)
                {
                    return &backend;
                }
            }
            return nullptr;
        }

        /// `extension` without the dot, case-sensitive lowercase (callers lower it).
        [[nodiscard]] const ScriptBackendDesc* FindByExtension(StringView extension) const
        {
            for (const ScriptBackendDesc& backend : m_backends)
            {
                for (const String& ext : backend.fileExtensions)
                {
                    if (ext == extension)
                    {
                        return &backend;
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] Span<const ScriptBackendDesc> All() const noexcept
        {
            return Span<const ScriptBackendDesc>{m_backends.Data(), m_backends.Size()};
        }

    private:
        Array<ScriptBackendDesc> m_backends;
    };

    /// Create a manager for `languageId`; null (with a warning) when unknown.
    [[nodiscard]] inline RefPtr<IScriptManager>
    CreateScriptManagerForLanguage(StringView languageId)
    {
        const ScriptBackendDesc* backend = ScriptBackendRegistry::Get().FindByLanguage(languageId);
        if (backend == nullptr || !backend->create)
        {
            DRACONIC_LOG_WARNING(u8"Script", u8"no script backend registered for language '{}'",
                                 languageId);
            return {};
        }
        return backend->create();
    }

    /// Create a manager for a script FILE by its extension. A path with an unknown or
    /// missing extension falls back to the sole registered backend (the common
    /// one-language project); ambiguity (several backends, no match) warns and fails.
    [[nodiscard]] inline RefPtr<IScriptManager> CreateScriptManagerForFile(StringView path)
    {
        StringView extension;
        for (usize i = path.Size(); i-- > 0;)
        {
            if (path[i] == u8'.')
            {
                extension = path.SubStr(i + 1, path.Size() - (i + 1));
                break;
            }
            if (path[i] == u8'/' || path[i] == u8'\\')
            {
                break;
            }
        }
        const ScriptBackendRegistry& registry = ScriptBackendRegistry::Get();
        if (!extension.IsEmpty())
        {
            if (const ScriptBackendDesc* backend = registry.FindByExtension(extension))
            {
                return backend->create ? backend->create() : RefPtr<IScriptManager>{};
            }
        }
        if (registry.All().Size() == 1)
        {
            const ScriptBackendDesc& sole = registry.All()[0];
            return sole.create ? sole.create() : RefPtr<IScriptManager>{};
        }
        DRACONIC_LOG_WARNING(u8"Script",
                             u8"no script backend matches '{}' ({} backend(s) registered)", path,
                             registry.All().Size());
        return {};
    }
}
