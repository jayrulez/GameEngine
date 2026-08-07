// Draconic::EditorCore - :export_template partition.
//
// Export templates: portable, per-platform prebuilt bundles (a player binary + its runtime sidecars +
// a template.xml manifest) that presets reference by id/platform (docs/design/export.md §2). They live
// in a machine-local templates root (not committed), are importable/downloadable, and are decoupled
// from any one machine's paths. The HOST implicit template is synthesized from the running tool's own
// directory (Bin/...), so a dev export for the current platform needs zero setup.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <filesystem> // recursive dir copy when importing a template bundle

module draconic.editor.core;

import draconic.foundation;
import draconic.vfs;
import draconic.xml.serialization;
import draconic.engine.project;
import :export_preset; // ExportPreset, ExportPresetSet

using namespace draconic::foundation;

namespace draconic::editor
{
    StringView ExportTemplate::EffectiveConfig() const noexcept
    {
        return config.IsEmpty() ? StringView(u8"Release") : config.AsView();
    }

    void ExportTemplate::Serialize(ISerializer& ar)
    {
        draconic::foundation::Serialize(ar, "id", id);
        draconic::foundation::Serialize(ar, "name", name);
        draconic::foundation::Serialize(ar, "platform", platform);
        draconic::foundation::Serialize(ar, "engineVersion", engineVersion);
        draconic::foundation::Serialize(ar, "playerBinary", playerBinary);
        draconic::foundation::Serialize(ar, "sidecars", sidecars);
        draconic::foundation::Serialize(ar, "notes", notes);
        // v2 added the (platform, config) axis: config + compiler metadata + a parallel symbols
        // group. A v1 template.xml lacks these fields, so gate them on the stored data version -
        // reading an old manifest leaves config empty (normalized to Release below) and works.
        if (ar.Version() >= 2)
        {
            draconic::foundation::Serialize(ar, "config", config);
            draconic::foundation::Serialize(ar, "compiler", compiler);
            draconic::foundation::Serialize(ar, "symbols", symbols);
        }
        if (ar.Mode() == SerializeMode::Read && config.IsEmpty())
        {
            config = String(u8"Release"); // back-compat: absent config => Release
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
}
