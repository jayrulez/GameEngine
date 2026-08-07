// Draconic::EditorCore - :export_controller partition.
//
// ExportPresetsController: the non-UI logic behind the editor's "Export presets" panel - a
// load -> mutate (add / edit / duplicate / delete) -> save round-trip over a project's
// export_presets.xml (:export_preset). Factored out of the raw UI so the authoring rules
// (name uniqueness, the duplicate " Copy" suffix, ordering) are testable headlessly; the dialog
// is a thin driver over this. Mirrors the ExportPreset persistence helpers - Load falls back to
// DefaultExportPresets when a project has no file yet; Save writes through the project's writable
// filesystem.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.core:export_controller;

import draconic.foundation;
import draconic.vfs;
import :export_preset;

using namespace draconic::foundation;

export namespace draconic::editor
{
    // Owns a project's ExportPresetSet and the add/edit/duplicate/delete operations the editor's
    // presets panel performs on it. Value-holding (copy the loaded set in, mutate, save back), so a
    // test can drive the whole round-trip against a temp filesystem without any UI.
    class ExportPresetsController
    {
    public:
        // Load the project's export_presets.xml; when absent (first export), seed with the built-in
        // default (one host-platform preset) so the panel always has something to show / export.
        void Load(vfs::IFileSystem& projectFs)
        {
            m_set.presets.Clear();
            if (!LoadExportPresets(projectFs, m_set).IsOk())
            {
                DefaultExportPresets(m_set);
            }
        }

        // Persist the current set to export_presets.xml (the panel calls this after every mutation
        // it wants durable - Add/Edit/Duplicate/Delete).
        [[nodiscard]] Status Save(vfs::IWritableFileSystem& projectFs)
        {
            return SaveExportPresets(projectFs, m_set);
        }

        [[nodiscard]] usize Count() const noexcept { return m_set.presets.Size(); }
        [[nodiscard]] const ExportPreset& At(usize index) const { return m_set.presets[index]; }
        [[nodiscard]] ExportPreset& At(usize index) { return m_set.presets[index]; }
        [[nodiscard]] const ExportPresetSet& Set() const noexcept { return m_set; }
        [[nodiscard]] ExportPresetSet& Set() noexcept { return m_set; }

        // Append `preset`, forcing a unique, non-empty name (export-by-name needs it). Returns the
        // new preset's index.
        usize Add(const ExportPreset& preset)
        {
            ExportPreset copy = preset;
            copy.name = UniqueName(copy.name.AsView(), -1);
            m_set.presets.PushBack(Move(copy));
            return m_set.presets.Size() - 1;
        }

        // Replace the preset at `index` with `preset`, keeping its name unique among the OTHERS
        // (a no-op rename when the name is unchanged, since `index` is excluded from the check).
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

        // Append a copy of the preset at `index` with a distinct "<name> Copy" name. Returns the new
        // index (or `index` unchanged when out of range).
        usize Duplicate(usize index)
        {
            if (index >= m_set.presets.Size())
            {
                return index;
            }
            ExportPreset copy = m_set.presets[index];
            copy.name =
                UniqueName(copy.name.AsView(), -1); // original still present => gets " Copy"
            m_set.presets.PushBack(Move(copy));
            return m_set.presets.Size() - 1;
        }

        // Drop the preset at `index` (out-of-range = no-op).
        void Remove(usize index)
        {
            if (index < m_set.presets.Size())
            {
                m_set.presets.RemoveAt(index);
            }
        }

        // A name unique among the presets (excluding index `skip`, or -1 for none): `base` verbatim
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
