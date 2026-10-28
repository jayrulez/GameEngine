// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_selector partition
//
// Matches views by type, style class(es), id, control state, structural position and an
// optional pseudo-element name - as a CHAIN of compounds joined by the descendant (` `) and
// child (`>`) combinators. Ported from Sedulous.UI/src/Styling/StyleSelector.bf and grown for
// the style model v2 (ui-layout-and-style-model.md, P1).
//
// Specificity is CSS's: id = 100, class / pseudo-class (each state flag, each structural
// test) = 10, type / pseudo-element = 1, summed over every compound of the chain.
// Beef `Type` -> const core::TypeInfo* (our RTTI); nullable String -> Optional<String>.

module;
#include "Core/Prelude.h"

export module foundation.ui:style_selector;

import foundation.core; // TypeInfo, IsDerivedFrom, Array, String, StringView, Optional, i32
import :control_state;

using namespace foundation::core;

export namespace foundation::ui
{
    class
        View; // defined in :view; Matches() body lives there (breaks the View<->styling module cycle)

    /// Structural pseudo-classes (position among siblings, emptiness). Flags.
    enum class StructuralMatch : u8
    {
        None = 0,
        FirstChild = 1,
        LastChild = 2,
        Empty = 4,
    };
    [[nodiscard]] constexpr StructuralMatch operator|(StructuralMatch a, StructuralMatch b) noexcept
    {
        return static_cast<StructuralMatch>(static_cast<u8>(a) | static_cast<u8>(b));
    }
    [[nodiscard]] constexpr bool HasStructural(StructuralMatch value, StructuralMatch flag) noexcept
    {
        return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
    }

    /// One compound: every constraint here must hold on ONE view.
    struct SelectorCompound
    {
        /// View type to match (null = any type). `UnknownType` marks a type name the sheet
        /// used that no registry knows: the compound then matches NOTHING (loud in tests,
        /// harmless at runtime) instead of silently matching everything.
        const TypeInfo* ViewType = nullptr;
        bool UnknownType = false;
        /// Style classes to match. All must be present on the view. Empty = any.
        Array<String> StyleClasses;
        /// `#id`: the view's Name.
        Optional<String> Id;
        /// Control state to match (empty = any state). All flags must be present on the view.
        Optional<ControlState> State;
        StructuralMatch Structural = StructuralMatch::None;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return ViewType == nullptr && !UnknownType && StyleClasses.Size() == 0 &&
                   !Id.HasValue() && !State.HasValue() && Structural == StructuralMatch::None;
        }

        [[nodiscard]] i32 Specificity() const noexcept
        {
            i32 s = static_cast<i32>(StyleClasses.Size()) * 10;
            if (Id.HasValue())
            {
                s += 100;
            }
            if (ViewType != nullptr || UnknownType)
            {
                s += 1;
            }
            if (State.HasValue())
            {
                // Each state flag is one pseudo-class; `:normal` (no flags) counts as one.
                const u32 bits = static_cast<u32>(State.Value());
                i32 count = 0;
                for (u32 b = bits; b != 0; b &= b - 1)
                {
                    ++count;
                }
                s += (count == 0 ? 1 : count) * 10;
            }
            for (u8 b = static_cast<u8>(Structural); b != 0; b &= static_cast<u8>(b - 1))
            {
                s += 10;
            }
            return s;
        }
    };

    /// An ancestor step: the compound an ancestor must match, and whether it must be the
    /// DIRECT parent (`>`) of the view matched by the step before it.
    struct SelectorAncestor
    {
        SelectorCompound Compound;
        bool DirectParent = false;
    };

    class StyleSelector
    {
    public:
        // The SUBJECT compound (the rightmost one: the view the rule applies to). Kept as flat
        // fields so the fluent sheet builders and every existing reader keep their shape.
        /// View type to match (null = any type).
        const TypeInfo* ViewType = nullptr;
        /// Style classes to match. All must be present on the view. Empty = any.
        Array<String> StyleClasses;
        /// `#id`: the view's Name.
        Optional<String> Id;
        /// Control state to match (empty = any state). All flags must be present on the view.
        Optional<ControlState> State;
        StructuralMatch Structural = StructuralMatch::None;
        /// See SelectorCompound::UnknownType.
        bool UnknownType = false;
        /// Pseudo-element name to match (empty = targets the element itself).
        Optional<String> PseudoElement;
        /// Ancestor steps, NEAREST FIRST: [0] is matched against the subject's parent (or an
        /// ancestor, per DirectParent), [1] against that view's parent/ancestor, and so on.
        Array<SelectorAncestor> Ancestors;

        StyleSelector() = default;

        /// Computed specificity (higher wins in the cascade).
        [[nodiscard]] i32 Specificity() const noexcept
        {
            i32 s = Subject().Specificity();
            if (PseudoElement.HasValue())
            {
                s += 1;
            }
            for (const SelectorAncestor& a : Ancestors)
            {
                s += a.Compound.Specificity();
            }
            return s;
        }

        /// Whether this selector matches the given view, state, and optional pseudo-element name.
        /// Body defined in the :view partition (needs View complete).
        [[nodiscard]] bool Matches(const View& view, ControlState state,
                                   StringView pseudoElement = {}) const;

        void AddClass(StringView name) { StyleClasses.PushBack(String(name)); }
        void SetId(StringView id) { Id = String(id); }
        void SetPseudoElement(StringView name) { PseudoElement = String(name); }
        /// Prepend an ancestor step (call in the order the sheet lists them: outermost first).
        void AddAncestor(SelectorCompound compound, bool directParent)
        {
            Ancestors.Insert(0, SelectorAncestor{Move(compound), directParent});
        }

        /// The subject as a compound (a copy; the flat fields are the storage).
        [[nodiscard]] SelectorCompound Subject() const
        {
            SelectorCompound c;
            c.ViewType = ViewType;
            c.UnknownType = UnknownType;
            for (const String& cls : StyleClasses)
            {
                c.StyleClasses.PushBack(cls);
            }
            c.Id = Id;
            c.State = State;
            c.Structural = Structural;
            return c;
        }

        /// True if this selector has no constraints (matches every view/state, no pseudo).
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return ViewType == nullptr && !UnknownType && StyleClasses.Size() == 0 &&
                   !Id.HasValue() && !State.HasValue() && Structural == StructuralMatch::None &&
                   !PseudoElement.HasValue() && Ancestors.IsEmpty();
        }

        /// True if this selector targets only a specific pseudo-element with no other constraints.
        [[nodiscard]] bool IsPseudoElementOnly(StringView part) const
        {
            return ViewType == nullptr && !UnknownType && StyleClasses.Size() == 0 &&
                   !Id.HasValue() && !State.HasValue() && Structural == StructuralMatch::None &&
                   Ancestors.IsEmpty() && PseudoElement.HasValue() &&
                   PseudoElement.Value().AsView() == part;
        }
    };
}
