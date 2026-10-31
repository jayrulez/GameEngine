// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script - the `:backend_registry` partition.
//
// The backend registry. A language backend is a library that registers itself here
// ({languageId, extensions, factory}), and every consumer resolves through the registry by
// language or by the script file's extension. No consumer names a backend type: adding a
// language means linking its library and making one registration call.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.script:backend_registry;

import foundation.core;
import :script_manager;

using namespace foundation::core;

export namespace foundation::script
{
    struct ScriptBackendDesc
    {
        String languageId;            // canonical lowercase id: u8"angelscript", u8"luau"
        String displayName;           // editor-facing: u8"AngelScript"
        Array<String> fileExtensions; // lowercase, no dot: { u8"as" }
        // The allocator becomes the manager's own (MakeRef records it; everything
        // the manager creates inherits via MemoryAllocator()).
        Function<RefPtr<IScriptManager>(IAllocator&)> create;
    };

    class ScriptBackendRegistry
    {
    public:
        // Not inline; defined in ScriptContextImpl.cpp. Backends register from their own
        // libraries and hosts resolve from others, so a per-library copy of the registry would
        // leave a host with "no script backend found".
        [[nodiscard]] static ScriptBackendRegistry& Get();

        /// Idempotent by languageId: registering again replaces the entry, which suits hot reload.
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
    CreateScriptManagerForLanguage(StringView languageId, IAllocator& allocator)
    {
        const ScriptBackendDesc* backend = ScriptBackendRegistry::Get().FindByLanguage(languageId);
        if (backend == nullptr || !backend->create)
        {
            LOG_WARNING(u8"Script", u8"no script backend registered for language '{}'",
                                 languageId);
            return {};
        }
        return backend->create(allocator);
    }

    /// Create a manager for a script file by its extension. A path with an unknown or
    /// missing extension falls back to the sole registered backend (the common
    /// one-language project); ambiguity (several backends, no match) warns and fails.
    [[nodiscard]] inline RefPtr<IScriptManager> CreateScriptManagerForFile(StringView path,
                                                                            IAllocator& allocator)
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
                return backend->create ? backend->create(allocator) : RefPtr<IScriptManager>{};
            }
        }
        if (registry.All().Size() == 1)
        {
            const ScriptBackendDesc& sole = registry.All()[0];
            return sole.create ? sole.create(allocator) : RefPtr<IScriptManager>{};
        }
        LOG_WARNING(u8"Script",
                             u8"no script backend matches '{}' ({} backend(s) registered)", path,
                             registry.All().Size());
        return {};
    }
}
