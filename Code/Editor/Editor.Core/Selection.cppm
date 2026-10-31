// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :selection partition.
//
// Generic editor selection set: ordered and deduplicated, with a primary element (Items()[0], the
// gizmo pivot and transform anchor) and a change event that panels (inspector, hierarchy, gizmos)
// subscribe to. Selection changes are not undoable.

module;
#include "Core/Prelude.h"

export module editor.core:selection;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    template <typename T>
    class Selection
    {
    public:
        Selection() = default;
        Selection(const Selection&) = delete;
        Selection& operator=(const Selection&) = delete;

        /// Fired after any change to the set.
        Function<void()> OnChanged;

        [[nodiscard]] Span<const T> Items() const noexcept
        {
            return Span<const T>{m_items.Data(), m_items.Size()};
        }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_items.IsEmpty(); }
        [[nodiscard]] usize Size() const noexcept { return m_items.Size(); }

        /// The primary element (the pivot): the first item, or null when empty.
        [[nodiscard]] const T* Primary() const noexcept
        {
            return m_items.IsEmpty() ? nullptr : &m_items[0];
        }

        [[nodiscard]] bool Contains(const T& item) const
        {
            for (const T& existing : m_items)
            {
                if (existing == item)
                {
                    return true;
                }
            }
            return false;
        }

        void Set(const T& item)
        {
            m_items.Clear();
            m_items.PushBack(item);
            Notify();
        }

        /// Replace the selection; duplicates are removed and the first occurrence stays primary.
        void Set(Span<const T> items)
        {
            m_items.Clear();
            for (const T& item : items)
            {
                if (!Contains(item))
                {
                    m_items.PushBack(item);
                }
            }
            Notify();
        }

        void Add(const T& item)
        {
            if (Contains(item))
            {
                return;
            }
            m_items.PushBack(item);
            Notify();
        }

        void Remove(const T& item)
        {
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                if (m_items[i] == item)
                {
                    m_items.RemoveAt(i);
                    Notify();
                    return;
                }
            }
        }

        /// Add if absent, remove if present (ctrl-click).
        void Toggle(const T& item)
        {
            if (Contains(item))
            {
                Remove(item);
            }
            else
            {
                Add(item);
            }
        }

        void Clear()
        {
            if (m_items.IsEmpty())
            {
                return;
            }
            m_items.Clear();
            Notify();
        }

    private:
        void Notify()
        {
            if (OnChanged)
            {
                OnChanged();
            }
        }

        Array<T> m_items;
    };
}
