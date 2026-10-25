// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :entity_ref_editor partition, its body. See EntityRefEditor.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :edit;
import :hierarchy;
import :entity_ref_editor;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        String GuidText(const Guid& id)
        {
            utf8char chars[37];
            id.ToChars(chars);
            return String(StringView(chars, 36));
        }
        // An entity's first component's icon, else the plain entity's.
        TypeIcon IconOf(scene::Scene& scene, scene::EntityHandle e, EditorIcons* icons)
        {
            TypeIcon found;
            if (icons != nullptr)
                scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                                     {
                                         if (found.Source.IsEmpty() && manager.ComponentType() != nullptr && manager.HasComponent(e))
                                             found = icons->For(*manager.ComponentType(), StringView{});
                                     });
            if (found.Source.IsEmpty())
                found.Source = String(u8"Editor/Icons/cube.svg");
            return found;
        }
        // The scene's entities in their tree's order, each at its depth.
        void Collect(scene::Scene& scene, scene::EntityHandle e, u32 depth, EditorIcons* icons, Array<app::PickerItem>& out)
        {
            app::PickerItem item;
            item.Key = GuidText(scene.GetEntityId(e));
            item.Name = String(scene.GetEntityName(e));
            if (item.Name.IsEmpty())
                item.Name = String(u8"(unnamed)");
            const TypeIcon icon = IconOf(scene, e, icons);
            item.Glyph = icon.Source;
            item.GlyphClass = icon.Class;
            item.Depth = depth;
            out.PushBack(Move(item));
            for (scene::EntityHandle c = scene.GetFirstChild(e); c.IsAssigned(); c = scene.GetNextSibling(c))
                Collect(scene, c, depth + 1, icons, out);
        }
    }

    void EntityRefEditor::OnBind(const gui::PropertyInfo& info, const gui::PropertyValue& value)
    {
        (void)info;
        m_current = Guid{};
        m_mixed = value.Mixed;
        if (!m_mixed)
            (void)Guid::TryParse(Trim(value.Value.AsString()), m_current);
        EntityRefEditor* self = this;
        const bool writes = !IsReadOnly() && m_services->edit != nullptr;
        m_slot->SetOnPick(writes && m_services->pick ? Function<void()>([self]() { self->Pick(); }) : Function<void()>{});
        m_slot->SetOnClear(writes ? Function<void()>([self]() { self->Assign(Guid{}); }) : Function<void()>{});
        Show();
    }

    void EntityRefEditor::Show()
    {
        if (m_mixed)
        {
            m_slot->SetValue(u8"(mixed)");
            m_slot->SetGlyph(StringView{});
            return;
        }
        if (m_current.IsNil() || m_services->edit == nullptr)
        {
            m_slot->SetValue(StringView{});
            m_slot->SetGlyph(StringView{});
            return;
        }
        scene::Scene& scene = m_services->edit->Scene();
        const scene::EntityHandle e = scene.FindEntity(m_current);
        if (!e.IsAssigned())
        {
            m_slot->SetValue(kMissingText); // an id no entity of the scene has
            m_slot->SetGlyph(u8"Editor/Icons/warning.svg");
            return;
        }
        const StringView name = scene.GetEntityName(e);
        m_slot->SetValue(name.IsEmpty() ? StringView(u8"(unnamed)") : name);
        const TypeIcon icon = IconOf(scene, e, m_services->icons);
        m_slot->SetGlyph(icon.Source.AsView(), icon.Class.AsView());
    }

    app::PickerRequest EntityRefEditor::Request()
    {
        app::PickerRequest request;
        request.Placeholder = String(u8"Search the scene");
        request.Tree = true;
        request.AllowNone = true;
        if (!m_current.IsNil() && !m_mixed)
            request.Current = GuidText(m_current);
        if (m_services->edit != nullptr)
        {
            scene::Scene& scene = m_services->edit->Scene();
            for (scene::EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
                Collect(scene, r, 0, m_services->icons, request.Items);
        }
        request.EmptyTitle = String(u8"No entities yet");
        request.EmptyText = String(u8"The scene has no entity to point at.");
        RefPtr<EntityRefEditor> keep(this);
        const String path(GetPath());
        request.OnPicked = [keep, path](StringView key)
        {
            if (keep->GetPath() != path.AsView())
                return; // the row shows another property now
            Guid id;
            (void)Guid::TryParse(key, id); // None: nil
            keep->Assign(id);
        };
        return request;
    }

    void EntityRefEditor::Pick()
    {
        if (m_services->pick)
            m_services->pick(Request());
    }

    void EntityRefEditor::Assign(const Guid& id)
    {
        RefPtr<EntityRefEditor> keep(this); // the write may rebind the row
        ApplyEdit(gui::PropertyWrite{gui::Variant(id.IsNil() ? StringView{} : GuidText(id).AsView())});
    }

    // An entity dragged from the hierarchy: the first it carries.
    void EntityRefEditor::OnDragQuery(gui::DropQuery& query)
    {
        Array<Guid> entities;
        if (IsReadOnly() || m_services->edit == nullptr || !EntityDrag::Read(query.Payload, entities))
            return;
        const scene::EntityHandle e = m_services->edit->Scene().FindEntity(entities[0]);
        if (!e.IsAssigned())
        {
            query.Refuse(u8"That entity is in another scene");
            return;
        }
        query.Accept(gui::DropEffect::Link);
        query.Reason = Format(u8"Use {}", m_services->edit->Scene().GetEntityName(e));
    }
    void EntityRefEditor::OnDrop(const gui::DragEvent& event) { (void)Take(event.Payload); }
    bool EntityRefEditor::Take(const gui::DragPayload& payload)
    {
        Array<Guid> entities;
        if (IsReadOnly() || m_services->edit == nullptr || !EntityDrag::Read(payload, entities) || !m_services->edit->Scene().FindEntity(entities[0]).IsAssigned())
            return false;
        Assign(entities[0]);
        return true;
    }
}
