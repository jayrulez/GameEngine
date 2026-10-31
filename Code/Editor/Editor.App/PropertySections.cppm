// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :property_sections partition
//
// PropertySections: a property source made of others, one section each, for an editor's grid to
// show what no one source stands for (a scene inspector: an entity's name, its transform and each
// of its components, each a source of its own). The grid shows the sections in their order, each
// section's top-level properties under its title as their category (a header the grid folds). A
// section's paths are its source's paths under a key of its own ("s2/position.x"), so every call is
// forwarded to the section's source with its own path, and what a section's source tells its
// client is passed on to this source's client. The sources remain their owners' and must outlive
// their time as sections. It lives in the editor for now and moves to GUI.Toolkit if something
// outside the editor needs it.

module;
#include "Core/Prelude.h"

export module editor.app:property_sections;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit; // IPropertySource

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;


    class PropertySections final : public gui::IPropertySource
    {
    public:
        explicit PropertySections(core::IAllocator& allocator) : m_sections(allocator), m_forwarder(*this) {}
        // Releases its sources without telling its client anything: the client may already be gone.
        ~PropertySections() override { Release(); }
        PropertySections(const PropertySections&) = delete;
        PropertySections& operator=(const PropertySections&) = delete;

        /// Adds a section after the others: `source`'s properties under `title`. `key` is what the
        /// section's paths start with and how a caller finds it again; it is unique among the
        /// sections and holds no '.' or '/'.
        void Add(core::StringView key, core::StringView title, gui::IPropertySource& source)
        {
            m_sections.PushBack(Section{core::String(key), core::String(title), &source});
            source.SetClient(&m_forwarder);
            NotifyPropertiesChanged();
        }
        /// Removes every section, releasing their sources.
        void Clear()
        {
            Release();
            NotifyPropertiesChanged();
        }
        /// Removes every section without touching their sources, for an owner whose sources may
        /// already be gone (a page's own sources are a derived page's members, which are destroyed
        /// before its base).
        void Abandon() { m_sections.Clear(); }
        [[nodiscard]] usize SectionCount() const noexcept { return m_sections.Size(); }
        /// Makes every row read-only and refuses every edit through it (a scene page while it simulates).
        void SetReadOnly(bool readOnly)
        {
            if (m_readOnly == readOnly)
                return;
            m_readOnly = readOnly;
            NotifyPropertiesChanged();
        }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }
        /// The section a path is in (a header's `#title`, a property's own path); empty when none.
        [[nodiscard]] core::StringView SectionOf(core::StringView path) const
        {
            if (path.Size() != 0 && path[0] == u8'#')
            {
                for (const Section& section : m_sections)
                    if (section.Title.AsView() == path.SubStr(1, path.Size() - 1))
                        return section.Key.AsView();
                return core::StringView{};
            }
            const Section* section = Find(path);
            return section != nullptr ? section->Key.AsView() : core::StringView{};
        }
        /// A section's path for its source's `inner` path (`inner` empty: the section itself).
        [[nodiscard]] core::String PathIn(core::StringView key, core::StringView inner) const
        {
            core::String path(key);
            path.Append(core::StringView(u8"/"));
            path.Append(inner);
            return path;
        }

        // === gui::IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override
        {
            usize count = 0;
            for (const Section& section : m_sections)
                count = core::Max(count, section.Source->ObjectCount());
            return count;
        }
        void GetProperties(core::StringView parent, core::Array<gui::PropertyInfo>& out) const override
        {
            if (parent.Size() == 0)
            {
                for (const Section& section : m_sections)
                {
                    core::Array<gui::PropertyInfo> own;
                    section.Source->GetProperties(core::StringView{}, own);
                    for (gui::PropertyInfo& info : own)
                    {
                        info.ReadOnly = info.ReadOnly || m_readOnly;
                        info.Name = PathIn(section.Key.AsView(), info.Name.AsView());
                        info.Category = section.Title;
                        if (info.DisplayName.Size() == 0)
                            info.DisplayName = core::String(Inner(info.Name.AsView()));
                        out.PushBack(core::Move(info));
                    }
                }
                return;
            }
            const Section* section = Find(parent);
            if (section == nullptr)
                return;
            const usize first = out.Size();
            section->Source->GetProperties(Inner(parent), out);
            for (usize i = first; i < out.Size(); ++i)
                out[i].ReadOnly = out[i].ReadOnly || m_readOnly;
        }
        [[nodiscard]] gui::Variant GetValue(core::StringView path, usize object) const override
        {
            const Section* section = Find(path);
            return section != nullptr && object < section->Source->ObjectCount() ? section->Source->GetValue(Inner(path), object) : gui::Variant{};
        }
        void BeginEdit(core::StringView path) override
        {
            if (Section* section = m_readOnly ? nullptr : Find(path))
                section->Source->BeginEdit(Inner(path));
        }
        void PreviewEdit(core::StringView path, const gui::PropertyWrite& write) override
        {
            if (Section* section = m_readOnly ? nullptr : Find(path))
                section->Source->PreviewEdit(Inner(path), write);
        }
        void CommitEdit(core::StringView path, const gui::PropertyWrite& write) override
        {
            if (Section* section = m_readOnly ? nullptr : Find(path))
                section->Source->CommitEdit(Inner(path), write);
        }
        void CancelEdit(core::StringView path) override
        {
            if (Section* section = Find(path))
                section->Source->CancelEdit(Inner(path));
        }
        bool EditList(core::StringView path, const gui::ListEdit& edit) override
        {
            Section* section = m_readOnly ? nullptr : Find(path);
            return section != nullptr && section->Source->EditList(Inner(path), edit);
        }
        [[nodiscard]] gui::Variant GetDefault(core::StringView path, usize object) const override
        {
            const Section* section = Find(path);
            return section != nullptr ? section->Source->GetDefault(Inner(path), object) : gui::Variant{};
        }
        bool Revert(core::StringView path) override
        {
            Section* section = m_readOnly ? nullptr : Find(path);
            return section != nullptr && section->Source->Revert(Inner(path));
        }
        [[nodiscard]] core::String GetRevertText(core::StringView path) const override
        {
            const Section* section = Find(path);
            return section != nullptr ? section->Source->GetRevertText(Inner(path)) : core::String{};
        }
        [[nodiscard]] core::String CopyValue(core::StringView path) const override
        {
            const Section* section = Find(path);
            return section != nullptr ? section->Source->CopyValue(Inner(path)) : core::String{};
        }
        bool PasteValue(core::StringView path, core::StringView text, core::String& reason) override
        {
            if (m_readOnly)
            {
                reason = core::String(u8"Read only");
                return false;
            }
            Section* section = Find(path);
            return section != nullptr && section->Source->PasteValue(Inner(path), text, reason);
        }

    private:
        struct Section
        {
            core::String Key;
            core::String Title;
            gui::IPropertySource* Source = nullptr;
        };
        // Passes what a section's source reports on to this source's client.
        class Forwarder final : public gui::IPropertySourceClient
        {
        public:
            explicit Forwarder(PropertySections& owner) : m_owner(owner) {}
            void OnPropertiesChanged() override { m_owner.NotifyPropertiesChanged(); }
            void OnValuesChanged() override { m_owner.NotifyValuesChanged(); }

        private:
            PropertySections& m_owner;
        };

        void Release()
        {
            for (const Section& section : m_sections)
                if (section.Source->GetClient() == &m_forwarder)
                    section.Source->SetClient(nullptr);
            m_sections.Clear();
        }
        // A path's key: what comes before its first '/'.
        [[nodiscard]] static core::StringView KeyOf(core::StringView path)
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == u8'/')
                    return path.SubStr(0, i);
            return core::StringView{};
        }
        // A path as its section's source knows it: what comes after its first '/'.
        [[nodiscard]] static core::StringView Inner(core::StringView path)
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == u8'/')
                    return path.SubStr(i + 1, path.Size() - i - 1);
            return path;
        }
        [[nodiscard]] Section* Find(core::StringView path)
        {
            const core::StringView key = KeyOf(path);
            for (Section& section : m_sections)
                if (section.Key.AsView() == key)
                    return &section;
            return nullptr;
        }
        [[nodiscard]] const Section* Find(core::StringView path) const { return const_cast<PropertySections*>(this)->Find(path); }

        core::Array<Section> m_sections;
        Forwarder m_forwarder;
        bool m_readOnly = false;
    };
}
