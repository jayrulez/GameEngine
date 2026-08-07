// Draconic Foundation - :string partition
//
// String types. `String` is the primary type (UTF-8 / char8_t);
// `WideString` is the secondary UTF-16 type (Win32 edge). Both are aliases of one
// allocator-backed BasicString<CharT>, each with a matching view.
//
// NOTE: cross-encoding transcoding (UTF-16 <-> UTF-8) and small-string
// optimization are deferred.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <charconv>
#include <system_error>

export module draconic.foundation:string;

import :base;
import :allocator;
import :hash;

export namespace draconic::foundation
{
    template <typename CharT>
    [[nodiscard]] constexpr usize CStringLength(const CharT* str) noexcept
    {
        if (str == nullptr)
        {
            return 0;
        }
        usize length = 0;
        while (str[length] != CharT(0))
        {
            ++length;
        }
        return length;
    }

    // =======================================================================
    // BasicStringView - non-owning view over a contiguous character range.
    // =======================================================================
    template <typename CharT>
    class BasicStringView
    {
    public:
        using ValueType = CharT;

        constexpr BasicStringView() noexcept = default;
        constexpr BasicStringView(const CharT* data, usize size) noexcept
            : m_data(data), m_size(size)
        {
        }
        constexpr BasicStringView(const CharT* str) noexcept
            : m_data(str), m_size(CStringLength(str))
        {
        }

        [[nodiscard]] constexpr const CharT* Data() const noexcept { return m_data; }
        [[nodiscard]] constexpr usize Size() const noexcept { return m_size; }
        [[nodiscard]] constexpr usize Length() const noexcept { return m_size; }
        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return m_size == 0; }

        [[nodiscard]] constexpr CharT operator[](usize index) const noexcept
        {
            DRACONIC_ASSERT(index < m_size);
            return m_data[index];
        }

        [[nodiscard]] constexpr const CharT* begin() const noexcept { return m_data; }
        [[nodiscard]] constexpr const CharT* end() const noexcept { return m_data + m_size; }

        [[nodiscard]] constexpr BasicStringView SubStr(usize offset, usize count) const noexcept
        {
            DRACONIC_ASSERT(offset + count <= m_size);
            return BasicStringView{m_data + offset, count};
        }

        [[nodiscard]] constexpr bool StartsWith(BasicStringView prefix) const noexcept
        {
            return prefix.m_size <= m_size && BasicStringView{m_data, prefix.m_size} == prefix;
        }

        [[nodiscard]] constexpr bool EndsWith(BasicStringView suffix) const noexcept
        {
            return suffix.m_size <= m_size &&
                   BasicStringView{m_data + (m_size - suffix.m_size), suffix.m_size} == suffix;
        }

    private:
        const CharT* m_data = nullptr;
        usize m_size = 0;
    };

    // =======================================================================
    // BasicString - null-terminated, growable string with small-string
    // optimization: short strings live inline; longer ones move to the heap.
    // =======================================================================
    template <typename CharT>
    class BasicString
    {
    public:
        using ValueType = CharT;
        using View = BasicStringView<CharT>;

        BasicString() noexcept : m_allocator(&DefaultAllocator())
        {
            m_storage.inlineBuf[0] = CharT(0);
        }
        explicit BasicString(IAllocator& allocator) noexcept : m_allocator(&allocator)
        {
            m_storage.inlineBuf[0] = CharT(0);
        }

        BasicString(const CharT* str, IAllocator& allocator = DefaultAllocator())
            : m_allocator(&allocator)
        {
            m_storage.inlineBuf[0] = CharT(0);
            Append(str, CStringLength(str));
        }

        BasicString(View view, IAllocator& allocator = DefaultAllocator()) : m_allocator(&allocator)
        {
            m_storage.inlineBuf[0] = CharT(0);
            Append(view.Data(), view.Size());
        }

        BasicString(const BasicString& other) : m_allocator(other.m_allocator)
        {
            m_storage.inlineBuf[0] = CharT(0);
            Append(other.Data(), other.m_size);
        }

        BasicString(BasicString&& other) noexcept : m_allocator(other.m_allocator)
        {
            AdoptOrCopy(other);
        }

        BasicString& operator=(const BasicString& other)
        {
            if (this != &other)
            {
                Clear();
                Append(other.Data(), other.m_size);
            }
            return *this;
        }

        BasicString& operator=(BasicString&& other) noexcept
        {
            if (this != &other)
            {
                FreeHeap();
                m_allocator = other.m_allocator;
                AdoptOrCopy(other);
            }
            return *this;
        }

        ~BasicString() { FreeHeap(); }

        // --- capacity ------------------------------------------------------
        [[nodiscard]] usize Size() const noexcept { return m_size; }
        [[nodiscard]] usize Length() const noexcept { return m_size; }
        [[nodiscard]] usize Capacity() const noexcept
        {
            return m_isHeap ? m_storage.heap.capacity : kInlineCapacity;
        }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }
        [[nodiscard]] bool IsSmall() const noexcept { return !m_isHeap; }

        void Reserve(usize newCapacity)
        {
            if (newCapacity <= Capacity())
            {
                return;
            }

            // +1 for the null terminator.
            CharT* newData = static_cast<CharT*>(
                m_allocator->Allocate((newCapacity + 1) * sizeof(CharT), alignof(CharT)));
            DRACONIC_ASSERT_MSG(newData != nullptr, "WideString allocation failed");

            MemCopy(newData, Data(), (m_size + 1) * sizeof(CharT)); // copy incl. terminator
            FreeHeap();
            m_storage.heap.data = newData;
            m_storage.heap.capacity = newCapacity;
            m_isHeap = true;
        }

        void Clear() noexcept
        {
            m_size = 0;
            Data()[0] = CharT(0);
        }

        // --- append --------------------------------------------------------
        void Append(const CharT* str, usize count)
        {
            if (count == 0)
            {
                return;
            }
            EnsureCapacity(m_size + count);
            CharT* data = Data();
            MemCopy(data + m_size, str, count * sizeof(CharT));
            m_size += count;
            data[m_size] = CharT(0);
        }

        void Append(View view) { Append(view.Data(), view.Size()); }

        void PushBack(CharT ch)
        {
            EnsureCapacity(m_size + 1);
            CharT* data = Data();
            data[m_size++] = ch;
            data[m_size] = CharT(0);
        }

        // Single-character append (alias for PushBack) - lets WideString serve as a
        // format sink alongside its Append(view)/Append(ptr,len) overloads.
        void Append(CharT ch) { PushBack(ch); }

        BasicString& operator+=(View view)
        {
            Append(view);
            return *this;
        }
        BasicString& operator+=(const CharT* str)
        {
            Append(str, CStringLength(str));
            return *this;
        }
        BasicString& operator+=(CharT ch)
        {
            PushBack(ch);
            return *this;
        }

        // --- insert / remove ----------------------------------------------
        // Inserts `view`'s code units at code-unit position `index` (clamped to [0, Size()]).
        void Insert(usize index, View view)
        {
            const usize count = view.Size();
            if (count == 0)
            {
                return;
            }
            if (index > m_size)
            {
                index = m_size;
            }
            EnsureCapacity(m_size + count);
            CharT* data = Data();
            // Shift the tail (incl. terminator) right to open a gap.
            MemMove(data + index + count, data + index, (m_size - index + 1) * sizeof(CharT));
            MemCopy(data + index, view.Data(), count * sizeof(CharT));
            m_size += count;
        }

        // Removes `count` code units starting at `index`. Out-of-range portions are clamped.
        void Remove(usize index, usize count)
        {
            if (index >= m_size || count == 0)
            {
                return;
            }
            if (count > m_size - index)
            {
                count = m_size - index;
            }
            CharT* data = Data();
            // Shift the tail (incl. terminator) left to close the gap.
            MemMove(data + index, data + index + count,
                    (m_size - index - count + 1) * sizeof(CharT));
            m_size -= count;
        }

        // In-place replace of every code unit equal to `from` with `to` (size-preserving; no realloc).
        // Returns the number replaced. Code-unit granular - intended for ASCII/separator characters.
        usize Replace(CharT from, CharT to)
        {
            CharT* data = Data();
            usize replaced = 0;
            for (usize i = 0; i < m_size; ++i)
            {
                if (data[i] == from)
                {
                    data[i] = to;
                    ++replaced;
                }
            }
            return replaced;
        }

        // --- access --------------------------------------------------------
        [[nodiscard]] CharT& operator[](usize index) noexcept
        {
            DRACONIC_ASSERT(index < m_size);
            return Data()[index];
        }
        [[nodiscard]] const CharT& operator[](usize index) const noexcept
        {
            DRACONIC_ASSERT(index < m_size);
            return Data()[index];
        }

        // Always null-terminated.
        [[nodiscard]] CharT* Data() noexcept
        {
            return m_isHeap ? m_storage.heap.data : m_storage.inlineBuf;
        }
        [[nodiscard]] const CharT* Data() const noexcept
        {
            return m_isHeap ? m_storage.heap.data : m_storage.inlineBuf;
        }
        [[nodiscard]] const CharT* CStr() const noexcept { return Data(); }

        [[nodiscard]] View AsView() const noexcept { return View{Data(), m_size}; }
        operator View() const noexcept { return AsView(); }

        [[nodiscard]] CharT* begin() noexcept { return Data(); }
        [[nodiscard]] CharT* end() noexcept { return Data() + m_size; }
        [[nodiscard]] const CharT* begin() const noexcept { return Data(); }
        [[nodiscard]] const CharT* end() const noexcept { return Data() + m_size; }

    private:
        static constexpr usize kInlineBytes = 3 * sizeof(void*);
        static constexpr usize kInlineCapacity = (kInlineBytes / sizeof(CharT)) > 1
                                                     ? (kInlineBytes / sizeof(CharT)) - 1
                                                     : 1;
        static constexpr usize kInitialHeapCapacity = (kInlineCapacity + 1) * 2;

        void EnsureCapacity(usize required)
        {
            const usize capacity = Capacity();
            if (required > capacity)
            {
                const usize doubled = capacity * 2;
                const usize next = (required > doubled) ? required : doubled;
                Reserve(next < kInitialHeapCapacity ? kInitialHeapCapacity : next);
            }
        }

        void FreeHeap() noexcept
        {
            if (m_isHeap && m_storage.heap.data != nullptr)
            {
                m_allocator->Free(m_storage.heap.data);
            }
            m_isHeap = false;
        }

        // Takes `other`'s buffer (heap) or copies its inline data; leaves
        // `other` empty. Assumes *this owns no heap buffer.
        void AdoptOrCopy(BasicString& other) noexcept
        {
            if (other.m_isHeap)
            {
                m_isHeap = true;
                m_storage.heap = other.m_storage.heap;
            }
            else
            {
                m_isHeap = false;
                MemCopy(m_storage.inlineBuf, other.m_storage.inlineBuf,
                        (other.m_size + 1) * sizeof(CharT));
            }
            m_size = other.m_size;
            other.m_isHeap = false;
            other.m_size = 0;
            other.m_storage.inlineBuf[0] = CharT(0);
        }

        union Storage
        {
            struct
            {
                CharT* data;
                usize capacity;
            } heap;
            CharT inlineBuf[kInlineCapacity + 1];
        };

        Storage m_storage;
        usize m_size = 0;
        IAllocator* m_allocator = nullptr;
        bool m_isHeap = false;
    };

    // Comparison as free function templates (not hidden friends): friends
    // defined in an exported module class can get strong per-TU symbols under
    // GCC, colliding at link; template free functions have COMDAT linkage.
    template <typename CharT>
    [[nodiscard]] constexpr bool operator==(BasicStringView<CharT> a,
                                            BasicStringView<CharT> b) noexcept
    {
        if (a.Size() != b.Size())
        {
            return false;
        }
        for (usize i = 0; i < a.Size(); ++i)
        {
            if (a.Data()[i] != b.Data()[i])
            {
                return false;
            }
        }
        return true;
    }
    // A C-string literal can't deduce CharT for the view==view template, so a
    // dedicated overload covers `view == "lit"` (and, via the C++20 reversed
    // candidate, `"lit" == view`).
    template <typename CharT>
    [[nodiscard]] constexpr bool operator==(BasicStringView<CharT> a, const CharT* b) noexcept
    {
        return a == BasicStringView<CharT>{b};
    }
    template <typename CharT>
    [[nodiscard]] bool operator==(const BasicString<CharT>& a, const BasicString<CharT>& b) noexcept
    {
        return a.AsView() == b.AsView();
    }
    template <typename CharT>
    [[nodiscard]] bool operator==(const BasicString<CharT>& a, BasicStringView<CharT> b) noexcept
    {
        return a.AsView() == b;
    }
    template <typename CharT>
    [[nodiscard]] bool operator==(const BasicString<CharT>& a, const CharT* b) noexcept
    {
        return a.AsView() == BasicStringView<CharT>{b};
    }

    // =======================================================================
    // Aliases - String is UTF-8 (primary); WideString is UTF-16 (Win32 edge).
    // =======================================================================
    using StringView = BasicStringView<utf8char>;
    using String = BasicString<utf8char>;

    using WideStringView = BasicStringView<widechar>;
    using WideString = BasicString<widechar>;

    // =======================================================================
    // StringBuilder - incrementally builds a string, including numbers
    // (formatted as ASCII via <charconv>). Templated over the code unit:
    // `StringBuilder` is UTF-8 (primary), `WideStringBuilder` is UTF-16.
    // =======================================================================
    template <typename CharT>
    class BasicStringBuilder
    {
    public:
        BasicStringBuilder() = default;
        explicit BasicStringBuilder(IAllocator& allocator) : m_string(allocator) {}

        BasicStringBuilder& Append(BasicStringView<CharT> view)
        {
            m_string.Append(view);
            return *this;
        }
        BasicStringBuilder& Append(const CharT* str)
        {
            m_string.Append(BasicStringView<CharT>{str});
            return *this;
        }
        BasicStringBuilder& Append(CharT ch)
        {
            m_string.PushBack(ch);
            return *this;
        }

        // Appends an ASCII C-string (each byte maps to one code unit).
        BasicStringBuilder& AppendAscii(const char* str)
        {
            for (usize i = 0; str[i] != '\0'; ++i)
            {
                m_string.PushBack(static_cast<CharT>(static_cast<unsigned char>(str[i])));
            }
            return *this;
        }

        BasicStringBuilder& AppendInt(i64 value) { return AppendChars(value); }
        BasicStringBuilder& AppendUInt(u64 value) { return AppendChars(value); }
        BasicStringBuilder& AppendFloat(f64 value) { return AppendChars(value); }
        BasicStringBuilder& AppendBool(bool value) { return AppendAscii(value ? "true" : "false"); }

        void Clear() noexcept { m_string.Clear(); }
        [[nodiscard]] usize Size() const noexcept { return m_string.Size(); }
        [[nodiscard]] BasicStringView<CharT> View() const noexcept { return m_string.AsView(); }

        // Copy out, or move the built string out (leaving the builder empty).
        [[nodiscard]] const BasicString<CharT>& Str() const noexcept { return m_string; }
        [[nodiscard]] BasicString<CharT> Take() noexcept { return Move(m_string); }

    private:
        template <typename T>
        BasicStringBuilder& AppendChars(T value)
        {
            char temp[48];
            const std::to_chars_result result = std::to_chars(temp, temp + sizeof(temp), value);
            for (char* p = temp; p != result.ptr; ++p)
            {
                m_string.PushBack(static_cast<CharT>(static_cast<unsigned char>(*p)));
            }
            return *this;
        }

        BasicString<CharT> m_string;
    };

    using StringBuilder = BasicStringBuilder<utf8char>;
    using WideStringBuilder = BasicStringBuilder<widechar>;

    // =======================================================================
    // UTF-8 <-> UTF-16 transcoding. Invalid sequences become U+FFFD.
    // =======================================================================
    [[nodiscard]] inline WideString ToWide(StringView utf8,
                                           IAllocator& allocator = DefaultAllocator())
    {
        WideString result(allocator);
        const usize size = utf8.Size();
        usize i = 0;
        while (i < size)
        {
            const u8 lead = static_cast<u8>(utf8[i]);
            u32 codepoint;
            usize extra;
            if (lead < 0x80u)
            {
                codepoint = lead;
                extra = 0;
            }
            else if ((lead & 0xE0u) == 0xC0u)
            {
                codepoint = lead & 0x1Fu;
                extra = 1;
            }
            else if ((lead & 0xF0u) == 0xE0u)
            {
                codepoint = lead & 0x0Fu;
                extra = 2;
            }
            else if ((lead & 0xF8u) == 0xF0u)
            {
                codepoint = lead & 0x07u;
                extra = 3;
            }
            else
            {
                codepoint = 0xFFFDu;
                extra = 0;
            }
            ++i;

            bool valid = true;
            for (usize k = 0; k < extra; ++k)
            {
                if (i >= size || (static_cast<u8>(utf8[i]) & 0xC0u) != 0x80u)
                {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6) | (static_cast<u8>(utf8[i]) & 0x3Fu);
                ++i;
            }
            if (!valid)
            {
                codepoint = 0xFFFDu;
            }

            if (codepoint <= 0xFFFFu)
            {
                result.PushBack(static_cast<widechar>(codepoint));
            }
            else
            {
                codepoint -= 0x10000u;
                result.PushBack(static_cast<widechar>(0xD800u + (codepoint >> 10)));
                result.PushBack(static_cast<widechar>(0xDC00u + (codepoint & 0x3FFu)));
            }
        }
        return result;
    }

    [[nodiscard]] inline String ToUTF8(WideStringView wide,
                                       IAllocator& allocator = DefaultAllocator())
    {
        String result(allocator);
        const usize size = wide.Size();
        usize i = 0;
        while (i < size)
        {
            u32 codepoint = static_cast<u16>(wide[i]);
            ++i;
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) // high surrogate
            {
                if (i < size)
                {
                    const u16 low = static_cast<u16>(wide[i]);
                    if (low >= 0xDC00u && low <= 0xDFFFu)
                    {
                        codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
                        ++i;
                    }
                    else
                    {
                        codepoint = 0xFFFDu;
                    }
                }
                else
                {
                    codepoint = 0xFFFDu;
                }
            }
            else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu)
            {
                codepoint = 0xFFFDu;
            } // lone low

            if (codepoint < 0x80u)
            {
                result.PushBack(static_cast<utf8char>(codepoint));
            }
            else if (codepoint < 0x800u)
            {
                result.PushBack(static_cast<utf8char>(0xC0u | (codepoint >> 6)));
                result.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
            }
            else if (codepoint < 0x10000u)
            {
                result.PushBack(static_cast<utf8char>(0xE0u | (codepoint >> 12)));
                result.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
                result.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
            }
            else
            {
                result.PushBack(static_cast<utf8char>(0xF0u | (codepoint >> 18)));
                result.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
                result.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
                result.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
            }
        }
        return result;
    }
    // =======================================================================
    // UTF-8 codepoint iteration & encoding. Draconic String is UTF-8, and text
    // editing / shaping walks Unicode codepoints (char32_t). Mirrors Beef's
    // `StringView.DecodedChars` iteration. Invalid/truncated sequences decode to
    // U+FFFD (consuming one byte).
    // =======================================================================

    /// Decodes the codepoint starting at `text[index]`, advancing `index` past the
    /// consumed byte(s). After the call `index` is the byte offset of the next
    /// codepoint (Beef's `@c.NextIndex`). Caller guarantees `index < text.Size()`.
    [[nodiscard]] inline u32 DecodeUtf8(StringView text, usize& index) noexcept
    {
        const u8 lead = static_cast<u8>(text[index]);
        ++index;

        u32 codepoint;
        int extra;
        if (lead < 0x80u)
        {
            return lead;
        }
        else if ((lead & 0xE0u) == 0xC0u)
        {
            codepoint = lead & 0x1Fu;
            extra = 1;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            codepoint = lead & 0x0Fu;
            extra = 2;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            codepoint = lead & 0x07u;
            extra = 3;
        }
        else
        {
            return 0xFFFDu;
        } // invalid lead byte

        for (int k = 0; k < extra; ++k)
        {
            if (index >= text.Size())
            {
                return 0xFFFDu;
            }
            const u8 cont = static_cast<u8>(text[index]);
            if ((cont & 0xC0u) != 0x80u)
            {
                return 0xFFFDu;
            } // not a continuation byte
            codepoint = (codepoint << 6) | (cont & 0x3Fu);
            ++index;
        }
        return codepoint;
    }

    /// Appends the UTF-8 encoding of `codepoint` to `out`.
    inline void AppendUtf8(String& out, u32 codepoint)
    {
        if (codepoint < 0x80u)
        {
            out.PushBack(static_cast<utf8char>(codepoint));
        }
        else if (codepoint < 0x800u)
        {
            out.PushBack(static_cast<utf8char>(0xC0u | (codepoint >> 6)));
            out.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
        }
        else if (codepoint < 0x10000u)
        {
            out.PushBack(static_cast<utf8char>(0xE0u | (codepoint >> 12)));
            out.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            out.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
        }
        else
        {
            out.PushBack(static_cast<utf8char>(0xF0u | (codepoint >> 18)));
            out.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
            out.PushBack(static_cast<utf8char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            out.PushBack(static_cast<utf8char>(0x80u | (codepoint & 0x3Fu)));
        }
    }

    /// Number of Unicode codepoints in a UTF-8 view (not bytes).
    [[nodiscard]] inline usize Utf8Length(StringView text) noexcept
    {
        usize count = 0;
        usize i = 0;
        while (i < text.Size())
        {
            (void)DecodeUtf8(text, i);
            ++count;
        }
        return count;
    }

    // =======================================================================
    // Number parse/format + trim (over <charconv>). ASCII whitespace only.
    // =======================================================================

    /// Trim leading/trailing ASCII whitespace, returning a sub-view (non-owning).
    [[nodiscard]] inline StringView Trimmed(StringView s) noexcept
    {
        const auto isWs = [](utf8char c) noexcept
        {
            return c == utf8char(' ') || c == utf8char('\t') || c == utf8char('\n') ||
                   c == utf8char('\r');
        };
        usize start = 0, end = s.Size();
        while (start < end && isWs(s[start]))
        {
            ++start;
        }
        while (end > start && isWs(s[end - 1]))
        {
            --end;
        }
        return s.SubStr(start, end - start);
    }

    /// Parse a base-10 floating value from a UTF-8 view. None unless the whole
    /// trimmed view is a valid number (Beef `double.Parse` semantics).
    [[nodiscard]] inline Optional<f64> ParseFloat(StringView s) noexcept
    {
        const StringView t = Trimmed(s);
        if (t.IsEmpty())
        {
            return {};
        }
        const char* begin = reinterpret_cast<const char*>(t.Data());
        const char* end = begin + t.Size();
        f64 value = 0.0;
        const std::from_chars_result r = std::from_chars(begin, end, value);
        if (r.ec != std::errc{} || r.ptr != end)
        {
            return {};
        }
        return value;
    }

    /// Parse a base-10 integer from a UTF-8 view. None unless the whole trimmed view is valid.
    [[nodiscard]] inline Optional<i64> ParseInt(StringView s) noexcept
    {
        const StringView t = Trimmed(s);
        if (t.IsEmpty())
        {
            return {};
        }
        const char* begin = reinterpret_cast<const char*>(t.Data());
        const char* end = begin + t.Size();
        i64 value = 0;
        const std::from_chars_result r = std::from_chars(begin, end, value);
        if (r.ec != std::errc{} || r.ptr != end)
        {
            return {};
        }
        return value;
    }

    /// Format `value` with a fixed number of decimal places (like printf %.*f; `decimals` 0 = integer).
    [[nodiscard]] inline String FormatFixed(f64 value, i32 decimals,
                                            IAllocator& allocator = DefaultAllocator())
    {
        char temp[64];
        const std::to_chars_result r =
            std::to_chars(temp, temp + sizeof(temp), value, std::chars_format::fixed,
                          decimals < 0 ? 0 : decimals);
        String out(allocator);
        out.Append(reinterpret_cast<const utf8char*>(temp), static_cast<usize>(r.ptr - temp));
        return out;
    }

    // Hash specializations so the string types work as hashed-container keys.
    // (The Hash<T> primary template + HashBytes live in :hash.)
    template <typename CharT>
    struct Hash<BasicStringView<CharT>>
    {
        [[nodiscard]] u64 operator()(BasicStringView<CharT> view) const noexcept
        {
            return HashBytes(view.Data(), view.Size() * sizeof(CharT));
        }
    };

    template <typename CharT>
    struct Hash<BasicString<CharT>>
    {
        [[nodiscard]] u64 operator()(const BasicString<CharT>& str) const noexcept
        {
            return HashBytes(str.Data(), str.Size() * sizeof(CharT));
        }
    };
}
