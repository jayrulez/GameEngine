// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :export_template partition.
//
// Export templates: portable, per-platform prebuilt bundles (a player binary + its runtime sidecars +
// a template.xml manifest) that presets reference by id/platform. They live
// in a machine-local templates root (not committed), are importable/downloadable, and are decoupled
// from any one machine's paths. The HOST implicit template is synthesized from the running tool's own
// directory (Bin/...), so a dev export for the current platform needs zero setup.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem> // recursive dir copy when importing a template bundle

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import engine.project;
import :export_preset; // ExportPreset, ExportPresetSet

using namespace foundation::core;
namespace vfs = foundation::vfs;

namespace editor
{
    StringView ExportTemplate::EffectiveConfig() const noexcept
    {
        return config.IsEmpty() ? StringView(u8"Release") : config.AsView();
    }

    void ExportTemplate::Serialize(ISerializer& ar)
    {
        foundation::core::Serialize(ar, "id", id);
        foundation::core::Serialize(ar, "name", name);
        foundation::core::Serialize(ar, "platform", platform);
        foundation::core::Serialize(ar, "engineVersion", engineVersion);
        foundation::core::Serialize(ar, "playerBinary", playerBinary);
        foundation::core::Serialize(ar, "sidecars", sidecars);
        foundation::core::Serialize(ar, "notes", notes);
        // The (platform, config) axis: config + compiler metadata + a parallel symbols group.
        foundation::core::Serialize(ar, "config", config);
        foundation::core::Serialize(ar, "compiler", compiler);
        foundation::core::Serialize(ar, "symbols", symbols);
        SerializeAppended(ar, "icon", icon); // after the rest: older manifests lack it
        if (ar.Mode() == SerializeMode::Read && config.IsEmpty())
        {
            config = String(u8"Release"); // an empty config resolves to Release
        }
    }
    const ExportTemplate* TemplateRegistry::FindBy(StringView platform, StringView config) const
    {
        const StringView wantConfig = config.IsEmpty() ? StringView(u8"Release") : config;

        // Pass 1: exact (platform, config).
        const ExportTemplate* exactHost = nullptr;
        for (const UniquePtr<ExportTemplate>& t : m_templates)
        {
            if (t->platform.AsView() != platform || t->EffectiveConfig() != wantConfig)
            {
                continue;
            }
            if (t->isHost)
            {
                exactHost = t.Get();
            }
            else
            {
                return t.Get();
            }
        }
        if (exactHost != nullptr)
        {
            return exactHost;
        }

        // Pass 2: platform-only fallback, preferring a Release config, imported over host.
        const ExportTemplate* bestImported = nullptr;
        bool bestImportedRelease = false;
        const ExportTemplate* bestHost = nullptr;
        bool bestHostRelease = false;
        for (const UniquePtr<ExportTemplate>& t : m_templates)
        {
            if (t->platform.AsView() != platform)
            {
                continue;
            }
            const bool isRelease = t->EffectiveConfig() == StringView(u8"Release");
            if (t->isHost)
            {
                if (bestHost == nullptr || (isRelease && !bestHostRelease))
                {
                    bestHost = t.Get();
                    bestHostRelease = isRelease;
                }
            }
            else
            {
                if (bestImported == nullptr || (isRelease && !bestImportedRelease))
                {
                    bestImported = t.Get();
                    bestImportedRelease = isRelease;
                }
            }
        }
        if (bestImported != nullptr)
        {
            return bestImported;
        }
        return bestHost;
    }

    const ExportTemplate* TemplateRegistry::Resolve(const ExportPreset& preset) const
    {
        if (!preset.templateId.IsEmpty())
        {
            return FindById(preset.templateId.AsView());
        }
        return FindBy(preset.platform.AsView(), preset.config.AsView());
    }

    // Declared in ExportTemplate.cppm; defined here so <filesystem> stays out of the module
    // interface (see the note at those declarations). Bodies are unchanged from when they were
    // inline there.
    Status ImportTemplate(StringView srcDir, StringView templatesRoot, String* outId)
    {
        vfs::NativeFileSystem srcFs(srcDir, editor::EditorRootAllocator());
        ExportTemplate manifest;
        if (!LoadTemplateManifest(srcFs, manifest).IsOk() || manifest.id.IsEmpty())
        {
            return Status{ErrorCode::NotFound};
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        const String rootCopy(templatesRoot);
        fs::create_directories(reinterpret_cast<const char*>(rootCopy.CStr()), ec);
        const String dst = PathJoin(templatesRoot, manifest.id.AsView());
        const String srcCopy(srcDir);
        fs::copy(fs::path(reinterpret_cast<const char*>(srcCopy.CStr())),
                 fs::path(reinterpret_cast<const char*>(dst.CStr())),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            return Status{ErrorCode::Internal};
        }
        if (outId != nullptr)
        {
            *outId = manifest.id;
        }
        return Status{};
    }

    Status RemoveTemplate(StringView templatesRoot, StringView templateId)
    {
        if (templateId.IsEmpty())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        const String dir = PathJoin(templatesRoot, templateId);
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto removed =
            fs::remove_all(fs::path(reinterpret_cast<const char*>(dir.CStr())), ec);
        if (ec)
        {
            return Status{ErrorCode::Internal};
        }
        return (removed > 0) ? Status{}
                             : Status{ErrorCode::NotFound}; // nothing deleted => not there
    }
}
