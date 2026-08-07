// Draconic UI - :style_selector partition
//
// Matches views by type, style class(es), control state, and optional pseudo-element name.
// Specificity: class=10, type=1, state=1, pseudo=1. Ported from Sedulous.UI/src/Styling/StyleSelector.bf.
// Beef `Type` -> const foundation::TypeInfo* (our RTTI); nullable String -> Optional<String>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:style_selector;

import draconic.foundation; // TypeInfo, IsDerivedFrom, Array, String, StringView, Optional, i32
import :control_state;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class
        View; // defined in :view; Matches() body lives there (breaks the View<->styling module cycle)

    class StyleSelector
    {
    public:
        /// View type to match (null = any type).
        const TypeInfo* ViewType = nullptr;
        /// Style classes to match. All must be present on the view. Empty = any.
        Array<String> StyleClasses;
        /// Control state to match (empty = any state). All flags must be present on the view.
        Optional<ControlState> State;
        /// Pseudo-element name to match (empty = targets the element itself).
        Optional<String> PseudoElement;

        StyleSelector() = default;

        /// Computed specificity (higher wins in the cascade).
        [[nodiscard]] i32 Specificity() const noexcept
        {
            i32 s = static_cast<i32>(StyleClasses.Size()) * 10;
            if (ViewType != nullptr)
            {
                s += 1;
            }
            if (State.HasValue())
            {
                s += 1;
            }
            if (PseudoElement.HasValue())
            {
                s += 1;
            }
            return s;
        }

        /// Whether this selector matches the given view, state, and optional pseudo-element name.
        /// Body defined in the :view partition (needs View complete).
        [[nodiscard]] bool Matches(const View& view, ControlState state,
                                   StringView pseudoElement = {}) const;

        void AddClass(StringView name) { StyleClasses.PushBack(String(name)); }
        void SetPseudoElement(StringView name) { PseudoElement = String(name); }

        /// True if this selector has no constraints (matches every view/state, no pseudo).
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return ViewType == nullptr && StyleClasses.Size() == 0 && !State.HasValue() &&
                   !PseudoElement.HasValue();
        }

        /// True if this selector targets only a specific pseudo-element with no other constraints.
        [[nodiscard]] bool IsPseudoElementOnly(StringView part) const
        {
            return ViewType == nullptr && StyleClasses.Size() == 0 && !State.HasValue() &&
                   PseudoElement.HasValue() && PseudoElement.Value().AsView() == part;
        }
    };
}
