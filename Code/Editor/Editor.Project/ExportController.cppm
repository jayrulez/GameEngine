// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_controller partition.
//
// ExportPresetsController is the non-UI logic behind the editor's "Export presets" panel: it
// loads a project's export_presets.xml (:export_preset), applies changes (add, edit, duplicate,
// delete) and saves it back. Keeping it apart from the UI makes the authoring rules (unique names,
// the duplicate " Copy" suffix, ordering) testable headlessly; the dialog is a thin driver over
// it. As with the ExportPreset persistence helpers, Load falls back to DefaultExportPresets when
// a project has no file yet, and Save writes through the project's writable filesystem.

module;
#include "Core/Prelude.h"

export module editor.project:export_controller;

import foundation.core;
import foundation.vfs;
import :export_preset;

using namespace foundation::core;
namespace vfs = foundation::vfs;

export namespace editor
{
    // Owns a project's ExportPresetSet and the add/edit/duplicate/delete operations the editor's
    // presets panel performs on it. It holds the set by value (the loaded set is copied in, changed
    // and saved back), so a test can drive the whole round trip against a temp filesystem without
    // any UI.
    class ExportPresetsController
    {
    public:
        // Loads the project's export_presets.xml. When it is absent (the first export), seeds the
        // built-in default (one host-platform preset) so the panel always has something to show
        // and export.
        void Load(vfs::IFileSystem& projectFs)
        {
            m_set.presets.Clear();
            if (!LoadExportPresets(projectFs, m_set).IsOk())
            {
                DefaultExportPresets(m_set);
            }
        }

        // Saves the current set to export_presets.xml (the panel calls this after every change it
        // wants to keep: Add, Edit, Duplicate, Delete).
        [[nodiscard]] Status Save(vfs::IWritableFileSystem& projectFs)
        {
            return SaveExportPresets(projectFs, m_set);
        }

        [[nodiscard]] usize Count() const noexcept { return m_set.presets.Size(); }
        [[nodiscard]] const ExportPreset& At(usize index) const { return m_set.presets[index]; }
        [[nodiscard]] ExportPreset& At(usize index) { return m_set.presets[index]; }
        [[nodiscard]] const ExportPresetSet& Set() const noexcept { return m_set; }
        [[nodiscard]] ExportPresetSet& Set() noexcept { return m_set; }

        // Appends `preset`, forcing a unique, non-empty name (export by name needs it). Returns the
        // new preset's index.
        usize Add(const ExportPreset& preset)
        {
            ExportPreset copy = preset;
            copy.name = UniqueName(copy.name.AsView(), -1);
            m_set.presets.PushBack(Move(copy));
            return m_set.presets.Size() - 1;
        }

        // Replaces the preset at `index` with `preset`, keeping its name unique among the others
        // (an unchanged name stays as it is, since `index` is excluded from the check).
        void Update(usize index, const ExportPreset& preset)
        {
            if (index >= m_set.presets.Size())
            {
                return;
            }
            ExportPreset copy = preset;
            copy.name = UniqueName(copy.name.AsView(), static_cast<isize>(index));
            m_set.presets[index] = Move(copy);
        }

        // Appends a copy of the preset at `index` with a distinct "<name> Copy" name. Returns the
        // new index (or `index` unchanged when it is out of range).
        usize Duplicate(usize index)
        {
            if (index >= m_set.presets.Size())
            {
                return index;
            }
            ExportPreset copy = m_set.presets[index];
            copy.name =
                UniqueName(copy.name.AsView(), -1); // the original is still present, so the copy gets " Copy"
            m_set.presets.PushBack(Move(copy));
            return m_set.presets.Size() - 1;
        }

        // Removes the preset at `index` (does nothing when it is out of range).
        void Remove(usize index)
        {
            if (index < m_set.presets.Size())
            {
                m_set.presets.RemoveAt(index);
            }
        }

        // A name unique among the presets (excluding index `skip`, or -1 for none): `base` as it is
        // when free, else "<base> Copy", then "<base> Copy 2", "<base> Copy 3", ... Empty `base`
        // becomes "Preset".
        [[nodiscard]] String UniqueName(StringView base, isize skip) const
        {
            const auto taken = [this, skip](StringView candidate) -> bool
            {
                for (usize i = 0; i < m_set.presets.Size(); ++i)
                {
                    if (static_cast<isize>(i) == skip)
                    {
                        continue;
                    }
                    if (m_set.presets[i].name.AsView() == candidate)
                    {
                        return true;
                    }
                }
                return false;
            };

            String root(base);
            if (root.IsEmpty())
            {
                root = String(u8"Preset");
            }
            if (!taken(root.AsView()))
            {
                return root;
            }

            String first(root);
            first += u8" Copy";
            if (!taken(first.AsView()))
            {
                return first;
            }
            for (u32 n = 2;; ++n)
            {
                String candidate(root);
                candidate += u8" Copy ";
                candidate += Format(u8"{}", n);
                if (!taken(candidate.AsView()))
                {
                    return candidate;
                }
            }
        }

    private:
        ExportPresetSet m_set;
    };
}
