// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUISandbox - the Toolkit page's property source (T1 of Specs/gui-lab-evolution-3.md): three lamps,
// an application's own objects, behind a selection of one or all of them, with the undo an
// application keeps. A commit is one entry, whatever the gesture previewed; a lamp's spot angle
// shows only on a spot, so changing the type tells the grid its properties changed; the falloff is
// hinted "curve", which no T1 editor takes. Until T3's reflected source, the page's model of one.

module;
#include "Core/Prelude.h"

export module gui.sandbox.lamps;

import foundation.core;
import experimental.gui;
import experimental.gui.toolkit;

using namespace foundation::core;
namespace core = foundation::core;
namespace gui = experimental::gui;

export namespace sandbox
{
    class LampSource : public gui::IPropertySource
    {
    public:
        static constexpr usize kLamps = 3;

        explicit LampSource(IAllocator& allocator)
            : m_allocator(allocator), m_selection(allocator), m_originals(allocator), m_undo(allocator), m_redo(allocator)
        {
            const char8_t* const names[kLamps] = {u8"Lamp A", u8"Lamp B", u8"Lamp C"};
            for (usize i = 0; i < kLamps; ++i)
            {
                Lamp& lamp = m_lamps[i];
                lamp.Name = String(names[i]);
                lamp.Id = 101 + static_cast<i64>(i);
                lamp.Position[0] = static_cast<f64>(i) * 2.0;
                lamp.Position[1] = 3.0;
            }
            // The second differs in intensity, type and one shadow flag, so all three show them mixed.
            m_lamps[1].Intensity = 12.5;
            m_lamps[1].Type = 1;
            m_lamps[1].Shadows = 0b011;
            m_lamps[2].Enabled = false;
            SetSelection(0);
        }

        /// Edits lamp `lamp`, or all of them with kLamps.
        void SetSelection(usize lamp)
        {
            m_selection.Clear();
            for (usize i = 0; i < kLamps; ++i)
                if (lamp >= kLamps || lamp == i)
                    m_selection.PushBack(i);
            NotifyPropertiesChanged();
        }

        [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.IsEmpty(); }
        [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.IsEmpty(); }
        void Undo() { Step(m_undo, m_redo, true); }
        void Redo() { Step(m_redo, m_undo, false); }
        /// Told when the history changes (to enable Undo and Redo) and what changed, in words.
        void SetOnHistoryChanged(Function<void(StringView)> changed) { m_onHistory = core::Move(changed); }

        // === gui::IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return m_selection.Size(); }
        void GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const override
        {
            using K = gui::PropertyKind;
            if (parent == StringView(u8"position"))
            {
                Add(out, u8"x", K::Float).Step = 0.1;
                Add(out, u8"y", K::Float).Step = 0.1;
                Add(out, u8"z", K::Float).Step = 0.1;
                return;
            }
            if (parent.Size() != 0)
                return;
            Add(out, u8"name", K::String).Description = String(u8"What the scene calls it");
            gui::PropertyInfo& id = Add(out, u8"id", K::Integer);
            id.DisplayName = String(u8"Id");
            id.ReadOnly = true;
            id.Description = String(u8"Given by the scene: read only");
            Add(out, u8"enabled", K::Bool, u8"Light");
            gui::PropertyInfo& type = Add(out, u8"type", K::Enum, u8"Light");
            type.Choices.PushBack(gui::PropertyChoice{String(u8"Point"), 0});
            type.Choices.PushBack(gui::PropertyChoice{String(u8"Spot"), 1});
            type.Choices.PushBack(gui::PropertyChoice{String(u8"Area"), 2});
            gui::PropertyInfo& intensity = Add(out, u8"intensity", K::Float, u8"Light");
            intensity.HasRange = true;
            intensity.Min = 0.0;
            intensity.Max = 100.0;
            intensity.Step = 0.5;
            intensity.Description = String(u8"Drag across it to scrub; Escape gives the scrub up");
            gui::PropertyInfo& range = Add(out, u8"range", K::Float, u8"Light");
            range.Step = 0.25;
            gui::PropertyInfo& angle = Add(out, u8"spotAngle", K::Float, u8"Light");
            angle.DisplayName = String(u8"Spot angle");
            angle.HasRange = true;
            angle.Min = 1.0;
            angle.Max = 179.0;
            angle.Visible = AllSpots();
            angle.Description = String(u8"Only a spot has one");
            gui::PropertyInfo& shadows = Add(out, u8"shadows", K::Flags, u8"Light");
            shadows.Choices.PushBack(gui::PropertyChoice{String(u8"Cast"), 1});
            shadows.Choices.PushBack(gui::PropertyChoice{String(u8"Receive"), 2});
            shadows.Choices.PushBack(gui::PropertyChoice{String(u8"Static"), 4});
            Add(out, u8"position", K::Group, u8"Transform");
            Add(out, u8"bounces", K::Integer, u8"Advanced").Description = String(u8"An integer: no range, snapped to whole numbers");
            gui::PropertyInfo& falloff = Add(out, u8"falloff", K::Group, u8"Advanced");
            falloff.Hint = String(u8"curve"); // a curve is no value of T1's: no editor takes it
            falloff.Description = String(u8"A hint no editor takes yet");
        }
        [[nodiscard]] gui::Variant GetValue(StringView path, usize object) const override
        {
            return object < m_selection.Size() ? Read(m_lamps[m_selection[object]], path) : gui::Variant{};
        }

        void BeginEdit(StringView path) override
        {
            Original original{String(path), Array<gui::Variant>(m_allocator)};
            for (const usize lamp : m_selection)
                original.Values.PushBack(Read(m_lamps[lamp], path));
            m_originals.PushBack(core::Move(original));
        }
        void PreviewEdit(StringView path, const gui::PropertyWrite& write) override
        {
            for (const usize lamp : m_selection)
                Write(m_lamps[lamp], path, gui::ApplyPropertyWrite(Read(m_lamps[lamp], path), write));
            Changed(path);
        }
        void CommitEdit(StringView path, const gui::PropertyWrite& write) override
        {
            Entry entry{String(path), m_selection, Array<gui::Variant>(m_allocator), Array<gui::Variant>(m_allocator)};
            const Original* original = FindOriginal(path);
            for (usize i = 0; i < m_selection.Size(); ++i)
            {
                Lamp& lamp = m_lamps[m_selection[i]];
                const gui::Variant before = original != nullptr && i < original->Values.Size() ? original->Values[i] : Read(lamp, path);
                entry.Before.PushBack(before);
                Write(lamp, path, gui::ApplyPropertyWrite(before, write));
                entry.After.PushBack(Read(lamp, path));
            }
            DropOriginal(path);
            m_undo.PushBack(core::Move(entry));
            m_redo.Clear();
            Changed(path);
            if (m_onHistory)
                m_onHistory(Format(u8"Set {}", path).AsView());
        }
        bool EditList(StringView, const gui::ListEdit&) override { return false; } // the lamps have no lists yet
        [[nodiscard]] gui::Variant GetDefault(StringView, usize) const override { return gui::Variant{}; } // nor defaults
        bool Revert(StringView) override { return false; }
        [[nodiscard]] String GetRevertText(StringView) const override { return String(u8"Revert to default"); }
        void CancelEdit(StringView path) override
        {
            if (const Original* original = FindOriginal(path))
                for (usize i = 0; i < m_selection.Size() && i < original->Values.Size(); ++i)
                    Write(m_lamps[m_selection[i]], path, original->Values[i]);
            DropOriginal(path);
            Changed(path);
        }

    private:
        struct Lamp
        {
            String Name;
            i64 Id = 0;
            bool Enabled = true;
            i64 Type = 0;
            f64 Intensity = 5.0;
            f64 Range = 10.0;
            f64 SpotAngle = 45.0;
            i64 Shadows = 0b001;
            f64 Position[3] = {0.0, 0.0, 0.0};
            i64 Bounces = 2;
        };
        struct Original
        {
            String Path;
            Array<gui::Variant> Values;
        };
        struct Entry
        {
            String Path;
            Array<usize> Lamps;
            Array<gui::Variant> Before;
            Array<gui::Variant> After;
        };

        static gui::PropertyInfo& Add(Array<gui::PropertyInfo>& out, const char8_t* name, gui::PropertyKind kind, const char8_t* category = u8"")
        {
            gui::PropertyInfo info;
            info.Name = String(name);
            info.DisplayName = String(name);
            if (info.DisplayName.Size() != 0 && info.DisplayName[0] >= u8'a' && info.DisplayName[0] <= u8'z')
                info.DisplayName[0] = static_cast<char8_t>(info.DisplayName[0] - u8'a' + u8'A'); // as a reflected name's display name reads
            info.Kind = kind;
            info.Category = String(category);
            out.PushBack(core::Move(info));
            return out.Back();
        }
        [[nodiscard]] bool AllSpots() const
        {
            for (const usize lamp : m_selection)
                if (m_lamps[lamp].Type != 1)
                    return false;
            return !m_selection.IsEmpty();
        }
        [[nodiscard]] static gui::Variant Read(const Lamp& lamp, StringView path)
        {
            if (path == StringView(u8"name"))
                return gui::Variant(lamp.Name.AsView());
            if (path == StringView(u8"id"))
                return gui::Variant(lamp.Id);
            if (path == StringView(u8"enabled"))
                return gui::Variant(lamp.Enabled);
            if (path == StringView(u8"type"))
                return gui::Variant(lamp.Type);
            if (path == StringView(u8"intensity"))
                return gui::Variant(lamp.Intensity);
            if (path == StringView(u8"range"))
                return gui::Variant(lamp.Range);
            if (path == StringView(u8"spotAngle"))
                return gui::Variant(lamp.SpotAngle);
            if (path == StringView(u8"shadows"))
                return gui::Variant(lamp.Shadows);
            if (path == StringView(u8"bounces"))
                return gui::Variant(lamp.Bounces);
            if (path == StringView(u8"position.x"))
                return gui::Variant(lamp.Position[0]);
            if (path == StringView(u8"position.y"))
                return gui::Variant(lamp.Position[1]);
            if (path == StringView(u8"position.z"))
                return gui::Variant(lamp.Position[2]);
            return gui::Variant{};
        }
        static void Write(Lamp& lamp, StringView path, const gui::Variant& value)
        {
            if (path == StringView(u8"name"))
                lamp.Name = String(value.AsString());
            else if (path == StringView(u8"enabled"))
                lamp.Enabled = value.AsBool();
            else if (path == StringView(u8"type"))
                lamp.Type = value.AsInt();
            else if (path == StringView(u8"intensity"))
                lamp.Intensity = value.AsFloat();
            else if (path == StringView(u8"range"))
                lamp.Range = value.AsFloat();
            else if (path == StringView(u8"spotAngle"))
                lamp.SpotAngle = value.AsFloat();
            else if (path == StringView(u8"shadows"))
                lamp.Shadows = value.AsInt();
            else if (path == StringView(u8"bounces"))
                lamp.Bounces = value.AsInt();
            else if (path == StringView(u8"position.x"))
                lamp.Position[0] = value.AsFloat();
            else if (path == StringView(u8"position.y"))
                lamp.Position[1] = value.AsFloat();
            else if (path == StringView(u8"position.z"))
                lamp.Position[2] = value.AsFloat();
        }
        // A type's change shows or hides the spot angle: the properties changed, not only values.
        void Changed(StringView path)
        {
            if (path == StringView(u8"type"))
                NotifyPropertiesChanged();
            else
                NotifyValuesChanged();
        }
        const Original* FindOriginal(StringView path) const
        {
            for (const Original& original : m_originals)
                if (original.Path.AsView() == path)
                    return &original;
            return nullptr;
        }
        void DropOriginal(StringView path)
        {
            for (usize i = 0; i < m_originals.Size(); ++i)
                if (m_originals[i].Path.AsView() == path)
                {
                    m_originals.RemoveAt(i);
                    return;
                }
        }
        void Step(Array<Entry>& from, Array<Entry>& to, bool undo)
        {
            if (from.IsEmpty())
                return;
            Entry entry = core::Move(from.Back());
            from.PopBack();
            for (usize i = 0; i < entry.Lamps.Size(); ++i)
                Write(m_lamps[entry.Lamps[i]], entry.Path.AsView(), undo ? entry.Before[i] : entry.After[i]);
            const String words = undo ? Format(u8"Undid {}", entry.Path.AsView()) : Format(u8"Redid {}", entry.Path.AsView());
            Changed(entry.Path.AsView());
            to.PushBack(core::Move(entry));
            if (m_onHistory)
                m_onHistory(words.AsView());
        }

        IAllocator& m_allocator;
        Lamp m_lamps[kLamps];
        Array<usize> m_selection;
        Array<Original> m_originals;
        Array<Entry> m_undo;
        Array<Entry> m_redo;
        Function<void(StringView)> m_onHistory;
    };
}
