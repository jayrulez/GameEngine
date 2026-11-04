// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin_loader partition
//
// PluginLoader: loads plugin parts, those linked into the binary (AddPart) and those in shared
// libraries (LoadPart, the loader owning the library and closing it on teardown), records what
// each part registers and reverses it on unload. A part whose load offers a type under an id
// another type holds (another plugin's, or the engine's) is refused: its load is undone at once and
// the refusal names both types and the holder. PluginHost, which decides what a project loads,
// loads through it.

module;
#include "Core/Prelude.h"

export module foundation.runtime:plugin_loader;

import foundation.core;
import :plugin_part;

namespace core = foundation::core;

export namespace foundation::runtime
{
    // A registry the RegistrationScope can record: armed around a part's load, it
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
        PluginLoader() noexcept = default;

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

        // Loads a part linked into this binary: calls its load with `context` (borrowed; it must
        // outlive the part's unload) under the recording. The error says why it was refused (it
        // offered a type under an id another holds), its load undone.
        core::Result<const PluginPart*, core::String> AddPart(const PluginPart& part, PluginContext& context)
        {
            Entry entry{&part, &context, core::DynamicLibrary{}, {}, {}, {}};
            return Admit(core::Move(entry));
        }

        // Loads a part from a shared library: resolves `symbol` (PluginPartSymbol), takes the part
        // it returns and loads it as AddPart does. The library is closed when the loader unloads.
        // The error says why: the library or its entry is missing, the entry gave no part, or the
        // part was refused as AddPart's is.
        core::Result<const PluginPart*, core::String> LoadPart(core::StringView path, core::StringView symbol,
                                                               PluginContext& context)
        {
            core::DynamicLibrary library;
            if (core::Status status = library.Load(path); !status)
            {
                return core::Err(core::Format(u8"did not load from '{}'", path));
            }
            const auto entryPoint = library.GetSymbol<PluginPartEntryFn>(symbol);
            if (entryPoint == nullptr)
            {
                return core::Err(core::Format(u8"did not load from '{}' (no entry {})", path, symbol));
            }
            const PluginPart* part = entryPoint();
            if (part == nullptr || part->load == nullptr)
            {
                return core::Err(core::Format(u8"did not load from '{}' (its entry gave no part)", path));
            }
            Entry entry{part, &context, core::Move(library), {}, {}, {}};
            return Admit(core::Move(entry));
        }

        [[nodiscard]] core::usize Count() const noexcept { return m_entries.Size(); }

        // The plugin whose parts' loads registered type `id` (the type registry's record), or empty
        // for a type no loaded part registered (the engine's).
        [[nodiscard]] core::StringView OwnerOf(core::TypeId id) const noexcept
        {
            for (const Entry& entry : m_entries)
            {
                for (core::TypeId registered : entry.registeredTypes)
                {
                    if (registered == id)
                    {
                        return entry.context->PluginId();
                    }
                }
            }
            return {};
        }

        // Appends to `out` every type the parts of plugin `pluginId` registered.
        void TypesOf(core::StringView pluginId, core::Array<core::TypeId>& out) const
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.context->PluginId() != pluginId)
                {
                    continue;
                }
                for (core::TypeId registered : entry.registeredTypes)
                {
                    out.PushBack(registered);
                }
            }
        }

        // Unloads everything in reverse load order: each part's unload, then the reversal of what
        // it registered (a registry entry or factory pointing into a closed library would be a
        // dangling read), then the libraries close.
        // closeLibraries=false is the hot-reload mode: the old modules stay mapped for
        // the process lifetime (leaked on purpose, so no page is freed under a pointer the
        // teardown missed) and the rebuilt module loads from a fresh versioned copy.
        void UnloadAll(bool closeLibraries = true) { UnloadFrom(0, closeLibraries); }

        // Unloads the parts loaded at or after position `first` (Count() before them), as
        // UnloadAll does, keeping those before: a plugin refused partway through its parts leaves
        // nothing behind.
        void UnloadFrom(core::usize first, bool closeLibraries = true)
        {
            while (m_entries.Size() > first)
            {
                Entry& entry = m_entries.Back();
                if (entry.part->unload != nullptr)
                {
                    entry.part->unload(*entry.context);
                }
                ReverseRecorded(entry);
                if (!closeLibraries)
                {
                    entry.library.Detach();
                }
                m_entries.PopBack(); // the DynamicLibrary dtor closes a non-detached library
            }
        }

    private:
        struct Entry
        {
            const PluginPart* part;
            PluginContext* context; // what its load and unload are handed
            core::DynamicLibrary library;
            // Everything its load registered, recorded by the ambient observers below and
            // reversed on unload, so the part never hand-mirrors its registrations.
            core::Array<core::TypeId> registeredTypes;
            core::Array<core::TypeId> registeredSerializables;
            core::Array<core::Array<core::TypeId>> recorded; // one list per extra recorder
        };

        // A type a part's load offered under an id another type held.
        struct Collision
        {
            const core::TypeInfo* held;
            const core::TypeInfo* offered;
            bool serializable; // the serializable registry's (else the type registry's)
        };

        // Loads the entry's part under the recording and keeps it, or, when its load offered a
        // type under a held id, undoes the load and answers why.
        core::Result<const PluginPart*, core::String> Admit(Entry entry)
        {
            core::Array<Collision> collisions;
            RecordedOnLoad(entry, collisions);
            if (!collisions.IsEmpty())
            {
                const core::String why = DescribeCollision(entry, collisions[0]);
                if (entry.part->unload != nullptr)
                {
                    entry.part->unload(*entry.context);
                }
                ReverseRecorded(entry);
                return core::Err(why);
            }
            const PluginPart* part = entry.part;
            m_entries.PushBack(core::Move(entry));
            return part;
        }

        // "registers type '<offered>', whose id <holder>'s type '<held>' holds": the holder is the
        // plugin whose load put the held type in that registry, the part itself, or the engine.
        [[nodiscard]] core::String DescribeCollision(const Entry& loading, const Collision& collision) const
        {
            const core::TypeId id = collision.held->id;
            const auto recorded = [&collision, id](const Entry& entry)
            {
                const core::Array<core::TypeId>& ids =
                    collision.serializable ? entry.registeredSerializables : entry.registeredTypes;
                for (core::TypeId held : ids)
                {
                    if (held == id)
                    {
                        return true;
                    }
                }
                return false;
            };
            core::String holder(u8"the engine's");
            if (recorded(loading))
            {
                holder = core::String(u8"its own");
            }
            else
            {
                for (const Entry& entry : m_entries)
                {
                    if (recorded(entry))
                    {
                        holder = core::Format(u8"plugin '{}''s", entry.context->PluginId());
                        break;
                    }
                }
            }
            const auto qualified = [](const core::TypeInfo& type)
            {
                return core::Format(u8"{}::{}", core::StringView(reinterpret_cast<const char8_t*>(type.namespaceName)),
                                    core::StringView(reinterpret_cast<const char8_t*>(type.name)));
            };
            return core::Format(u8"registers type '{}', whose id {} type '{}' holds", qualified(*collision.offered),
                                holder, qualified(*collision.held));
        }

        // The RegistrationScope: arm ambient observers on the global registries for the
        // duration of the part's load. Observers fire only on real inserts, so a type another
        // party already owns is never recorded (and never torn down by this plugin); a different
        // type offered under a held id goes to `collisions`.
        void RecordedOnLoad(Entry& entry, core::Array<Collision>& collisions)
        {
            struct Capture
            {
                core::Array<core::TypeId>* types;
                core::Array<core::TypeId>* serializables;
                core::Array<Collision>* collisions;
            } capture{&entry.registeredTypes, &entry.registeredSerializables, &collisions};
            core::GlobalTypeRegistry().SetRegistrationObserver(
                [](void* ctx, core::TypeId id)
                { static_cast<Capture*>(ctx)->types->PushBack(id); },
                &capture);
            core::GlobalSerializableRegistry().SetRegistrationObserver(
                [](void* ctx, core::TypeId id)
                { static_cast<Capture*>(ctx)->serializables->PushBack(id); },
                &capture);
            core::GlobalTypeRegistry().SetCollisionObserver(
                [](void* ctx, const core::TypeInfo& held, const core::TypeInfo& offered)
                { static_cast<Capture*>(ctx)->collisions->PushBack(Collision{&held, &offered, false}); },
                &capture);
            core::GlobalSerializableRegistry().SetCollisionObserver(
                [](void* ctx, const core::TypeInfo& held, const core::TypeInfo& offered)
                { static_cast<Capture*>(ctx)->collisions->PushBack(Collision{&held, &offered, true}); },
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
            entry.part->load(*entry.context);
            for (core::usize i = m_recorders.Size(); i-- > 0;)
            {
                m_recorders[i]->Disarm();
            }
            core::GlobalTypeRegistry().SetRegistrationObserver(nullptr, nullptr);
            core::GlobalSerializableRegistry().SetRegistrationObserver(nullptr, nullptr);
            core::GlobalTypeRegistry().SetCollisionObserver(nullptr, nullptr);
            core::GlobalSerializableRegistry().SetCollisionObserver(nullptr, nullptr);
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

        core::Array<Entry> m_entries;
        core::Array<IRegistrationRecorder*> m_recorders; // borrowed
    };
}
