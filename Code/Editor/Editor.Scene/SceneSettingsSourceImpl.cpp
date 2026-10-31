// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_settings_source partition, its body. See SceneSettingsSource.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app; // ReflectedEditCommand
import :edit;
import :scene_settings_source;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        // A path's top-level property: what comes before its first '.'.
        String TopOf(StringView path)
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (path[i] == u8'.')
                    return String(path.SubStr(0, i));
            return String(path);
        }

    }

    // Forwards whatever either reflected source reports to this source's client.
    class SceneSettingsSource::Forwarder final : public gui::IPropertySourceClient
    {
    public:
        explicit Forwarder(SceneSettingsSource& owner) : m_owner(owner) {}
        void OnPropertiesChanged() override { m_owner.NotifyPropertiesChanged(); }
        void OnValuesChanged() override { m_owner.NotifyValuesChanged(); }

    private:
        SceneSettingsSource& m_owner;
    };

    SceneSettingsSource::SceneSettingsSource(IAllocator& allocator, SceneEditContext& edit, const TypeInfo& settingsType, gui::IPropertyTextFormat& format)
        : m_allocator(allocator), m_edit(&edit), m_type(&settingsType), m_format(&format)
    {
        m_forwarder = MakeUnique<Forwarder>(m_allocator, *this);
        SceneSettingsSource* self = this;
        m_own = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
        m_own->SetTextFormat(m_format);
        m_own->SetClient(m_forwarder.Get());
        m_own->SetOnChange([self](const gui::ReflectedChange& change) { self->Changed(Guid{}, change); });
        m_profile = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
        m_profile->SetTextFormat(m_format);
        m_profile->SetClient(m_forwarder.Get());
        m_profile->SetOnChange([self](const gui::ReflectedChange& change) { self->Changed(self->m_profileId, change); });
        Bind();
    }

    SceneSettingsSource::~SceneSettingsSource()
    {
        m_own->SetClient(nullptr);
        m_profile->SetClient(nullptr);
    }

    void SceneSettingsSource::Bind()
    {
        void* own = m_edit->SettingsEditTarget(m_type, Guid{});
        if (own != m_ownValues)
        {
            m_ownValues = own;
            const Instance object(own, m_type);
            m_own->SetObjects(own != nullptr ? Span<const Instance>(&object, 1u) : Span<const Instance>{});
        }
        scene::SceneSystem* system = m_edit->FindSystemBySettingsType(m_type);
        const Guid profile = system != nullptr ? system->SettingsProfile() : Guid{};
        void* values = profile.IsNil() ? nullptr : m_edit->SettingsEditTarget(m_type, profile);
        if (values != m_profileValues || profile != m_profileId)
        {
            m_profileValues = values;
            m_profileId = profile;
            const Instance object(values, m_type);
            m_profile->SetObjects(values != nullptr ? Span<const Instance>(&object, 1u) : Span<const Instance>{});
            NotifyPropertiesChanged(); // which values each row shows may have changed
        }
    }

    void SceneSettingsSource::Refresh()
    {
        Bind();
        NotifyValuesChanged();
    }

    Guid SceneSettingsSource::ProfileOf(StringView path) const
    {
        const String top = TopOf(path);
        return m_edit->SettingsEditProfile(m_type, reinterpret_cast<const char*>(top.CStr()));
    }

    gui::ReflectedPropertySource* SceneSettingsSource::SourceOf(StringView path) const
    {
        const Guid profile = ProfileOf(path);
        if (!profile.IsNil() && profile == m_profileId && m_profileValues != nullptr)
            return m_profile.Get();
        return m_ownValues != nullptr ? m_own.Get() : nullptr;
    }

    void SceneSettingsSource::Changed(const Guid& profile, const gui::ReflectedChange& change)
    {
        // The source has already written the value; the command keeps it and puts it back on undo.
        SceneEditContext* edit = m_edit;
        const TypeInfo* type = m_type;
        (void)m_edit->Commands().Execute(UniquePtr<IEditorCommand>(
            EditorRootAllocator().New<app::ReflectedEditCommand>(
                *m_type, [edit, type, profile]() { return edit->SettingsEditTarget(type, profile); }, change, *m_format, u8"inspector_settings",
                [edit, type, profile]()
                {
                    edit->ResolveSettingsResources(type);           // bind a newly written reference id
                    edit->NoteSettingsProfileEdited(type, profile); // nil: nothing to note
                }),
            EditorRootAllocator()));
    }

    void SceneSettingsSource::GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const
    {
        // Both stand for the same type, so the rows are the block's; the values come from each
        // source.
        if (gui::ReflectedPropertySource* source = SourceOf(parent.IsEmpty() ? StringView(u8"") : parent))
            source->GetProperties(parent, out);
    }
    gui::Variant SceneSettingsSource::GetValue(StringView path, usize object) const
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr ? source->GetValue(path, object) : gui::Variant{};
    }
    void SceneSettingsSource::BeginEdit(StringView path)
    {
        if (gui::ReflectedPropertySource* source = SourceOf(path))
            source->BeginEdit(path);
    }
    void SceneSettingsSource::PreviewEdit(StringView path, const gui::PropertyWrite& write)
    {
        if (gui::ReflectedPropertySource* source = SourceOf(path))
            source->PreviewEdit(path, write);
    }
    void SceneSettingsSource::CommitEdit(StringView path, const gui::PropertyWrite& write)
    {
        if (gui::ReflectedPropertySource* source = SourceOf(path))
            source->CommitEdit(path, write);
    }
    void SceneSettingsSource::CancelEdit(StringView path)
    {
        if (gui::ReflectedPropertySource* source = SourceOf(path))
            source->CancelEdit(path);
    }
    bool SceneSettingsSource::EditList(StringView path, const gui::ListEdit& edit)
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr && source->EditList(path, edit);
    }
    gui::Variant SceneSettingsSource::GetDefault(StringView path, usize object) const
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr ? source->GetDefault(path, object) : gui::Variant{};
    }
    bool SceneSettingsSource::Revert(StringView path)
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr && source->Revert(path);
    }
    String SceneSettingsSource::GetRevertText(StringView path) const
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr ? source->GetRevertText(path) : String{};
    }
    String SceneSettingsSource::CopyValue(StringView path) const
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr ? source->CopyValue(path) : String{};
    }
    bool SceneSettingsSource::PasteValue(StringView path, StringView text, String& reason)
    {
        gui::ReflectedPropertySource* source = SourceOf(path);
        return source != nullptr && source->PasteValue(path, text, reason);
    }
}
