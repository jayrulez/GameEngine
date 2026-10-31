// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :property_owner partition
//
// IPropertyOwner: objects that own Property<T> instances and respond to value changes
// with invalidation.
//
// It is a pure-abstract base (View inherits it) rather than part of View, which breaks
// the Property/View dependency cycle and keeps Property<T> self-contained and testable.
// It is never queried through the tree, so the As*() capability split does not cover it.

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
