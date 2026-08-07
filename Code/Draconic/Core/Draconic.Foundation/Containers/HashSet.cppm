module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:hash_set;

import :base;
import :allocator;
import :hash;
import :hash_map;

export namespace draconic::foundation
{
    // =======================================================================
    // HashSet - a set of keys, built on HashMap. Iterates keys.
    // =======================================================================
    template <typename K, typename Hasher = Hash<K>>
    class HashSet
    {
        using MapType = HashMap<K, u8, Hasher>;

    public:
        HashSet() noexcept = default;
        explicit HashSet(IAllocator& allocator) noexcept : m_map(allocator) {}

        // Returns true if the key was newly inserted, false if already present.
        bool Insert(const K& key)
        {
            const bool existed = m_map.Contains(key);
            m_map.InsertOrAssign(key, u8{0});
            return !existed;
        }

        [[nodiscard]] bool Contains(const K& key) const noexcept { return m_map.Contains(key); }
        bool Remove(const K& key) noexcept { return m_map.Remove(key); }
        void Clear() noexcept { m_map.Clear(); }

        [[nodiscard]] usize Size() const noexcept { return m_map.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_map.IsEmpty(); }

        // Iteration yields keys.
        template <typename MapIterator>
        class BasicIterator
        {
        public:
            explicit BasicIterator(MapIterator it) noexcept : m_it(it) {}
            [[nodiscard]] const K& operator*() const noexcept { return (*m_it).key; }
            BasicIterator& operator++() noexcept
            {
                ++m_it;
                return *this;
            }
            [[nodiscard]] bool operator!=(const BasicIterator& other) const noexcept
            {
                return m_it != other.m_it;
            }

        private:
            MapIterator m_it;
        };

        // Explicit iterator return types (NOT `auto`): GCC's C++ modules mis-merge `auto`-deduced return
        // types for a class-template member when the same instantiation crosses several imported modules
        // ("conflicting deduced return type for imported declaration"). Naming the types avoids that.
        using Iterator = BasicIterator<typename MapType::Iterator>;
        using ConstIterator = BasicIterator<typename MapType::ConstIterator>;

        [[nodiscard]] Iterator begin() noexcept { return Iterator{m_map.begin()}; }
        [[nodiscard]] Iterator end() noexcept { return Iterator{m_map.end()}; }
        [[nodiscard]] ConstIterator begin() const noexcept { return ConstIterator{m_map.begin()}; }
        [[nodiscard]] ConstIterator end() const noexcept { return ConstIterator{m_map.end()}; }

    private:
        MapType m_map;
    };
}
