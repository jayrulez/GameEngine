// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_picker_slot partition
//
// The asset slot as the kit draws it: the referenced asset's thumbnail (or its type glyph, tinted
// by category, until it has one), its name, and its verbs: pick (the target button; a click on the
// name also picks), open, reveal (in the assets browser) and clear. An empty slot says "None" and
// keeps only pick. A verb shows only when its owner wired it; open, reveal and clear also need
// something referenced. The slot is a drop target for assets of the types it accepts, and gives
// the reason when it refuses one. A drop and a pick make the same assignment.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:asset_picker_slot;

import foundation.core;
import foundation.gui;
import :asset_drag_data;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class AssetPickerSlot : public gui::UIWidget
    {
        RTTI_OBJECT(AssetPickerSlot, gui::UIWidget)
    public:
        AssetPickerSlot()
        {
            SetTag(u8"assetslot");
            AddClass(u8"slot");
            m_glyph = MakeRef<gui::Image>(MemoryAllocator());
            m_glyph->AddClass(u8"slot-glyph");
            m_glyph->SetHitTestVisible(false);
            m_name = MakeRef<gui::Label>(MemoryAllocator());
            m_name->AddClass(u8"slot-name");
            m_name->SetHitTestVisible(false); // a click on it goes to the slot, which picks
            AddChild(m_glyph.Get());
            AddChild(m_name.Get());
            AssetPickerSlot* self = this;
            m_pick = Verb(u8"Editor/Icons/target.svg", u8"Pick (or drop an asset here)", [self]() { self->Pick(); });
            m_open = Verb(u8"Editor/Icons/open.svg", u8"Open", [self]()
                          {
                              if (self->m_onOpen)
                                  self->m_onOpen();
                          });
            m_reveal = Verb(u8"Editor/Icons/folder-open.svg", u8"Show in Assets", [self]()
                            {
                                if (self->m_onReveal)
                                    self->m_onReveal();
                            });
            m_clear = Verb(u8"Editor/Icons/close.svg", u8"Clear", [self]()
                           {
                               if (self->m_onClear)
                                   self->m_onClear();
                           });
            SetValue({});
        }

        // The verbs; each shows once it is wired.
        void SetOnPick(Function<void()> pick) { m_onPick = foundation::core::Move(pick); Show(); }
        void SetOnOpen(Function<void()> open) { m_onOpen = foundation::core::Move(open); Show(); }
        void SetOnReveal(Function<void()> reveal) { m_onReveal = foundation::core::Move(reveal); Show(); }
        void SetOnClear(Function<void()> clear) { m_onClear = foundation::core::Move(clear); Show(); }
        /// Called when an asset of an accepted type is dropped on the slot: assign it, as a pick
        /// would.
        void SetOnDropped(Function<void(const Guid&)> dropped) { m_onDropped = foundation::core::Move(dropped); }

        /// The asset types it accepts, by name; empty accepts any.
        void SetAcceptedTypes(Array<String> types) { m_types = foundation::core::Move(types); }
        [[nodiscard]] Span<const String> AcceptedTypes() const noexcept { return m_types.AsSpan(); }

        /// What it references: the asset's name, or nothing when `name` is empty.
        void SetValue(StringView name)
        {
            m_hasValue = !name.IsEmpty();
            m_name->SetText(m_hasValue ? name : StringView(u8"Not set"));
            Show();
        }
        [[nodiscard]] bool HasValue() const noexcept { return m_hasValue; }
        [[nodiscard]] StringView ShownName() const { return m_name->GetText(); }
        /// The referenced asset type's glyph and its category's class (a tint); empty for none.
        void SetGlyph(StringView path, StringView categoryClass = {})
        {
            if (!m_glyphClass.IsEmpty())
                m_glyph->RemoveClass(m_glyphClass.AsView());
            m_glyphClass = String(categoryClass);
            if (!m_glyphClass.IsEmpty())
                m_glyph->AddClass(m_glyphClass.AsView());
            m_glyphSource = String(path);
            if (!m_picture)
                m_glyph->SetSource(m_glyphSource);
            m_hasGlyph = !path.IsEmpty();
            Show();
        }
        /// The referenced asset's thumbnail, drawn in the glyph's place; null shows the glyph.
        void SetPicture(RefPtr<gui::Drawable> picture)
        {
            if (picture.Get() == m_picture.Get())
                return;
            m_picture = foundation::core::Move(picture);
            if (m_picture)
            {
                m_glyph->SetDrawable(m_picture);
                m_glyph->AddClass(u8"slot-picture");
            }
            else
            {
                m_glyph->RemoveClass(u8"slot-picture");
                m_glyph->SetSource(m_glyphSource); // back to the type's glyph
            }
            Show();
        }
        /// The thumbnail it shows, or null.
        [[nodiscard]] gui::Drawable* Picture() const noexcept { return m_picture.Get(); }

        /// Picks, as its target button does.
        void Pick()
        {
            if (m_onPick)
                m_onPick();
        }
        [[nodiscard]] gui::Button* PickButton() const noexcept { return m_pick.Get(); }
        [[nodiscard]] gui::Button* OpenButton() const noexcept { return m_open.Get(); }
        [[nodiscard]] gui::Button* RevealButton() const noexcept { return m_reveal.Get(); }
        [[nodiscard]] gui::Button* ClearButton() const noexcept { return m_clear.Get(); }

    protected:
        void OnMouseClick(const gui::MouseEvent& event) override
        {
            if (event.Button != gui::MouseButton::Left || !m_onPick)
                return;
            event.Handled = true;
            Pick();
        }
        void OnDragQuery(gui::DropQuery& query) override
        {
            AssetDrag dragged;
            String why;
            if (!m_onDropped)
                return;
            if (AssetDrag::Takes(query.Payload, m_types.AsSpan(), dragged, why))
            {
                query.Accept(gui::DropEffect::Link);
                query.Reason = Format(u8"Use {}", dragged.Name);
            }
            else if (dragged.Id != Guid{}) // an asset of a type it does not accept; another kind of payload is not ours to refuse
                query.Refuse(why.AsView());
        }
        void OnDrop(const gui::DragEvent& event) override
        {
            AssetDrag dropped;
            String why;
            if (m_onDropped && AssetDrag::Takes(event.Payload, m_types.AsSpan(), dropped, why))
                m_onDropped(dropped.Id);
        }

    private:
        RefPtr<gui::Button> Verb(StringView icon, StringView tooltip, Function<void()> click)
        {
            RefPtr<gui::Button> button = MakeRef<gui::Button>(MemoryAllocator());
            button->AddClass(u8"slot-button");
            button->SetIconSource(String(icon));
            button->SetTooltip(tooltip);
            button->SetOnClick(foundation::core::Move(click));
            AddChild(button.Get());
            return button;
        }
        // Shows the wired verbs, and the empty look when nothing is referenced.
        void Show()
        {
            if (m_hasValue)
                RemoveClass(u8"empty");
            else
                AddClass(u8"empty");
            m_glyph->SetDisplayed(m_hasValue && (m_hasGlyph || m_picture));
            m_pick->SetDisplayed(static_cast<bool>(m_onPick));
            m_open->SetDisplayed(static_cast<bool>(m_onOpen) && m_hasValue);
            m_reveal->SetDisplayed(static_cast<bool>(m_onReveal) && m_hasValue);
            m_clear->SetDisplayed(static_cast<bool>(m_onClear) && m_hasValue);
        }

        RefPtr<gui::Image> m_glyph;
        RefPtr<gui::Label> m_name;
        RefPtr<gui::Button> m_pick;
        RefPtr<gui::Button> m_open;
        RefPtr<gui::Button> m_reveal;
        RefPtr<gui::Button> m_clear;
        Function<void()> m_onPick;
        Function<void()> m_onOpen;
        Function<void()> m_onReveal;
        Function<void()> m_onClear;
        Function<void(const Guid&)> m_onDropped;
        Array<String> m_types;
        String m_glyphClass;
        String m_glyphSource;
        RefPtr<gui::Drawable> m_picture; // SetPicture: shown in the glyph's place
        bool m_hasValue = false;
        bool m_hasGlyph = false;
    };

    RTTI_DEFINE_OBJECT(AssetPickerSlot, "rtti::editor::editor::app")
}
