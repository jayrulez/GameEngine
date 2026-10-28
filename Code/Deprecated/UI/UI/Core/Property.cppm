// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :property partition
//
// Property<T>: observable value wrapper with change notification + owner invalidation.
// Ported from Sedulous.UI/src/Core/Property.bf.
//
// Divergences (language): Beef `Value` property -> Value() getter + SetValue() setter;
// Beef `Event` -> our Event<void(T)>; owner is IPropertyOwner* (see :property_owner).
// Requires T to be equality-comparable (Beef's `where bool : operator T == T`).

module;
#include "Core/Prelude.h"

export module foundation.ui:property;

import foundation.core;
import :enums;
import :event;
import :property_owner;

using namespace foundation::core;

export namespace foundation::ui
{
    template <typename T>
    class Property
    {
    public:
        Property() = default;
        explicit Property(T initialValue) : m_value(Move(initialValue)) {}
        Property(T initialValue, InvalidationKind kind)
            : m_value(Move(initialValue)), m_invalidationKind(kind)
        {
        }

        /// The current value.
        [[nodiscard]] const T& Value() const noexcept { return m_value; }

        /// Set the value. Fires Changed and notifies the owner only if the value differs.
        /// A loop guard prevents infinite recursion from two-way bindings.
        void SetValue(T value)
        {
            if (m_isUpdating)
            {
                return;
            }
            if (m_value == value)
            {
                return;
            }

            m_isUpdating = true;
            m_value = Move(value);
            Changed(m_value);
            if (m_owner != nullptr)
            {
                m_owner->OnPropertyChanged(m_invalidationKind);
            }
            m_isUpdating = false;
        }

        /// Set the value without firing Changed or invalidating (init / echo suppression).
        void SetSilent(T value) { m_value = Move(value); }

        /// Wire up auto-invalidation. Called during view construction.
        void SetOwner(IPropertyOwner* owner, InvalidationKind kind = InvalidationKind::Layout)
        {
            m_owner = owner;
            m_invalidationKind = kind;
        }

        /// One-way: when this changes, push the value into target.
        void BindTo(Property<T>& target)
        {
            Property<T>* t = &target;
            Changed.Add(typename Event<void(T)>::Handler{[t](T val) { t->SetValue(Move(val)); }});
        }

        /// Two-way: changes to either side update the other (loop guard prevents recursion).
        void BindTwoWay(Property<T>& other)
        {
            Property<T>* o = &other;
            Property<T>* self = this;
            Changed.Add(typename Event<void(T)>::Handler{[o](T val) { o->SetValue(Move(val)); }});
            other.Changed.Add(
                typename Event<void(T)>::Handler{[self](T val) { self->SetValue(Move(val)); }});
        }

        /// Fired with the new value whenever it changes.
        Event<void(T)> Changed;

    private:
        T m_value{};
        bool m_isUpdating = false;
        IPropertyOwner* m_owner = nullptr;
        InvalidationKind m_invalidationKind = InvalidationKind::Layout;
    };
}
