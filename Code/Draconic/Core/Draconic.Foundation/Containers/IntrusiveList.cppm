module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <type_traits>

export module draconic.foundation:intrusive_list;

import :base;

export namespace draconic::foundation
{
    // =======================================================================
    // IntrusiveList - doubly-linked list whose link lives in the element.
    // Non-owning: the caller owns the elements. T must derive from
    // IntrusiveListNode. O(1) insert/remove given the element.
    // =======================================================================
    struct IntrusiveListNode
    {
        IntrusiveListNode* prev = nullptr;
        IntrusiveListNode* next = nullptr;
    };

    template <typename T>
    class IntrusiveList
    {
        static_assert(std::is_base_of_v<IntrusiveListNode, T>,
                      "T must derive from IntrusiveListNode.");

    public:
        IntrusiveList() noexcept { Reset(); }

        IntrusiveList(const IntrusiveList&) = delete;
        IntrusiveList& operator=(const IntrusiveList&) = delete;

        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }
        [[nodiscard]] usize Size() const noexcept { return m_size; }

        void PushBack(T& item) noexcept { InsertBefore(&m_sentinel, item); }
        void PushFront(T& item) noexcept { InsertBefore(m_sentinel.next, item); }

        void Remove(T& item) noexcept
        {
            IntrusiveListNode* node = &item;
            node->prev->next = node->next;
            node->next->prev = node->prev;
            node->prev = nullptr;
            node->next = nullptr;
            --m_size;
        }

        [[nodiscard]] T* Front() noexcept
        {
            return IsEmpty() ? nullptr : static_cast<T*>(m_sentinel.next);
        }
        [[nodiscard]] T* Back() noexcept
        {
            return IsEmpty() ? nullptr : static_cast<T*>(m_sentinel.prev);
        }

        // Unlinks all elements (does not destroy them).
        void Clear() noexcept
        {
            IntrusiveListNode* node = m_sentinel.next;
            while (node != &m_sentinel)
            {
                IntrusiveListNode* nextNode = node->next;
                node->prev = nullptr;
                node->next = nullptr;
                node = nextNode;
            }
            Reset();
        }

        class Iterator
        {
        public:
            explicit Iterator(IntrusiveListNode* node) noexcept : m_node(node) {}
            [[nodiscard]] T& operator*() const noexcept { return *static_cast<T*>(m_node); }
            Iterator& operator++() noexcept
            {
                m_node = m_node->next;
                return *this;
            }
            [[nodiscard]] bool operator!=(const Iterator& other) const noexcept
            {
                return m_node != other.m_node;
            }

        private:
            IntrusiveListNode* m_node;
        };

        [[nodiscard]] Iterator begin() noexcept { return Iterator{m_sentinel.next}; }
        [[nodiscard]] Iterator end() noexcept { return Iterator{&m_sentinel}; }

    private:
        void Reset() noexcept
        {
            m_sentinel.next = &m_sentinel;
            m_sentinel.prev = &m_sentinel;
            m_size = 0;
        }

        void InsertBefore(IntrusiveListNode* position, T& item) noexcept
        {
            IntrusiveListNode* node = &item;
            node->prev = position->prev;
            node->next = position;
            position->prev->next = node;
            position->prev = node;
            ++m_size;
        }

        IntrusiveListNode m_sentinel;
        usize m_size = 0;
    };
}
