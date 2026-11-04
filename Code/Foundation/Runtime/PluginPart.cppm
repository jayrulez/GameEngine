// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin_part partition.
//
// A plugin part's contract. Each part of a package has
// exactly one entry point, an extern "C" function named from the manifest,
// `PluginPart_<id>_<Kind>`, returning a constant PluginPart: the load and unload the part
// performs, nothing else. The record is constant-initialised, so a part linked into a binary
// but not enabled does no work at all, static initialisation included.
//
// The host role says which parts a process loads: a player has no use for an asset importer,
// and a cook has no window to put an editor page in.

module;
#include "Core/Prelude.h"

export module foundation.runtime:plugin_part;

import foundation.core;
import :subsystem_manager;
import :plugin_manifest;
import :enabled_plugin;

namespace core = foundation::core;

export namespace foundation::runtime
{
    // The kind of process loading plugins.
    enum class HostRole : core::u8
    {
        Player,
        Editor,
        Cook,
        Export,
        Mcp,
    };

    inline constexpr HostRole kHostRoles[] = {HostRole::Player, HostRole::Editor, HostRole::Cook, HostRole::Export,
                                              HostRole::Mcp};

    [[nodiscard]] constexpr core::StringView HostRoleName(HostRole role) noexcept
    {
        switch (role)
        {
        case HostRole::Player: return u8"player";
        case HostRole::Editor: return u8"editor";
        case HostRole::Cook: return u8"cook";
        case HostRole::Export: return u8"export";
        case HostRole::Mcp: return u8"mcp";
        }
        return u8"";
    }

    // Whether a host of `role` loads parts of `kind`. The build's copy of this table
    // (cmake/Plugins.cmake, UTIL_PLUGIN_ROLE_<role>_KINDS) is pinned to this one by Runtime.Tests.
    [[nodiscard]] constexpr bool RoleLoads(HostRole role, PluginPartKind kind) noexcept
    {
        switch (kind)
        {
        case PluginPartKind::Engine: return true;
        case PluginPartKind::Pipeline: return role != HostRole::Player;
        case PluginPartKind::Tooling:
            return role == HostRole::Editor || role == HostRole::Export || role == HostRole::Mcp;
        case PluginPartKind::Editor: return role == HostRole::Editor;
        }
        return false;
    }

    // A service a host hands its plugins, keyed by its type: the layer that owns the service
    // provides it (PluginHost::Provide), so this layer never names the types above it.
    struct PluginService
    {
        core::TypeId type;
        void* object;
    };

    // What a part's load and unload are handed: the host's services, as far as its role has them,
    // and the project's settings for the plugin. One context per plugin, shared by its parts, alive
    // from the plugin's load to its unload.
    class PluginContext
    {
    public:
        PluginContext(HostRole role, SubsystemManager& subsystems, core::StringView pluginId,
                      core::Span<const PluginService> services, core::Span<const core::u8> settings,
                      core::IAllocator& allocator)
            : m_role(role), m_subsystems(&subsystems), m_pluginId(pluginId, allocator), m_services(services),
              m_settingsXml(allocator), m_allocator(&allocator)
        {
            m_settingsXml.Resize(settings.Size());
            if (!settings.IsEmpty())
            {
                core::MemCopy(m_settingsXml.Data(), settings.Data(), settings.Size());
            }
        }

        PluginContext(const PluginContext&) = delete;
        PluginContext& operator=(const PluginContext&) = delete;

        [[nodiscard]] HostRole Role() const noexcept { return m_role; }
        [[nodiscard]] SubsystemManager& Subsystems() noexcept { return *m_subsystems; }
        [[nodiscard]] core::StringView PluginId() const noexcept { return m_pluginId.AsView(); }
        [[nodiscard]] core::IAllocator& Allocator() noexcept { return *m_allocator; }

        // The process-wide registries a part registers its types into. The host records what a
        // load registers in them and reverses it on unload, so a part never mirrors its
        // registrations by hand.
        [[nodiscard]] core::TypeRegistry& Types() noexcept { return core::GlobalTypeRegistry(); }
        [[nodiscard]] core::SerializableRegistry& Serializables() noexcept { return core::GlobalSerializableRegistry(); }

        // The host's service of type T, or null when this host has none (a cook has no window, so
        // no application host): a part checks, and leaves out what the host cannot support.
        template <typename T>
        [[nodiscard]] T* Find() const noexcept
        {
            const core::TypeId type = core::TypeOf<T>().id;
            for (const PluginService& service : m_services)
            {
                if (service.type == type)
                {
                    return static_cast<T*>(service.object);
                }
            }
            return nullptr;
        }

        // The plugin's settings, read from the project's entry into T, its reflected settings type,
        // on the first call (T's defaults when the entry has none); every part asking for them shares
        // one object, alive until the plugin unloads. A plugin has one settings type: null when an
        // earlier call named another. When the entry's settings do not read as a T, the defaults are
        // used and SettingsRead says so, so an editor never writes them over what it could not read.
        template <typename T>
            requires(__is_base_of(core::ISerializable, T))
        [[nodiscard]] T* Settings()
        {
            if (m_settings)
            {
                return m_settings->GetType() == &T::StaticType() ? static_cast<T*>(m_settings.Get()) : nullptr;
            }
            core::UniquePtr<T> settings = core::MakeUnique<T>(*m_allocator);
            core::ISerializable& object = *settings;
            m_settings = core::Move(settings);
            const core::Status read = ReadPluginSettings(
                core::Span<const core::u8>(m_settingsXml.Data(), m_settingsXml.Size()), object, *m_allocator);
            if (!read.IsOk())
            {
                m_settingsRead = false;
                m_settings = core::MakeUnique<T>(*m_allocator); // the defaults, not what the reader left
            }
            return static_cast<T*>(m_settings.Get());
        }

        // The settings object a part asked for (Settings<T>), or null when none did.
        [[nodiscard]] core::ISerializable* SettingsObject() const noexcept { return m_settings.Get(); }

        // False when the project's settings did not read into the plugin's type.
        [[nodiscard]] bool SettingsRead() const noexcept { return m_settingsRead; }

    private:
        HostRole m_role;
        SubsystemManager* m_subsystems;
        core::String m_pluginId;
        core::Span<const PluginService> m_services; // the host's, which outlives every context
        core::Array<core::u8> m_settingsXml;        // the project entry's, read by Settings<T>
        core::UniquePtr<core::ISerializable> m_settings;
        bool m_settingsRead = true;
        core::IAllocator* m_allocator;
    };

    using PluginPartFn = void (*)(PluginContext& context);

    // A part, as its entry point returns it. `load` performs every registration explicitly;
    // `unload` (may be null) removes what the recording cannot reverse by itself, such as a
    // subsystem it registered.
    struct PluginPart
    {
        const char8_t* pluginId; // the manifest's id, checked against it on load
        PluginPartKind kind;
        PluginPartFn load;
        PluginPartFn unload;
    };

    using PluginPartEntryFn = const PluginPart* (*)();

    // The entry symbol of a part: `PluginPart_<id>_<Kind>`.
    [[nodiscard]] inline core::String PluginPartSymbol(core::StringView pluginId, PluginPartKind kind,
                                                       core::IAllocator& allocator)
    {
        core::String symbol(u8"PluginPart_", allocator);
        symbol.Append(pluginId);
        symbol.Append(u8"_");
        symbol.Append(PluginPartKindName(kind));
        return symbol;
    }

    // A plugin compiled into this binary, as the generated registry lists it: the manifest's
    // text (so a player with no package files on disk still checks versions, dependencies and
    // slots) and the entry points of the parts linked here.
    struct LinkedPlugin
    {
        const char8_t* manifestXml;
        const PluginPartEntryFn* parts;
        core::usize partCount;
    };
}

// The generated registry of a binary that links plugins (util_link_plugins, cmake/Plugins.cmake)
// defines this: the plugins compiled in, and their count in `count`. Only such a binary may call
// it; it has C linkage so the generated, non-module source can define what this module declares.
export extern "C" const foundation::runtime::LinkedPlugin* LinkedPlugins(foundation::core::usize* count);

export namespace foundation::runtime
{
    // The plugins this binary links, as LinkedPlugins lists them.
    [[nodiscard]] inline core::Span<const LinkedPlugin> LinkedPluginList()
    {
        core::usize count = 0;
        const LinkedPlugin* plugins = LinkedPlugins(&count);
        return core::Span<const LinkedPlugin>(plugins, count);
    }
}
