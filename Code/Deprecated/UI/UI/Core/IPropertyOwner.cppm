// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :property_owner partition
//
// IPropertyOwner: objects that own Property<T> instances and respond to value changes
// with invalidation. Ported from Sedulous.UI/src/Core/IPropertyOwner.bf.
//
// Port note: kept as a pure-abstract base (View inherits it) rather than folded into
// View - this breaks the Property<->View dependency cycle cleanly, keeps Property<T>
// self-contained/testable, and is faithful to Sedulous (whose Property stores an
// IPropertyOwner). It is never tree-queried, so the As*() capability split is unaffected.

export module foundation.ui:property_owner;

import :enums;

export namespace foundation::ui
{
    class IPropertyOwner
    {
    public:
        virtual ~IPropertyOwner() = default;

        /// Called when an owned property's value changes.
        virtual void OnPropertyChanged(InvalidationKind kind) = 0;
    };
}
