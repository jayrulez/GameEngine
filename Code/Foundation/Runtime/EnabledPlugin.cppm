// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :enabled_plugin partition.
//
// A project's entry for one plugin it enables: which plugin, where a project-local package
// is, the platforms it is enabled on, and its settings. The settings stay the XML they were read
// from, in every host and whether or not the plugin is there to read them: the project is read
// before any plugin loads, and an entry whose plugin is missing must be written back unchanged.
// A plugin's part reads them into its own reflected type when it loads (PluginContext::Settings),
// and an editor writes the type back into the entry (WritePluginSettings).

module;
#include "Core/Prelude.h"

export module foundation.runtime:enabled_plugin;

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import :plugin_manifest;

using namespace foundation::core;

export namespace foundation::runtime
{
    struct EnabledPlugin
    {
        String id;              // the plugin's manifest id
        String path;            // a project-local package, relative to the project; empty for the engine's
        Array<String> platforms; // the platforms it is enabled on; empty is every one its manifest supports
        Array<u8> settings;     // its settings payload, as XML elements (empty: the type's defaults)

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "id", id);
            foundation::core::Serialize(ar, "path", path);
            foundation::core::Serialize(ar, "platforms", platforms);
            // Carried verbatim: the XML's remaining elements, and through the binary serializer
            // (a copy) the same bytes inside a framed region.
            ar.Key("settings");
            ar.BeginObject();
            ar.BeginFramedRegion();
            (void)ar.RawRemainder(settings);
            ar.EndFramedRegion();
            ar.EndObject();
        }
    };

    // ADL hook so Serialize(ar, Array<EnabledPlugin>&) frames each entry as its own object node.
    inline void Serialize(ISerializer& ar, EnabledPlugin& value)
    {
        ar.BeginObject();
        value.Serialize(ar);
        ar.EndObject();
    }

    // Whether a plugin is enabled on a platform.
    enum class PluginEnablement : u8
    {
        Enabled,
        NotEnabled,  // the entry leaves it out, or names no platforms and the manifest lacks it
        Unsupported, // the entry names the platform, and the manifest does not support it
    };

    [[nodiscard]] inline PluginEnablement EnablementOn(const EnabledPlugin& entry, const PluginManifest& manifest,
                                                       StringView platform) noexcept
    {
        const bool supported = manifest.FindPlatform(platform) != nullptr;
        if (entry.platforms.IsEmpty())
        {
            return supported ? PluginEnablement::Enabled : PluginEnablement::NotEnabled;
        }
        for (const String& listed : entry.platforms)
        {
            if (listed.AsView() == platform)
            {
                return supported ? PluginEnablement::Enabled : PluginEnablement::Unsupported;
            }
        }
        return PluginEnablement::NotEnabled;
    }

    // The platforms an entry names that its manifest does not support, which the project reports.
    [[nodiscard]] inline Array<String> UnsupportedPlatforms(const EnabledPlugin& entry, const PluginManifest& manifest,
                                                            IAllocator& allocator)
    {
        Array<String> unsupported(allocator);
        for (const String& listed : entry.platforms)
        {
            if (manifest.FindPlatform(listed.AsView()) == nullptr)
            {
                unsupported.PushBack(String(listed.AsView(), allocator));
            }
        }
        return unsupported;
    }

    // Reads an entry's settings into `into`, a versioned payload of into's type. Empty settings
    // leave its defaults; a payload that does not read (another type, a stale data version, bad
    // XML) fails and leaves `into` as the reader left it.
    [[nodiscard]] inline Status ReadPluginSettings(Span<const u8> settings, ISerializable& into, IAllocator& allocator)
    {
        if (settings.IsEmpty())
        {
            return Status{};
        }
        String wrapped(u8"<root>", allocator);
        wrapped.Append(StringView(reinterpret_cast<const utf8char*>(settings.Data()), settings.Size()));
        wrapped.Append(u8"</root>");
        foundation::xml::XmlDocument document(allocator);
        if (document.Parse(wrapped.AsView()) != foundation::xml::XmlResult::Ok)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        foundation::xml::XmlSerializer ar(document);
        BeginVersionedPayload(ar, *into.GetType());
        into.Serialize(ar);
        EndVersionedPayload(ar);
        return ar.IsOk() ? Status{} : ar.GetStatus();
    }

    // Writes `from` into an entry's settings, replacing them.
    [[nodiscard]] inline Status WritePluginSettings(ISerializable& from, Array<u8>& settings, IAllocator& allocator)
    {
        foundation::xml::XmlSerializer ar(allocator);
        BeginVersionedPayload(ar, *from.GetType());
        from.Serialize(ar);
        EndVersionedPayload(ar);
        if (!ar.IsOk())
        {
            return ar.GetStatus();
        }
        String written(allocator);
        ar.GetOutput(written);
        foundation::xml::XmlDocument document(allocator);
        if (document.Parse(written.AsView()) != foundation::xml::XmlResult::Ok || document.RootElement() == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        String elements(allocator);
        for (foundation::xml::XmlNode* child = document.RootElement()->FirstChild(); child != nullptr;
             child = child->NextSibling())
        {
            if (child->NodeType() == foundation::xml::XmlNodeType::Element)
            {
                String outer(allocator);
                child->GetOuterXml(outer);
                elements.Append(outer.AsView());
            }
        }
        settings.Clear();
        settings.Resize(elements.Size());
        if (!elements.IsEmpty())
        {
            MemCopy(settings.Data(), elements.Data(), elements.Size());
        }
        return Status{};
    }
}
