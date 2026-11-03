// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin_loader partition
//
// PluginLoader: owns the set of loaded plugins and their backing shared libraries
// and drives the load/unload contract against a SubsystemManager. Statically-created
// plugins are registered with Add() (caller keeps ownership of the object);
// shared-library plugins with Load() (the loader owns the library handle and
// closes it on teardown). It records what each plugin registers and reverses it on unload;
// PluginHost, which decides what a project loads, loads through it.

module;
#include "Core/Prelude.h"

export module foundation.runtime:plugin_loader;

import foundation.core;
import :subsystem_manager;
import :plugin;
import :plugin_part;

namespace core = foundation::core;

export namespace foundation::runtime
{
    // A registry the RegistrationScope can record: armed around a plugin's OnLoad, it
    // reports every real insert to the sink; Reverse undoes one. The two core registries
    // are built in; layers above (scene-manager contributions, Engine.Scene) plug theirs
    // in through PluginLoader::AddRecorder so one unload reverses everything a plugin did.
    class IRegistrationRecorder
    {
    public:
        virtual ~IRegistrationRecorder() = default;
        virtual void Arm(core::Array<core::TypeId>& sink) = 0;
        virtual void Disarm() = 0;
        virtual void Reverse(core::TypeId id) = 0;
    };

    class PluginLoader
    {
    public:
        explicit PluginLoader(SubsystemManager& subsystems) noexcept : m_subsystems(&subsystems) {}

        // Registers an extra recorder (borrowed; must outlive the host). Add it before loading
        // plugins: later loads record through it, earlier ones do not.
        void AddRecorder(IRegistrationRecorder* recorder)
        {
            if (recorder != nullptr)
            {
                m_recorders.PushBack(recorder);
            }
        }
        ~PluginLoader() { UnloadAll(); }

        PluginLoader(const PluginLoader&) = delete;
        PluginLoader& operator=(const PluginLoader&) = delete;

        // Registers an already-created plugin (the caller retains ownership of the
        // object) and calls OnLoad immediately. Returns the plugin for chaining.
        IRuntimePlugin* Add(IRuntimePlugin* plugin)
        {
            if (plugin == nullptr)
            {
                return nullptr;
            }
            Entry entry{plugin, nullptr, nullptr, core::DynamicLibrary{}, {}, {}, {}};
            RecordedOnLoad(entry);
            m_entries.PushBack(core::Move(entry));
            return plugin;
        }

        // Loads a part linked into this binary: calls its load with `context` (borrowed; it must
        // outlive the part's unload) under the recording, like a plugin's OnLoad.
        void AddPart(const PluginPart& part, PluginContext& context)
        {
            Entry entry{nullptr, &part, &context, core::DynamicLibrary{}, {}, {}, {}};
            RecordedOnLoad(entry);
            m_entries.PushBack(core::Move(entry));
        }

        // Loads a part from a shared library: resolves `symbol` (PluginPartSymbol), takes the part
        // it returns and loads it as AddPart does. The library is closed when the loader unloads.
        // NotFound for a missing library or symbol; Internal for an entry returning null.
        core::Result<const PluginPart*> LoadPart(core::StringView path, core::StringView symbol,
                                                 PluginContext& context)
        {
            core::DynamicLibrary library;
            if (core::Status status = library.Load(path); !status)
            {
                return core::Err(status.Code());
            }
            const auto entryPoint = library.GetSymbol<PluginPartEntryFn>(symbol);
            if (entryPoint == nullptr)
            {
                return core::Err(core::ErrorCode::NotFound);
            }
            const PluginPart* part = entryPoint();
            if (part == nullptr || part->load == nullptr)
            {
                return core::Err(core::ErrorCode::Internal);
            }
            Entry entry{nullptr, part, &context, core::Move(library), {}, {}, {}};
            RecordedOnLoad(entry);
            m_entries.PushBack(core::Move(entry));
            return part;
        }

        // Loads a plugin from a shared library: resolves the factory, creates the
        // plugin, and calls OnLoad. The library is closed when the host unloads.
        core::Result<IRuntimePlugin*> Load(core::StringView path)
        {
            core::DynamicLibrary library;
            if (core::Status status = library.Load(path); !status)
            {
                return core::Err(status.Code());
            }

            const auto create = library.GetSymbol<CreatePluginFn>(CreatePluginSymbol);
            if (create == nullptr)
            {
                return core::Err(core::ErrorCode::NotFound);
            }

            IRuntimePlugin* plugin = create();
            if (plugin == nullptr)
            {
                return core::Err(core::ErrorCode::Internal);
            }

            Entry entry{plugin, nullptr, nullptr, core::Move(library), {}, {}, {}};
            RecordedOnLoad(entry);
            m_entries.PushBack(core::Move(entry));
            return plugin;
        }

        [[nodiscard]] core::usize Count() const noexcept { return m_entries.Size(); }

        // Unloads everything in reverse load order: OnUnload each plugin, reverse its
        // recorded registrations (a registry entry or factory pointing into a closed
        // library would be a dangling read), then close libraries (which may invalidate
        // library-owned plugin objects).
        // closeLibraries=false is the hot-reload mode: the old modules stay mapped for
        // the process lifetime (leaked on purpose, so no page is freed under a pointer the
        // teardown missed) and the rebuilt module loads from a fresh versioned copy.
        void UnloadAll(bool closeLibraries = true)
        {
            for (core::usize i = m_entries.Size(); i-- > 0;)
            {
                if (m_entries[i].plugin != nullptr)
                {
                    m_entries[i].plugin->OnUnload(*m_subsystems);
                }
                else if (m_entries[i].part != nullptr && m_entries[i].part->unload != nullptr)
                {
                    m_entries[i].part->unload(*m_entries[i].context);
                }
                ReverseRecorded(m_entries[i]);
                if (!closeLibraries)
                {
                    m_entries[i].library.Detach();
                }
            }
            m_entries.Clear(); // DynamicLibrary dtors close the (non-detached) libraries
        }

    private:
        struct Entry
        {
            IRuntimePlugin* plugin;     // a game's native module, or
            const PluginPart* part;     // a package's part,
            PluginContext* context;     // with the context its load and unload are handed
            core::DynamicLibrary library;
            // Everything OnLoad registered, recorded by the ambient observers below and
            // reversed on unload, so the plugin never hand-mirrors its registrations.
            core::Array<core::TypeId> registeredTypes;
            core::Array<core::TypeId> registeredSerializables;
            core::Array<core::Array<core::TypeId>> recorded; // one list per extra recorder
        };

        // The RegistrationScope: arm ambient observers on the global registries for the
        // duration of OnLoad. Observers fire only on real inserts, so a type another
        // party already owns is never recorded (and never torn down by this plugin).
        void RecordedOnLoad(Entry& entry)
        {
            struct Capture
            {
                core::Array<core::TypeId>* types;
                core::Array<core::TypeId>* serializables;
            } capture{&entry.registeredTypes, &entry.registeredSerializables};
            core::GlobalTypeRegistry().SetRegistrationObserver(
                [](void* ctx, core::TypeId id)
                { static_cast<Capture*>(ctx)->types->PushBack(id); },
                &capture);
            core::GlobalSerializableRegistry().SetRegistrationObserver(
                [](void* ctx, core::TypeId id)
                { static_cast<Capture*>(ctx)->serializables->PushBack(id); },
                &capture);
            entry.recorded.Clear();
            for (core::usize i = 0; i < m_recorders.Size(); ++i)
            {
                entry.recorded.PushBack(core::Array<core::TypeId>{});
            }
            for (core::usize i = 0; i < m_recorders.Size(); ++i)
            {
                m_recorders[i]->Arm(entry.recorded[i]);
            }
            if (entry.plugin != nullptr)
            {
                entry.plugin->OnLoad(*m_subsystems);
            }
            else
            {
                entry.part->load(*entry.context);
            }
            for (core::usize i = m_recorders.Size(); i-- > 0;)
            {
                m_recorders[i]->Disarm();
            }
            core::GlobalTypeRegistry().SetRegistrationObserver(nullptr, nullptr);
            core::GlobalSerializableRegistry().SetRegistrationObserver(nullptr, nullptr);
        }

        void ReverseRecorded(Entry& entry)
        {
            // Extra recorders first (a scene manager leaves live scenes before the types it
            // depends on vanish), each in reverse order.
            for (core::usize r = entry.recorded.Size(); r-- > 0;)
            {
                if (r < m_recorders.Size())
                {
                    for (core::usize i = entry.recorded[r].Size(); i-- > 0;)
                    {
                        m_recorders[r]->Reverse(entry.recorded[r][i]);
                    }
                }
                entry.recorded[r].Clear();
            }
            for (core::usize i = entry.registeredSerializables.Size(); i-- > 0;)
            {
                core::GlobalSerializableRegistry().Unregister(entry.registeredSerializables[i]);
            }
            for (core::usize i = entry.registeredTypes.Size(); i-- > 0;)
            {
                core::GlobalTypeRegistry().Unregister(entry.registeredTypes[i]);
            }
            entry.registeredTypes.Clear();
            entry.registeredSerializables.Clear();
        }

        SubsystemManager* m_subsystems;
        core::Array<Entry> m_entries;
        core::Array<IRegistrationRecorder*> m_recorders; // borrowed
    };
}
