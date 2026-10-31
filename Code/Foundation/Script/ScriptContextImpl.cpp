// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The current-script-context slot. It is defined here, one per process per thread,
// because the pushers (script backends) and readers (native facades) live in
// different libraries; an inline thread_local in the interface would be duplicated
// per shared library.

module;
#include "Core/Prelude.h"

module foundation.script;

import foundation.core;

namespace foundation::script
{
    ScriptBackendRegistry& ScriptBackendRegistry::Get()
    {
        static ScriptBackendRegistry instance;
        return instance;
    }
} // namespace foundation::script

namespace foundation::script::detail
{
    IScriptContext*& CurrentScriptContextSlot() noexcept
    {
        static thread_local IScriptContext* slot = nullptr;
        return slot;
    }
} // namespace foundation::script::detail

namespace foundation::script
{
    // The facades' delegate shapes: one store per process, beside the context slot, since the
    // registering facades and the declaring backends live in different libraries.
    namespace
    {
        core::Array<core::String>& DelegateShapeStorage()
        {
            static core::Array<core::String> shapes;
            return shapes;
        }
    }

    void RegisterDelegateShape(core::StringView declaration)
    {
        for (const core::String& existing : DelegateShapeStorage())
        {
            if (existing.AsView() == declaration)
            {
                return; // idempotent
            }
        }
        DelegateShapeStorage().PushBack(core::String(declaration));
    }

    core::Span<const core::String> DelegateShapes() { return DelegateShapeStorage().AsSpan(); }
} // namespace foundation::script
