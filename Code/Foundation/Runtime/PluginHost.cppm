// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin_host partition.
//
// PluginHost owns the enabled plugins of one process. It knows
// the plugins it could load (those linked into the binary, from the generated registry, and
// packages built as shared libraries for development), and given the ids a project enables it:
//
//   1. resolves each id, the linked copy first;
//   2. checks each manifest, the dependencies (enabled, loadable, recent enough) and the
//      `provides` slots (one provider each), refusing with a message, never crashing;
//   3. loads the parts the host's role takes, in dependency order, through the PluginLoader's
//      recording, and unloads in reverse.
//
// A plugin that is linked but not enabled is never touched: dormant costs nothing.

module;
#include "Core/Prelude.h"

export module foundation.runtime:plugin_host;

import foundation.core;
import :subsystem_manager;
import :plugin_manifest;
import :plugin_part;
import :plugin_loader;

namespace core = foundation::core;

export namespace foundation::runtime
{
    // A part of a development package, built as a shared library.
    struct PluginLibrary
    {
        PluginPartKind kind;
        core::String path;
    };

    struct PluginRefusal
    {
        core::String id;
        core::String reason;
    };

    // What one Load did: the plugins loaded, in load order, and the ones refused, with why.
    struct PluginLoadReport
    {
        core::Array<core::String> loaded;
        core::Array<PluginRefusal> refused;

        [[nodiscard]] const PluginRefusal* FindRefusal(core::StringView id) const noexcept
        {
            for (const PluginRefusal& refusal : refused)
            {
                if (refusal.id.AsView() == id)
                {
                    return &refusal;
                }
            }
            return nullptr;
        }
    };

    class PluginHost
    {
    public:
        PluginHost(core::IAllocator& allocator, SubsystemManager& subsystems, HostRole role,
                   core::StringView engineVersion)
            : m_allocator(&allocator), m_subsystems(&subsystems), m_role(role),
              m_engineVersion(engineVersion, allocator), m_loader(subsystems), m_candidates(allocator),
              m_contexts(allocator), m_loaded(allocator)
        {
        }

        ~PluginHost() { Unload(); }

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        [[nodiscard]] HostRole Role() const noexcept { return m_role; }

        // The loader the parts load through, for adding recorders before Load.
        [[nodiscard]] PluginLoader& Loader() noexcept { return m_loader; }

        // The plugins linked into this binary (the generated registry's list). A manifest that
        // does not read is a build defect: Internal, and nothing from the list is added.
        [[nodiscard]] core::Status AddLinked(core::Span<const LinkedPlugin> linked)
        {
            core::Array<Candidate> read(*m_allocator);
            for (const LinkedPlugin& plugin : linked)
            {
                Candidate candidate;
                candidate.manifest = core::MakeUnique<PluginManifest>(*m_allocator);
                const core::StringView xml{plugin.manifestXml};
                core::MemoryStream stream(*m_allocator);
                (void)stream.Write(xml.Data(), xml.Size());
                (void)stream.Seek(0, core::SeekOrigin::Begin);
                if (!ReadPluginManifest(stream, *candidate.manifest).IsOk())
                {
                    return core::Status{core::ErrorCode::Internal};
                }
                for (core::usize i = 0; i < plugin.partCount; ++i)
                {
                    candidate.linked.PushBack(plugin.parts[i]);
                }
                read.PushBack(core::Move(candidate));
            }
            for (Candidate& candidate : read)
            {
                Remember(core::Move(candidate));
            }
            return core::Status{};
        }

        // A development package whose parts are shared libraries. A linked plugin of the same id
        // wins over it.
        void AddShared(core::UniquePtr<PluginManifest> manifest, core::Span<const PluginLibrary> libraries)
        {
            if (!manifest)
            {
                return;
            }
            Candidate candidate;
            candidate.manifest = core::Move(manifest);
            for (const PluginLibrary& library : libraries)
            {
                candidate.libraries.PushBack(PluginLibrary{library.kind, core::String(library.path.AsView(), *m_allocator)});
            }
            Remember(core::Move(candidate));
        }

        // Whether a plugin of this id is known (linked or added as a package).
        [[nodiscard]] bool Knows(core::StringView id) const noexcept { return Find(id) != nullptr; }

        [[nodiscard]] bool IsLoaded(core::StringView id) const noexcept
        {
            for (const core::String& loaded : m_loaded)
            {
                if (loaded.AsView() == id)
                {
                    return true;
                }
            }
            return false;
        }

        // Loads the plugins `enabled` names (ids, in the project's order). Call once; Unload first
        // to load another set.
        PluginLoadReport Load(core::Span<const core::StringView> enabled)
        {
            PluginLoadReport report;
            core::Array<const Candidate*> accepted(*m_allocator);
            const auto refuse = [&report, this](core::StringView id, core::String reason)
            {
                if (report.FindRefusal(id) == nullptr)
                {
                    report.refused.PushBack(PluginRefusal{core::String(id, *m_allocator), core::Move(reason)});
                }
            };
            const auto isAccepted = [&accepted](core::StringView id) -> const Candidate*
            {
                for (const Candidate* candidate : accepted)
                {
                    if (candidate->manifest->id.AsView() == id)
                    {
                        return candidate;
                    }
                }
                return nullptr;
            };

            // 1. Resolve and check each manifest; then the slots, first come first served.
            for (core::StringView id : enabled)
            {
                if (isAccepted(id) != nullptr || report.FindRefusal(id) != nullptr)
                {
                    continue; // listed twice
                }
                const Candidate* candidate = Find(id);
                if (candidate == nullptr)
                {
                    refuse(id, core::Format(u8"plugin '{}' is not in this build", id));
                    continue;
                }
                core::String whyNot;
                if (!CheckPluginManifest(*candidate->manifest, m_engineVersion.AsView(), whyNot))
                {
                    refuse(id, core::Move(whyNot));
                    continue;
                }
                if (core::String missing = MissingPart(*candidate); !missing.IsEmpty())
                {
                    refuse(id, core::Move(missing));
                    continue;
                }
                const core::String* taken = nullptr;
                core::StringView slot;
                for (const core::String& provided : candidate->manifest->provides)
                {
                    for (const Candidate* other : accepted)
                    {
                        for (const core::String& otherSlot : other->manifest->provides)
                        {
                            if (otherSlot == provided)
                            {
                                taken = &other->manifest->id;
                                slot = provided.AsView();
                            }
                        }
                    }
                }
                if (taken != nullptr)
                {
                    refuse(id, core::Format(u8"plugin '{}' provides '{}', which '{}' already provides", id, slot, *taken));
                    continue;
                }
                accepted.PushBack(candidate);
            }

            // 2. Dependencies, to a fixed point: refusing one plugin may strand its dependents.
            for (bool changed = true; changed;)
            {
                changed = false;
                for (core::usize i = 0; i < accepted.Size(); ++i)
                {
                    const PluginManifest& manifest = *accepted[i]->manifest;
                    core::String why = UnmetDependency(manifest, isAccepted, report);
                    if (!why.IsEmpty())
                    {
                        refuse(manifest.id.AsView(), core::Move(why));
                        accepted.RemoveAt(i);
                        changed = true;
                        break;
                    }
                }
            }

            // 3. Dependency order (depth first, dependencies before dependents); a cycle is refused.
            core::Array<const Candidate*> order(*m_allocator);
            core::Array<const Candidate*> visiting(*m_allocator);
            core::Array<core::String> cyclic(*m_allocator);
            for (const Candidate* candidate : accepted)
            {
                Visit(candidate, isAccepted, order, visiting, cyclic);
            }

            // 4. Load each plugin's parts for this role, a plugin whose dependency failed skipped.
            for (const Candidate* candidate : order)
            {
                const PluginManifest& manifest = *candidate->manifest;
                if (Contains(cyclic, manifest.id.AsView()))
                {
                    refuse(manifest.id.AsView(),
                           core::Format(u8"plugin '{}' is in a dependency cycle", manifest.id));
                    continue;
                }
                const PluginDependency* failed = nullptr;
                for (const PluginDependency& dependency : manifest.dependsOn)
                {
                    if (!IsLoaded(dependency.id.AsView()))
                    {
                        failed = &dependency;
                        break;
                    }
                }
                if (failed != nullptr)
                {
                    refuse(manifest.id.AsView(), core::Format(u8"plugin '{}' needs '{}', which did not load",
                                                              manifest.id, failed->id));
                    continue;
                }
                core::String why = LoadParts(*candidate);
                if (!why.IsEmpty())
                {
                    refuse(manifest.id.AsView(), core::Move(why));
                    continue;
                }
                m_loaded.PushBack(core::String(manifest.id.AsView(), *m_allocator));
                report.loaded.PushBack(core::String(manifest.id.AsView(), *m_allocator));
            }
            return report;
        }

        // Unloads every loaded part in reverse load order, reversing what each registered.
        void Unload()
        {
            m_loader.UnloadAll();
            m_contexts.Clear();
            m_loaded.Clear();
        }

    private:
        struct Candidate
        {
            core::UniquePtr<PluginManifest> manifest;
            core::Array<PluginPartEntryFn> linked;
            core::Array<PluginLibrary> libraries;
        };

        void Remember(Candidate candidate)
        {
            for (Candidate& known : m_candidates)
            {
                if (known.manifest->id == candidate.manifest->id)
                {
                    if (known.linked.IsEmpty() && !candidate.linked.IsEmpty())
                    {
                        known = core::Move(candidate); // the linked copy wins
                    }
                    return;
                }
            }
            m_candidates.PushBack(core::Move(candidate));
        }

        [[nodiscard]] const Candidate* Find(core::StringView id) const noexcept
        {
            for (const Candidate& candidate : m_candidates)
            {
                if (candidate.manifest->id.AsView() == id)
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static bool Contains(const core::Array<core::String>& ids, core::StringView id) noexcept
        {
            for (const core::String& entry : ids)
            {
                if (entry.AsView() == id)
                {
                    return true;
                }
            }
            return false;
        }

        // The linked entry of `kind`, or null.
        [[nodiscard]] static const PluginPart* LinkedPart(const Candidate& candidate, PluginPartKind kind)
        {
            for (PluginPartEntryFn entry : candidate.linked)
            {
                const PluginPart* part = entry != nullptr ? entry() : nullptr;
                if (part != nullptr && part->kind == kind)
                {
                    return part;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static const PluginLibrary* SharedPart(const Candidate& candidate, PluginPartKind kind)
        {
            for (const PluginLibrary& library : candidate.libraries)
            {
                if (library.kind == kind)
                {
                    return &library;
                }
            }
            return nullptr;
        }

        // Empty when every part the manifest lists for this role can be reached; else why not.
        [[nodiscard]] core::String MissingPart(const Candidate& candidate) const
        {
            for (PluginPartKind kind : kPluginPartKinds)
            {
                if (!RoleLoads(m_role, kind) || !candidate.manifest->HasPart(kind))
                {
                    continue;
                }
                if (LinkedPart(candidate, kind) == nullptr && SharedPart(candidate, kind) == nullptr)
                {
                    return core::Format(u8"plugin '{}': its {} part is not in this build", candidate.manifest->id,
                                        PluginPartKindName(kind));
                }
            }
            return {};
        }

        template <typename IsAccepted>
        [[nodiscard]] core::String UnmetDependency(const PluginManifest& manifest, const IsAccepted& isAccepted,
                                                   const PluginLoadReport& report) const
        {
            for (const PluginDependency& dependency : manifest.dependsOn)
            {
                const Candidate* found = isAccepted(dependency.id.AsView());
                if (found == nullptr)
                {
                    const PluginRefusal* refusal = report.FindRefusal(dependency.id.AsView());
                    return refusal != nullptr
                               ? core::Format(u8"plugin '{}' needs '{}', which was refused", manifest.id, dependency.id)
                               : core::Format(u8"plugin '{}' needs '{}', which is not enabled", manifest.id,
                                              dependency.id);
                }
                const core::Optional<PluginVersion> have = ParsePluginVersion(found->manifest->version.AsView());
                const core::Optional<PluginVersion> want = ParsePluginVersion(dependency.version.AsView());
                if (!have || !want || !VersionAtLeast(*have, *want))
                {
                    return core::Format(u8"plugin '{}' needs '{}' {} or later; it is {}", manifest.id, dependency.id,
                                        dependency.version, found->manifest->version);
                }
            }
            return {};
        }

        template <typename IsAccepted>
        void Visit(const Candidate* candidate, const IsAccepted& isAccepted, core::Array<const Candidate*>& order,
                   core::Array<const Candidate*>& visiting, core::Array<core::String>& cyclic)
        {
            for (const Candidate* done : order)
            {
                if (done == candidate)
                {
                    return;
                }
            }
            for (const Candidate* open : visiting)
            {
                if (open == candidate)
                {
                    // Back on a plugin still being visited: everything from it to here is a cycle.
                    bool inCycle = false;
                    for (const Candidate* member : visiting)
                    {
                        inCycle = inCycle || member == candidate;
                        if (inCycle && !Contains(cyclic, member->manifest->id.AsView()))
                        {
                            cyclic.PushBack(core::String(member->manifest->id.AsView(), *m_allocator));
                        }
                    }
                    return;
                }
            }
            visiting.PushBack(candidate);
            for (const PluginDependency& dependency : candidate->manifest->dependsOn)
            {
                if (const Candidate* next = isAccepted(dependency.id.AsView()); next != nullptr)
                {
                    Visit(next, isAccepted, order, visiting, cyclic);
                }
            }
            visiting.PopBack();
            order.PushBack(candidate);
        }

        // Loads the candidate's parts for this role in kind order; empty on success.
        [[nodiscard]] core::String LoadParts(const Candidate& candidate)
        {
            const PluginManifest& manifest = *candidate.manifest;
            m_contexts.PushBack(core::MakeUnique<PluginContext>(*m_allocator, m_role, *m_subsystems,
                                                                manifest.id.AsView(), *m_allocator));
            PluginContext& context = *m_contexts.Back();
            // Every linked entry is checked before any part loads, so a plugin refused here leaves
            // nothing behind. (A shared library can only be checked by loading it.)
            for (PluginPartKind kind : kPluginPartKinds)
            {
                if (!RoleLoads(m_role, kind) || !manifest.HasPart(kind))
                {
                    continue;
                }
                const PluginPart* part = LinkedPart(candidate, kind);
                if (part != nullptr && (core::StringView(part->pluginId) != manifest.id.AsView() || part->load == nullptr))
                {
                    m_contexts.PopBack();
                    return core::Format(u8"plugin '{}': its {} part's entry is not this plugin's", manifest.id,
                                        PluginPartKindName(kind));
                }
            }
            for (PluginPartKind kind : kPluginPartKinds)
            {
                if (!RoleLoads(m_role, kind) || !manifest.HasPart(kind))
                {
                    continue;
                }
                if (const PluginPart* part = LinkedPart(candidate, kind); part != nullptr)
                {
                    m_loader.AddPart(*part, context);
                    continue;
                }
                const PluginLibrary* library = SharedPart(candidate, kind);
                const core::String symbol = PluginPartSymbol(manifest.id.AsView(), kind, *m_allocator);
                core::Result<const PluginPart*> loaded = m_loader.LoadPart(library->path.AsView(), symbol.AsView(), context);
                if (!loaded.HasValue())
                {
                    return core::Format(u8"plugin '{}': its {} part did not load from '{}'", manifest.id,
                                        PluginPartKindName(kind), library->path);
                }
            }
            return {};
        }

        core::IAllocator* m_allocator;
        SubsystemManager* m_subsystems;
        HostRole m_role;
        core::String m_engineVersion;
        PluginLoader m_loader;
        core::Array<Candidate> m_candidates;
        core::Array<core::UniquePtr<PluginContext>> m_contexts; // one per loaded plugin
        core::Array<core::String> m_loaded;
    };
}
