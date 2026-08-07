// Draconic Foundation - :hash_map partition
//
// Open-addressing hash map: power-of-two capacity, linear probing, tombstones
// on erase, max load factor 3/4. Allocator-backed. Keys are compared with
// operator==; hashing via Hash<K> (override with the Hasher template param).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <type_traits>

export module draconic.foundation:hash_map;

import :base;
import :allocator;
import :hash;

export namespace draconic::foundation
{
    template <typename K, typename V, typename Hasher = Hash<K>>
    class HashMap
    {
    public:
        struct Entry
        {
            K key;
            V value;
        };

        HashMap() noexcept : m_allocator(&DefaultAllocator()) {}
        explicit HashMap(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        HashMap(const HashMap& other) : m_allocator(other.m_allocator)
        {
            for (const Entry& e : other)
            {
                InsertOrAssign(e.key, e.value);
            }
        }

        HashMap(HashMap&& other) noexcept
            : m_entries(other.m_entries), m_states(other.m_states), m_size(other.m_size),
              m_tombstones(other.m_tombstones), m_capacity(other.m_capacity),
              m_allocator(other.m_allocator)
        {
            other.m_entries = nullptr;
            other.m_states = nullptr;
            other.m_size = 0;
            other.m_tombstones = 0;
            other.m_capacity = 0;
        }

        HashMap& operator=(const HashMap& other)
        {
            if (this != &other)
            {
                Clear();
                for (const Entry& e : other)
                {
                    InsertOrAssign(e.key, e.value);
                }
            }
            return *this;
        }

        HashMap& operator=(HashMap&& other) noexcept
        {
            if (this != &other)
            {
                Destroy();
                m_entries = other.m_entries;
                m_states = other.m_states;
                m_size = other.m_size;
                m_tombstones = other.m_tombstones;
                m_capacity = other.m_capacity;
                m_allocator = other.m_allocator;
                other.m_entries = nullptr;
                other.m_states = nullptr;
                other.m_size = 0;
                other.m_tombstones = 0;
                other.m_capacity = 0;
            }
            return *this;
        }

        ~HashMap() { Destroy(); }

        [[nodiscard]] usize Size() const noexcept { return m_size; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }
        [[nodiscard]] usize Capacity() const noexcept { return m_capacity; }

        // Inserts the key, or overwrites the value if present. Returns the value.
        V& InsertOrAssign(const K& key, const V& value)
        {
            EnsureCapacityForInsert();

            const usize mask = m_capacity - 1;
            usize index = Hasher{}(key)&mask;
            usize tombstone = kNoSlot;

            while (m_states[index] != State::Empty)
            {
                if (m_states[index] == State::Occupied && m_entries[index].key == key)
                {
                    m_entries[index].value = value;
                    return m_entries[index].value;
                }
                if (m_states[index] == State::Tombstone && tombstone == kNoSlot)
                {
                    tombstone = index;
                }
                index = (index + 1) & mask;
            }

            const usize target = (tombstone != kNoSlot) ? tombstone : index;
            if (tombstone != kNoSlot)
            {
                --m_tombstones;
            }
            Construct<Entry>(&m_entries[target], key, value);
            m_states[target] = State::Occupied;
            ++m_size;
            return m_entries[target].value;
        }

        // Move-value overload: inserts/overwrites with a moved value, so MOVE-ONLY values
        // (Function, UniquePtr, ...) can live in a HashMap.
        V& InsertOrAssign(const K& key, V&& value)
        {
            EnsureCapacityForInsert();

            const usize mask = m_capacity - 1;
            usize index = Hasher{}(key)&mask;
            usize tombstone = kNoSlot;

            while (m_states[index] != State::Empty)
            {
                if (m_states[index] == State::Occupied && m_entries[index].key == key)
                {
                    m_entries[index].value = Move(value);
                    return m_entries[index].value;
                }
                if (m_states[index] == State::Tombstone && tombstone == kNoSlot)
                {
                    tombstone = index;
                }
                index = (index + 1) & mask;
            }

            const usize target = (tombstone != kNoSlot) ? tombstone : index;
            if (tombstone != kNoSlot)
            {
                --m_tombstones;
            }
            Construct<Entry>(&m_entries[target], key, Move(value));
            m_states[target] = State::Occupied;
            ++m_size;
            return m_entries[target].value;
        }

        [[nodiscard]] V* Find(const K& key) noexcept
        {
            const usize index = FindIndex(key);
            return (index != kNoSlot) ? &m_entries[index].value : nullptr;
        }

        [[nodiscard]] const V* Find(const K& key) const noexcept
        {
            const usize index = FindIndex(key);
            return (index != kNoSlot) ? &m_entries[index].value : nullptr;
        }

        [[nodiscard]] bool Contains(const K& key) const noexcept
        {
            return FindIndex(key) != kNoSlot;
        }

        bool Remove(const K& key) noexcept
        {
            const usize index = FindIndex(key);
            if (index == kNoSlot)
            {
                return false;
            }
            Destruct(&m_entries[index]);
            m_states[index] = State::Tombstone;
            --m_size;
            ++m_tombstones;
            return true;
        }

        void Clear() noexcept
        {
            for (usize i = 0; i < m_capacity; ++i)
            {
                if (m_states[i] == State::Occupied)
                {
                    Destruct(&m_entries[i]);
                }
                m_states[i] = State::Empty;
            }
            m_size = 0;
            m_tombstones = 0;
        }

        // --- iteration (over occupied slots) -------------------------------
        template <bool Const>
        class BasicIterator
        {
        public:
            using MapPtr = std::conditional_t<Const, const HashMap*, HashMap*>;
            using EntryRef = std::conditional_t<Const, const Entry&, Entry&>;

            BasicIterator(MapPtr map, usize index) noexcept : m_map(map), m_index(index)
            {
                Advance();
            }

            [[nodiscard]] EntryRef operator*() const noexcept { return m_map->m_entries[m_index]; }

            BasicIterator& operator++() noexcept
            {
                ++m_index;
                Advance();
                return *this;
            }

            [[nodiscard]] bool operator!=(const BasicIterator& other) const noexcept
            {
                return m_index != other.m_index;
            }

        private:
            void Advance() noexcept
            {
                while (m_index < m_map->m_capacity && m_map->m_states[m_index] != State::Occupied)
                {
                    ++m_index;
                }
            }

            MapPtr m_map;
            usize m_index;
        };

        using Iterator = BasicIterator<false>;
        using ConstIterator = BasicIterator<true>;

        [[nodiscard]] Iterator begin() noexcept { return Iterator{this, 0}; }
        [[nodiscard]] Iterator end() noexcept { return Iterator{this, m_capacity}; }
        [[nodiscard]] ConstIterator begin() const noexcept { return ConstIterator{this, 0}; }
        [[nodiscard]] ConstIterator end() const noexcept { return ConstIterator{this, m_capacity}; }

    private:
        enum class State : u8
        {
            Empty,
            Occupied,
            Tombstone,
        };

        static constexpr usize kInitialCapacity = 16;
        static constexpr usize kNoSlot = static_cast<usize>(-1);

        [[nodiscard]] usize FindIndex(const K& key) const noexcept
        {
            if (m_capacity == 0)
            {
                return kNoSlot;
            }
            const usize mask = m_capacity - 1;
            usize index = Hasher{}(key)&mask;
            while (m_states[index] != State::Empty)
            {
                if (m_states[index] == State::Occupied && m_entries[index].key == key)
                {
                    return index;
                }
                index = (index + 1) & mask;
            }
            return kNoSlot;
        }

        static constexpr usize NextPowerOfTwo(usize n) noexcept
        {
            if (n < 2)
            {
                return 1;
            }
            --n;
            n |= n >> 1;
            n |= n >> 2;
            n |= n >> 4;
            n |= n >> 8;
            n |= n >> 16;
            n |= (n >> 16) >> 16; // the 64-bit fold; two legal shifts so 32-bit usize
                                  // (wasm32) neither overflows nor warns - it's a no-op
            return n + 1;
        }

        void EnsureCapacityForInsert()
        {
            if (m_capacity == 0)
            {
                Rehash(kInitialCapacity);
            }
            else if ((m_size + m_tombstones + 1) * 4 >= m_capacity * 3)
            {
                const usize wanted = (m_size + 1) * 2;
                Rehash(NextPowerOfTwo(wanted < kInitialCapacity ? kInitialCapacity : wanted));
            }
        }

        void Rehash(usize newCapacity)
        {
            Entry* oldEntries = m_entries;
            State* oldStates = m_states;
            const usize oldCapacity = m_capacity;

            m_entries = static_cast<Entry*>(
                m_allocator->Allocate(newCapacity * sizeof(Entry), alignof(Entry)));
            m_states = static_cast<State*>(
                m_allocator->Allocate(newCapacity * sizeof(State), alignof(State)));
            DRACONIC_ASSERT_MSG(m_entries != nullptr && m_states != nullptr,
                                "HashMap allocation failed");

            for (usize i = 0; i < newCapacity; ++i)
            {
                m_states[i] = State::Empty;
            }

            m_capacity = newCapacity;
            m_size = 0;
            m_tombstones = 0;

            for (usize i = 0; i < oldCapacity; ++i)
            {
                if (oldStates[i] == State::Occupied)
                {
                    InsertMoved(Move(oldEntries[i]));
                    Destruct(&oldEntries[i]);
                }
            }

            if (oldEntries != nullptr)
            {
                m_allocator->Free(oldEntries);
            }
            if (oldStates != nullptr)
            {
                m_allocator->Free(oldStates);
            }
        }

        // Inserts a moved entry during rehash; the key is known to be unique
        // and capacity is guaranteed sufficient, so no growth/probing for dups.
        void InsertMoved(Entry&& entry)
        {
            const usize mask = m_capacity - 1;
            usize index = Hasher{}(entry.key) & mask;
            while (m_states[index] == State::Occupied)
            {
                index = (index + 1) & mask;
            }
            Construct<Entry>(&m_entries[index], Move(entry.key), Move(entry.value));
            m_states[index] = State::Occupied;
            ++m_size;
        }

        void Destroy() noexcept
        {
            if (m_capacity != 0)
            {
                Clear();
                m_allocator->Free(m_entries);
                m_allocator->Free(m_states);
            }
            m_entries = nullptr;
            m_states = nullptr;
            m_capacity = 0;
        }

        Entry* m_entries = nullptr;
        State* m_states = nullptr;
        usize m_size = 0;
        usize m_tombstones = 0;
        usize m_capacity = 0;
        IAllocator* m_allocator = nullptr;
    };
}
