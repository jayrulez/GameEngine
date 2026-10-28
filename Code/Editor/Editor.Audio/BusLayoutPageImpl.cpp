// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the bus layout page's implementation (see BusLayoutPage.cppm).

module;
#include "Core/Prelude.h"

module editor.audio;

import :bus_layout_page;
import :bus_layout_model;

import foundation.core;
import foundation.content;
import foundation.audio;
import audio.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        using Bus = pipeline::AudioBusLayoutAsset::Bus;

        // A bus's setting as a row: its path, its name, its group in the grid, its range (a float's),
        // and where it is in a Bus.
        struct EffectRow
        {
            const char8_t* path;
            const char8_t* shown;
            const char8_t* category;
            const char8_t* description;
            f64 min;
            f64 max;
            f64 step;
            f32 Bus::*number;
            bool Bus::*flag;
        };
        constexpr EffectRow kEffects[] = {
            {u8"volume", u8"Volume", u8"Mix", u8"", 0.0, 2.0, 0.01, &Bus::volume, nullptr},
            {u8"muted", u8"Muted", u8"Mix", u8"", 0.0, 0.0, 0.0, nullptr, &Bus::muted},
            {u8"lowpassHz", u8"Lowpass (Hz)", u8"Filters", u8"Cuts what is above it; 0 is off", 0.0, 22000.0, 10.0, &Bus::lowpassHz, nullptr},
            {u8"highpassHz", u8"Highpass (Hz)", u8"Filters", u8"Cuts what is below it; 0 is off", 0.0, 22000.0, 10.0, &Bus::highpassHz, nullptr},
            {u8"delaySeconds", u8"Delay (s)", u8"Delay", u8"The echo's delay; 0 is off", 0.0, 2.0, 0.01, &Bus::delaySeconds, nullptr},
            {u8"delayDecay", u8"Decay", u8"Delay", u8"How much of each echo comes back", 0.0, 1.0, 0.01, &Bus::delayDecay, nullptr},
            {u8"reverbWet", u8"Wet", u8"Reverb", u8"How much reverb is heard; 0 is off", 0.0, 1.0, 0.01, &Bus::reverbWet, nullptr},
            {u8"reverbRoomSize", u8"Room Size", u8"Reverb", u8"", 0.0, 1.0, 0.01, &Bus::reverbRoomSize, nullptr},
            {u8"reverbDamping", u8"Damping", u8"Reverb", u8"How fast its high end fades", 0.0, 1.0, 0.01, &Bus::reverbDamping, nullptr},
        };
        // The settings' groups, each a section of the grid, in order.
        constexpr StringView kGroups[] = {u8"Mix", u8"Filters", u8"Delay", u8"Reverb"};
        [[nodiscard]] usize GroupOf(StringView category)
        {
            for (usize g = 0; g < 4; ++g)
                if (kGroups[g] == category)
                    return g;
            return 0;
        }
    }

    AudioBusLayoutEditorPage::AudioBusLayoutEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::AudioBusLayoutAsset::StaticType(), kPath)
    {
        HideAssetRows(); // the layout reflects nothing of its own: its rows are the chosen bus's
        m_model = MakeUnique<gui::TreeModel>(Allocator());
        m_tree = Find<gui::TreeView>(u8"bus-tree");
        m_remove = Find<gui::ButtonBase>(u8"bus-remove");
        AudioBusLayoutEditorPage* self = this;
        if (auto* add = Find<gui::ButtonBase>(u8"bus-add"))
            add->SetOnClick([self]() { (void)self->AddBus(); });
        if (m_remove != nullptr)
            m_remove->SetOnClick([self]() { (void)self->RemoveSelected(); });
        if (m_tree != nullptr)
        {
            m_tree->SetModel(m_model.Get());
            m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                          {
                                              if (self->m_showing || !index.IsValid())
                                                  return;
                                              const usize row = static_cast<usize>(index.InternalId);
                                              if (row < self->m_rows.Size())
                                                  self->Select(self->m_rows[row].bus);
                                          });
            (void)m_tree->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                           {
                                               const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                               self->ShowMenu(self->m_tree->ItemAt(at), at);
                                               e.Handled = true;
                                           });
        }
        BuildRows();
        ShowTree();
        ShowBus();
    }

    AudioBusLayoutEditorPage::~AudioBusLayoutEditorPage()
    {
        if (m_tree != nullptr)
            m_tree->SetModel(nullptr);
    }

    String AudioBusLayoutEditorPage::Tree() const
    {
        String out;
        for (const BusRow& row : m_rows)
        {
            if (!out.IsEmpty())
                out += u8"\n";
            for (i32 d = 0; d < row.depth; ++d)
                out += u8"  ";
            out += row.name;
        }
        return out;
    }

    gui::PropertyBag& AudioBusLayoutEditorPage::EffectRows(StringView group)
    {
        return *m_effectRows[GroupOf(group)];
    }

    void AudioBusLayoutEditorPage::BuildRows()
    {
        m_busRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        for (UniquePtr<gui::PropertyBag>& group : m_effectRows)
            group = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        gui::PropertyInfo& name = m_busRows->Add(u8"name", gui::PropertyKind::String);
        name.DisplayName = String(u8"Name");
        name.Description = String(u8"Its own: not a fixed bus's nor another's; its children follow it");
        gui::PropertyInfo& parent = m_busRows->Add(u8"parent", gui::PropertyKind::Enum);
        parent.DisplayName = String(u8"Parent");
        parent.Description = String(u8"The bus it mixes into: a fixed one, or a custom one that is not under it");
        for (const EffectRow& effect : kEffects)
        {
            gui::PropertyInfo& info = m_effectRows[GroupOf(effect.category)]->Add(effect.path, effect.flag != nullptr ? gui::PropertyKind::Bool : gui::PropertyKind::Float);
            info.DisplayName = String(effect.shown);
            if (effect.description[0] != 0)
                info.Description = String(effect.description);
            if (effect.number != nullptr)
            {
                info.HasRange = true;
                info.Min = effect.min;
                info.Max = effect.max;
                info.Step = effect.step;
                info.Hint = String(u8"slider");
            }
        }
        AudioBusLayoutEditorPage* self = this;
        gui::PropertyBag* const bags[] = {m_busRows.Get(), m_effectRows[0].Get(), m_effectRows[1].Get(), m_effectRows[2].Get(), m_effectRows[3].Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
        }
        AddSection(u8"bus", u8"Bus", *m_busRows);
        for (usize g = 0; g < 4; ++g)
            AddSection(kGroups[g], kGroups[g], *m_effectRows[g]);
    }

    void AudioBusLayoutEditorPage::ShowTree()
    {
        const pipeline::AudioBusLayoutAsset* layout = Layout();
        if (layout == nullptr)
            return;
        m_rows = BusTree(*layout);
        bool found = false;
        for (const BusRow& row : m_rows)
            found = found || row.bus == m_selected;
        if (!found)
            m_selected = BusRef{0, -1}; // gone (removed, an undo): Master
        m_showing = true;
        m_model->Clear();
        Array<i32> parents; // the last node at each depth
        i32 chosen = -1;
        for (const BusRow& row : m_rows)
        {
            const i32 parent = row.depth > 0 && static_cast<usize>(row.depth - 1) < parents.Size() ? parents[static_cast<usize>(row.depth - 1)] : gui::kTreeModelRoot;
            const i32 node = m_model->AddNode(parent, row.name.AsView());
            m_model->SetIcon(node, u8"Editor/Icons/speaker.svg");
            while (parents.Size() > static_cast<usize>(row.depth))
                parents.PopBack();
            parents.PushBack(node);
            if (row.bus == m_selected)
                chosen = node;
        }
        if (m_tree != nullptr)
        {
            for (usize node = 0; node < m_rows.Size(); ++node)
                m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), true);
            if (chosen >= 0)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(chosen);
                m_tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
        }
        m_showing = false;
        if (m_remove != nullptr)
            m_remove->SetEnabled(!m_selected.IsFixed());
        SetNotice(u8"cycle", HasBusCycle(*layout) ? StringView(u8"Some buses are under each other in a loop: the cook refuses the layout until one moves to a fixed bus.")
                                                  : StringView{});
    }

    void AudioBusLayoutEditorPage::ShowBus()
    {
        pipeline::AudioBusLayoutAsset* layout = Layout();
        if (layout == nullptr)
            return;
        const Bus* bus = BusSettings(*layout, m_selected);
        if (bus == nullptr)
            return;
        // A custom bus's name and parent; a fixed one has neither to change.
        const bool custom = !m_selected.IsFixed();
        gui::PropertyInfo* name = m_busRows->FindInfo(u8"name");
        gui::PropertyInfo* parent = m_busRows->FindInfo(u8"parent");
        name->Visible = custom;
        parent->Visible = custom;
        parent->Choices.Clear();
        if (custom)
        {
            const Array<String> choices = BusParentChoices(*layout, m_selected.slot);
            i64 current = 0;
            for (usize i = 0; i < choices.Size(); ++i)
            {
                parent->Choices.PushBack(gui::PropertyChoice{String(choices[i].AsView()), static_cast<i64>(i)});
                const StringView own = layout->custom[m_selected.slot].parent.AsView();
                if (BusNamesMatch(choices[i].AsView(), own.IsEmpty() ? kFixedBusNames[0] : own))
                    current = static_cast<i64>(i);
            }
            m_busRows->PropertiesChanged();
            m_busRows->SetValue(u8"name", 0, gui::Variant(BusName(*layout, m_selected)));
            m_busRows->SetValue(u8"parent", 0, gui::Variant(current));
        }
        else
            m_busRows->PropertiesChanged();
        for (const EffectRow& effect : kEffects)
            m_effectRows[GroupOf(effect.category)]->SetValue(effect.path, 0, effect.flag != nullptr ? gui::Variant(bus->*effect.flag) : gui::Variant(static_cast<f64>(bus->*effect.number)));
    }

    void AudioBusLayoutEditorPage::Select(const BusRef& bus)
    {
        m_selected = bus;
        ShowTree();
        ShowBus();
    }

    void AudioBusLayoutEditorPage::OnObjectChanged()
    {
        ShowTree();
        ShowBus();
    }

    bool AudioBusLayoutEditorPage::AddBus()
    {
        pipeline::AudioBusLayoutAsset* layout = Layout();
        if (layout == nullptr)
            return false;
        bool room = false;
        for (const pipeline::AudioBusLayoutAsset::CustomBusSlot& slot : layout->custom)
            room = room || slot.name.IsEmpty();
        if (!room)
        {
            Context().SetStatus(u8"Every custom bus slot is in use: remove one to add another.");
            return false;
        }
        i32 added = -1;
        EditWhole(u8"edit_buslayout", [&added](ISerializable& object) { added = editor::AddBus(static_cast<pipeline::AudioBusLayoutAsset&>(object)); });
        if (added >= 0)
            Select(BusRef{-1, added});
        return added >= 0;
    }

    bool AudioBusLayoutEditorPage::RemoveSelected()
    {
        if (Layout() == nullptr || m_selected.IsFixed() || !m_selected.IsValid())
            return false;
        const i32 slot = m_selected.slot;
        m_selected = BusRef{0, -1};
        EditWhole(u8"edit_buslayout", [slot](ISerializable& object) { RemoveBus(static_cast<pipeline::AudioBusLayoutAsset&>(object), slot); });
        return true;
    }

    void AudioBusLayoutEditorPage::EditRow(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        rows.SetValue(path, 0, value); // as the grid's commit leaves it
        Committed(rows, path, value);
    }

    void AudioBusLayoutEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        pipeline::AudioBusLayoutAsset* layout = Layout();
        if (layout == nullptr)
            return;
        const BusRef bus = m_selected;
        if (&rows == m_busRows.Get())
        {
            if (bus.IsFixed())
                return;
            if (path == u8"name")
            {
                const String name(Trim(value.AsString()));
                // Refused (empty, a fixed bus's, another's): the row shows the name it has.
                if (name.IsEmpty() || IsFixedBusName(name.AsView()) || (FindBusSlot(*layout, name.AsView()) >= 0 && FindBusSlot(*layout, name.AsView()) != bus.slot))
                {
                    Context().SetStatus(Format(u8"\"{}\" cannot name this bus: a name is a bus's own, and not a fixed bus's.", name).AsView());
                    ShowBus();
                    return;
                }
                EditWhole(u8"edit_buslayout", [bus, name](ISerializable& object) { (void)RenameBus(static_cast<pipeline::AudioBusLayoutAsset&>(object), bus.slot, name.AsView()); });
            }
            else if (path == u8"parent")
            {
                const Array<String> choices = BusParentChoices(*layout, bus.slot);
                const i64 index = value.AsInt();
                if (index < 0 || static_cast<usize>(index) >= choices.Size())
                    return;
                const String parent(choices[static_cast<usize>(index)].AsView());
                EditWhole(u8"edit_buslayout", [bus, parent](ISerializable& object) { (void)ReparentBus(static_cast<pipeline::AudioBusLayoutAsset&>(object), bus.slot, parent.AsView()); });
            }
            return;
        }
        for (const EffectRow& effect : kEffects)
            if (path == effect.path)
            {
                const bool flag = value.AsBool();
                const f32 number = static_cast<f32>(value.AsFloat());
                const EffectRow* row = &effect;
                EditWhole(u8"edit_buslayout", [bus, row, flag, number](ISerializable& object)
                          {
                              Bus* settings = BusSettings(static_cast<pipeline::AudioBusLayoutAsset&>(object), bus);
                              if (settings == nullptr)
                                  return;
                              if (row->flag != nullptr)
                                  settings->*row->flag = flag;
                              else
                                  settings->*row->number = number;
                          });
                return;
            }
    }

    void AudioBusLayoutEditorPage::ShowMenu(i32 item, Float2 at)
    {
        if (m_tree == nullptr)
            return;
        // A press on a row chooses it first, so Remove acts on it.
        if (item >= 0)
        {
            const gui::ModelIndex index = m_tree->GetItemIndex(item);
            const usize row = static_cast<usize>(index.InternalId);
            if (index.IsValid() && row < m_rows.Size())
                Select(m_rows[row].bus);
        }
        AudioBusLayoutEditorPage* self = this;
        m_menu = MakeRef<gui::Menu>(Allocator());
        (void)m_menu->AddItem(u8"Add Bus", [self]() { (void)self->AddBus(); });
        if (!m_selected.IsFixed())
            (void)m_menu->AddItem(u8"Remove Bus", [self]() { (void)self->RemoveSelected(); });
        m_menu->Open(*m_tree, at);
    }

    const TypeInfo* AudioBusLayoutPageFactory::PrimaryType() const
    {
        return &pipeline::AudioBusLayoutAsset::StaticType();
    }

    UniquePtr<EditorPage> AudioBusLayoutPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<AudioBusLayoutEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
