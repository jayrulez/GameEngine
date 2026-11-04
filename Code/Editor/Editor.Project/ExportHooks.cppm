// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_hooks partition.
//
// What a plugin's Tooling part adds to an export, through the service its host provides
// (PluginContext::Find<ExportPluginServices>): hooks that run once a preset's dist is staged, for
// what a plugin's data cannot say (a platform's own packaging step, a file written from the
// project's settings). A hook that fails fails the export, with its reason. The service records what
// each plugin added, so the plugin host's unload takes it out again.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.project:export_hooks;

import foundation.core;
import foundation.runtime;
import :project;
import :export_preset;

using namespace foundation::core;

export namespace editor
{
    /// What an export hook is handed: the project, the preset and the staged dist's folder.
    struct ExportHookContext
    {
        EditorProject* project = nullptr;
        const ExportPreset* preset = nullptr;
        StringView outputDir;
    };

    /// A hook: runs after the dist is staged, and answers why it failed (empty when it did not), which
    /// fails the export.
    using ExportHook = Function<String(const ExportHookContext&)>;

    class ExportPluginServices final : public foundation::runtime::IRegistrationRecorder
    {
    public:
        explicit ExportPluginServices(IAllocator& allocator) : m_hooks(allocator) {}
        ~ExportPluginServices() override = default;

        ExportPluginServices(const ExportPluginServices&) = delete;
        ExportPluginServices& operator=(const ExportPluginServices&) = delete;

        /// Hands this service to `host`'s plugins and records what they add through it. Before the
        /// host loads; this outlives the host.
        [[nodiscard]] bool Attach(foundation::runtime::PluginHost& host)
        {
            if (!host.Provide(*this))
            {
                return false;
            }
            host.Loader().AddRecorder(this);
            return true;
        }

        /// Adds a hook every export runs after staging, in the order they were added. `name` says
        /// whose it is in a failure. False for an empty hook.
        bool AddHook(StringView name, ExportHook hook)
        {
            if (!hook)
            {
                return false;
            }
            const u64 token = m_nextToken++;
            m_hooks.PushBack(Entry{token, String(name), Move(hook)});
            if (m_sink != nullptr)
            {
                m_sink->PushBack(token);
            }
            return true;
        }

        [[nodiscard]] usize Count() const noexcept { return m_hooks.Size(); }

        /// Runs every hook over a staged dist; the first failure as "<name>: <reason>", or empty.
        [[nodiscard]] String Run(const ExportHookContext& context) const
        {
            for (const Entry& entry : m_hooks)
            {
                const String failed = entry.hook(context);
                if (!failed.IsEmpty())
                {
                    return Format(u8"{}: {}", entry.name, failed);
                }
                LOG_INFO(u8"Export", u8"export hook '{}' ran", entry.name);
            }
            return String{};
        }

        // IRegistrationRecorder: armed around each part's load, so what it added is recorded
        // against it, and reversed when it unloads.
        void Arm(Array<TypeId>& sink) override { m_sink = &sink; }
        void Disarm() override { m_sink = nullptr; }
        void Reverse(TypeId id) override
        {
            for (usize i = 0; i < m_hooks.Size(); ++i)
            {
                if (m_hooks[i].token == id)
                {
                    m_hooks.RemoveAt(i);
                    return;
                }
            }
        }

    private:
        struct Entry
        {
            u64 token; // this service's, what the recording keeps
            String name;
            ExportHook hook;
        };

        Array<Entry> m_hooks;
        Array<TypeId>* m_sink = nullptr;
        u64 m_nextToken = 1;
    };
}
