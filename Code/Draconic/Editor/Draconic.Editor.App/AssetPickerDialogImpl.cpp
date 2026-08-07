// Draconic::EditorApp - :asset_picker_dialog partition.
//
// AssetPickerDialog: a modal, READ-ONLY mirror of the asset browser for resource-ref picking -
// group tree on the left, matching instances on the right, filter across all groups. Replaces
// the flat context-menu picker (which grew unusable once same-named assets lived in different
// groups). Deliberately not the browser itself: no rename (Sedulous's picker made slow-clicks
// start renames - the recorded annoyance), no delete, no cook actions.
//
//   - only instances whose type is in `assetTypeNames` are listed
//   - FAVORITES (pinned via right-click here or anywhere EditorContext favorites reach) sort
//     first with a * prefix, regardless of the selected group
//   - double-click or [Select] confirms; [Clear] picks "none"; [Cancel]/Escape dismisses
//
// The result is delivered through OnPicked(guid) (nil = cleared), fired BEFORE the dialog
// closes itself.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.editor.app;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :editor_icons;

using namespace draconic::foundation;

namespace draconic::editor::app
{
    bool AssetPickerDialog::TypeMatches(content::Instance& instance) const
    {
        // An EMPTY filter matches every type (the generic asset page's untyped guid picker).
        if (m_typeNames.IsEmpty())
        {
            return true;
        }
        for (const String& typeName : m_typeNames)
        {
            if (instance.TypeName() == typeName.AsView())
            {
                return true;
            }
        }
        return false;
    }

    i32 AssetPickerDialog::AddGroupNode(content::Group* group, i32 depth)
    {
        const i32 nodeId = static_cast<i32>(m_groups.Size());
        GroupNode node;
        node.group = group;
        node.depth = depth;
        m_groups.PushBack(Move(node));
        for (content::Group* child : group->Groups())
        {
            const i32 childId = AddGroupNode(child, depth + 1);
            m_groups[static_cast<usize>(nodeId)].children.PushBack(childId);
        }
        return nodeId;
    }

    void AssetPickerDialog::RebuildModel()
    {
        m_groups.Clear();
        content::Group* root = (m_context->Project() != nullptr)
                                   ? m_context->Project()->SourceDb().RootGroup()
                                   : nullptr;
        if (root != nullptr)
        {
            AddGroupNode(root, 0);
        }
        if (m_selectedGroup == nullptr)
        {
            m_selectedGroup = root;
        }
        m_tree->SetAdapter(m_treeAdapter.Get());
        ui::FlattenedTreeAdapter* flat = m_tree->FlatAdapter();
        for (usize i = 0; i < m_groups.Size(); ++i)
        {
            if (!m_groups[i].children.IsEmpty())
            {
                flat->Expand(static_cast<i32>(i));
            }
        }
        RebuildList();
    }

    void AssetPickerDialog::RebuildList()
    {
        m_rows.Clear();
        if (m_context->Project() != nullptr)
        {
            // Favorites first (matching type, any group), then the scoped/filtered rest.
            for (const Guid& id : m_context->Favorites())
            {
                content::Instance* instance = m_context->Project()->SourceDb().GetInstance(id);
                if (instance != nullptr && TypeMatches(*instance) &&
                    MatchesFilter(instance->Name(), m_filter.AsView()))
                {
                    m_rows.PushBack(id);
                }
            }
            if (m_filter.IsEmpty())
            {
                if (m_selectedGroup != nullptr)
                {
                    CollectGroup(*m_selectedGroup, false);
                }
            }
            else
            {
                CollectFiltered(m_context->Project()->SourceDb().RootGroup());
            }
        }
        m_list->Selection.ClearSelection();
        m_list->NotifyDataChanged();
    }

    void AssetPickerDialog::CollectGroup(content::Group& group, bool recurse)
    {
        for (content::Instance* instance : group.Instances())
        {
            if (!TypeMatches(*instance) || m_context->IsFavorite(instance->Id()))
            {
                continue;
            }
            m_rows.PushBack(instance->Id());
        }
        if (recurse)
        {
            for (content::Group* child : group.Groups())
            {
                CollectGroup(*child, true);
            }
        }
    }

    void AssetPickerDialog::CollectFiltered(content::Group* group)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* instance : group->Instances())
        {
            if (!TypeMatches(*instance) || m_context->IsFavorite(instance->Id()))
            {
                continue;
            }
            if (MatchesFilter(instance->Name(), m_filter.AsView()))
            {
                m_rows.PushBack(instance->Id());
            }
        }
        for (content::Group* child : group->Groups())
        {
            CollectFiltered(child);
        }
    }

    bool AssetPickerDialog::MatchesFilter(StringView name, StringView filter)
    {
        if (filter.IsEmpty())
        {
            return true;
        }
        if (name.Size() < filter.Size())
        {
            return false;
        }
        auto lower = [](utf8char c)
        { return (c >= utf8char('A') && c <= utf8char('Z')) ? static_cast<utf8char>(c + 32) : c; };
        for (usize i = 0; i + filter.Size() <= name.Size(); ++i)
        {
            bool match = true;
            for (usize j = 0; j < filter.Size(); ++j)
            {
                if (lower(name[i + j]) != lower(filter[j]))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return true;
            }
        }
        return false;
    }

    content::Instance* AssetPickerDialog::Resolve(const Guid& id)
    {
        return (m_context->Project() != nullptr) ? m_context->Project()->SourceDb().GetInstance(id)
                                                 : nullptr;
    }

    void AssetPickerDialog::ConfirmAt(i32 position)
    {
        if (position < 0 || position >= static_cast<i32>(m_rows.Size()))
        {
            return;
        }
        const Guid id = m_rows[static_cast<usize>(position)];
        if (OnPicked)
        {
            OnPicked(id);
        }
        Close(ui::DialogResult::OK);
    }

    void AssetPickerDialog::ShowRowMenu(i32 position, f32 x, f32 y)
    {
        if (position < 0 || position >= static_cast<i32>(m_rows.Size()) || Context == nullptr)
        {
            return;
        }
        const Guid id = m_rows[static_cast<usize>(position)];
        AssetPickerDialog* self = this;
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
        menu->AddItem(m_context->IsFavorite(id) ? StringView(u8"Unpin favorite")
                                                : StringView(u8"Pin favorite"),
                      [self, id]()
                      {
                          self->m_context->ToggleFavorite(id);
                          self->RebuildList();
                      });
        const Float2 screenPos = m_list->LocalToScreen(Float2{x, y});
        menu->Show(Context, screenPos.x, screenPos.y);
    }
}
