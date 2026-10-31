// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :bus_layout_model partition.
//
// The rules the bus layout page edits by, kept apart from the page. There are four fixed buses
// (Master, with Effects, Music and UI under it), and a custom bus is a named slot under a fixed bus
// or another custom one. These functions build the tree the buses make and perform the edits that
// keep it a tree: add a bus in the first empty slot, remove one (its children move to Master),
// rename one (its children follow the name), and move one under another (never under itself or its
// own descendants, since a cycle fails the cook). They are pure functions over the asset, shared by
// the page's edits and its tests.

module;
#include "Core/Prelude.h"

export module editor.audio:bus_layout_model;

import foundation.core;
import audio.pipeline;

using namespace foundation::core;

export namespace editor
{
    /// The fixed buses, in the tree's order: Master first, the others under it.
    inline constexpr StringView kFixedBusNames[] = {u8"Master", u8"Effects", u8"Music", u8"UI"};
    inline constexpr usize kFixedBusCount = sizeof(kFixedBusNames) / sizeof(kFixedBusNames[0]);

    /// One bus of the layout: a fixed one by its index (0 Master, 1 Effects, 2 Music, 3 UI) or a
    /// custom one by its slot.
    struct BusRef
    {
        i32 fixed = -1;
        i32 slot = -1;
        [[nodiscard]] bool IsFixed() const noexcept { return fixed >= 0; }
        [[nodiscard]] bool IsValid() const noexcept { return fixed >= 0 || slot >= 0; }
        [[nodiscard]] bool operator==(const BusRef& other) const noexcept { return fixed == other.fixed && slot == other.slot; }
    };

    /// A row of the bus tree: which bus, how deep, in the order the tree shows them.
    struct BusRow
    {
        BusRef bus;
        i32 depth = 0;
        String name;
    };

    /// Whether two bus names are one, ignoring ASCII case (the cook takes a fixed bus's name in
    /// any case: "music" is Music).
    [[nodiscard]] inline bool BusNamesMatch(StringView a, StringView b)
    {
        if (a.Size() != b.Size())
            return false;
        for (usize i = 0; i < a.Size(); ++i)
        {
            utf8char x = a[i];
            utf8char y = b[i];
            if (x >= u8'A' && x <= u8'Z')
                x = static_cast<utf8char>(x + (u8'a' - u8'A'));
            if (y >= u8'A' && y <= u8'Z')
                y = static_cast<utf8char>(y + (u8'a' - u8'A'));
            if (x != y)
                return false;
        }
        return true;
    }

    [[nodiscard]] inline bool IsFixedBusName(StringView name)
    {
        for (const StringView fixed : kFixedBusNames)
            if (BusNamesMatch(name, fixed))
                return true;
        return false;
    }

    /// The custom slot named `name`, or -1 for a fixed name, an unknown one or an empty one.
    [[nodiscard]] inline i32 FindBusSlot(const pipeline::AudioBusLayoutAsset& asset, StringView name)
    {
        if (name.IsEmpty() || IsFixedBusName(name))
            return -1;
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            if (!asset.custom[i].name.IsEmpty() && asset.custom[i].name.AsView() == name)
                return static_cast<i32>(i);
        return -1;
    }

    /// True when moving custom slot `slot` under `parent` would make a cycle among the custom
    /// buses (a fixed parent never can).
    [[nodiscard]] inline bool AudioBusWouldCycle(const pipeline::AudioBusLayoutAsset& asset, i32 slot, StringView parent)
    {
        if (slot < 0 || static_cast<usize>(slot) >= pipeline::kAudioCustomBusSlotCount)
            return false;
        i32 walk = FindBusSlot(asset, parent);
        for (usize guard = 0; walk >= 0 && guard <= pipeline::kAudioCustomBusSlotCount; ++guard)
        {
            if (walk == slot)
                return true;
            walk = FindBusSlot(asset, asset.custom[static_cast<usize>(walk)].parent.AsView());
        }
        return false;
    }

    /// A bus's name as the tree and the parents show it.
    [[nodiscard]] inline StringView BusName(const pipeline::AudioBusLayoutAsset& asset, const BusRef& bus)
    {
        if (bus.IsFixed())
            return static_cast<usize>(bus.fixed) < kFixedBusCount ? kFixedBusNames[bus.fixed] : StringView{};
        return bus.slot >= 0 && static_cast<usize>(bus.slot) < pipeline::kAudioCustomBusSlotCount ? asset.custom[bus.slot].name.AsView() : StringView{};
    }

    /// The layout's settings for `bus` (null when there is no such bus).
    [[nodiscard]] inline pipeline::AudioBusLayoutAsset::Bus* BusSettings(pipeline::AudioBusLayoutAsset& asset, const BusRef& bus)
    {
        switch (bus.fixed)
        {
        case 0:
            return &asset.master;
        case 1:
            return &asset.effects;
        case 2:
            return &asset.music;
        case 3:
            return &asset.ui;
        default:
            break;
        }
        if (bus.slot >= 0 && static_cast<usize>(bus.slot) < pipeline::kAudioCustomBusSlotCount && !asset.custom[bus.slot].name.IsEmpty())
            return &asset.custom[bus.slot].bus;
        return nullptr;
    }

    /// The bus tree in order: Master, the fixed buses under it, each bus's custom children under it
    /// (an empty or unknown parent is Master, as the cook takes it).
    [[nodiscard]] inline Array<BusRow> BusTree(const pipeline::AudioBusLayoutAsset& asset)
    {
        Array<BusRow> rows;
        // The custom slots whose parent is `parent` (by name), under it at `depth`.
        const auto children = [&asset, &rows](auto& self, StringView parent, i32 depth) -> void
        {
            for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            {
                const pipeline::AudioBusLayoutAsset::CustomBusSlot& slot = asset.custom[i];
                if (slot.name.IsEmpty())
                    continue;
                const bool underMaster = slot.parent.IsEmpty() || (!IsFixedBusName(slot.parent.AsView()) && FindBusSlot(asset, slot.parent.AsView()) < 0);
                const bool under = BusNamesMatch(parent, kFixedBusNames[0]) ? (underMaster || BusNamesMatch(slot.parent.AsView(), parent))
                                                                               : (IsFixedBusName(parent) ? BusNamesMatch(slot.parent.AsView(), parent) : slot.parent.AsView() == parent);
                if (!under)
                    continue;
                rows.PushBack(BusRow{BusRef{-1, static_cast<i32>(i)}, depth, String(slot.name.AsView())});
                self(self, slot.name.AsView(), depth + 1);
            }
        };
        rows.PushBack(BusRow{BusRef{0, -1}, 0, String(kFixedBusNames[0])});
        for (usize f = 1; f < kFixedBusCount; ++f)
        {
            rows.PushBack(BusRow{BusRef{static_cast<i32>(f), -1}, 1, String(kFixedBusNames[f])});
            children(children, kFixedBusNames[f], 2);
        }
        children(children, kFixedBusNames[0], 1);
        // Buses in a cycle (each under another of them) cannot be reached from a fixed bus. They are
        // still shown under Master so they can be moved out of the cycle (the cook fails until they are).
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
        {
            if (asset.custom[i].name.IsEmpty())
                continue;
            bool shown = false;
            for (const BusRow& row : rows)
                shown = shown || row.bus.slot == static_cast<i32>(i);
            if (!shown)
                rows.PushBack(BusRow{BusRef{-1, static_cast<i32>(i)}, 1, String(asset.custom[i].name.AsView())});
        }
        return rows;
    }

    /// Whether some custom buses are under each other in a cycle (the cook fails on it).
    [[nodiscard]] inline bool HasBusCycle(const pipeline::AudioBusLayoutAsset& asset)
    {
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            if (!asset.custom[i].name.IsEmpty() && AudioBusWouldCycle(asset, static_cast<i32>(i), asset.custom[i].parent.AsView()))
                return true;
        return false;
    }

    /// The parents custom slot `slot` may move under: the fixed buses, then the other custom ones
    /// that would not make a cycle.
    [[nodiscard]] inline Array<String> BusParentChoices(const pipeline::AudioBusLayoutAsset& asset, i32 slot)
    {
        Array<String> choices;
        for (const StringView fixed : kFixedBusNames)
            choices.PushBack(String(fixed));
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            if (static_cast<i32>(i) != slot && !asset.custom[i].name.IsEmpty() && !AudioBusWouldCycle(asset, slot, asset.custom[i].name.AsView()))
                choices.PushBack(String(asset.custom[i].name.AsView()));
        return choices;
    }

    /// Adds a custom bus under Master in the first empty slot, named "Bus N" with the first free N.
    /// Returns its slot, or -1 when every slot is in use.
    inline i32 AddBus(pipeline::AudioBusLayoutAsset& asset)
    {
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
        {
            if (!asset.custom[i].name.IsEmpty())
                continue;
            String name;
            for (usize n = 1;; ++n)
            {
                name = Format(u8"Bus {}", n);
                if (FindBusSlot(asset, name.AsView()) < 0)
                    break;
            }
            asset.custom[i].name = Move(name);
            asset.custom[i].parent = String(kFixedBusNames[0]);
            asset.custom[i].bus = pipeline::AudioBusLayoutAsset::Bus{};
            return static_cast<i32>(i);
        }
        return -1;
    }

    /// Removes custom slot `slot`; its children move to Master.
    inline void RemoveBus(pipeline::AudioBusLayoutAsset& asset, i32 slot)
    {
        if (slot < 0 || static_cast<usize>(slot) >= pipeline::kAudioCustomBusSlotCount || asset.custom[slot].name.IsEmpty())
            return;
        const String name(asset.custom[slot].name.AsView());
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            if (!asset.custom[i].name.IsEmpty() && asset.custom[i].parent.AsView() == name.AsView())
                asset.custom[i].parent = String(kFixedBusNames[0]);
        asset.custom[slot] = pipeline::AudioBusLayoutAsset::CustomBusSlot{};
    }

    /// Renames custom slot `slot`, and its children follow the name. False (nothing changed) for an
    /// empty name, a fixed bus's name or another bus's name.
    inline bool RenameBus(pipeline::AudioBusLayoutAsset& asset, i32 slot, StringView name)
    {
        if (slot < 0 || static_cast<usize>(slot) >= pipeline::kAudioCustomBusSlotCount || asset.custom[slot].name.IsEmpty())
            return false;
        if (name.IsEmpty() || IsFixedBusName(name))
            return false;
        const i32 owner = FindBusSlot(asset, name);
        if (owner >= 0 && owner != slot)
            return false;
        const String old(asset.custom[slot].name.AsView());
        for (usize i = 0; i < pipeline::kAudioCustomBusSlotCount; ++i)
            if (!asset.custom[i].name.IsEmpty() && asset.custom[i].parent.AsView() == old.AsView())
                asset.custom[i].parent = String(name);
        asset.custom[slot].name = String(name);
        return true;
    }

    /// Moves custom slot `slot` under `parent`; false (nothing changed) when that would make a cycle
    /// or names no bus.
    inline bool ReparentBus(pipeline::AudioBusLayoutAsset& asset, i32 slot, StringView parent)
    {
        if (slot < 0 || static_cast<usize>(slot) >= pipeline::kAudioCustomBusSlotCount || asset.custom[slot].name.IsEmpty())
            return false;
        if (!IsFixedBusName(parent) && (FindBusSlot(asset, parent) < 0 || FindBusSlot(asset, parent) == slot))
            return false;
        if (AudioBusWouldCycle(asset, slot, parent))
            return false;
        asset.custom[slot].parent = String(parent);
        return true;
    }
}
