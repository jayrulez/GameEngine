// Draconic Foundation - :format partition
//
// Lightweight typesafe text formatting (no iostreams, no exceptions). Output
// targets a growable, allocator-backed UTF-8 buffer; `{}` marks a substitution,
// `{{`/`}}` are literal braces. Numbers go through <charconv> (std::to_chars,
// allocation-free, locale-independent) and their ASCII digits append directly.
// UTF-8 is the API surface; sinks write bytes straight to the OS edge.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <charconv>
#include <type_traits>

export module draconic.foundation:format;

import :base;
import :allocator;
import :string;
import :guid;

export namespace draconic::foundation
{
    class FormatBuffer
    {
    public:
        FormatBuffer() noexcept : m_allocator(&DefaultAllocator()) {}
        explicit FormatBuffer(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        FormatBuffer(const FormatBuffer&) = delete;
        FormatBuffer& operator=(const FormatBuffer&) = delete;

        ~FormatBuffer()
        {
            if (m_data != nullptr)
            {
                m_allocator->Free(m_data);
            }
        }

        void Append(utf8char c)
        {
            EnsureCapacity(m_size + 1);
            m_data[m_size++] = c;
            m_data[m_size] = u8'\0';
        }

        void Append(const utf8char* text, usize length)
        {
            if (length == 0)
            {
                return;
            }
            EnsureCapacity(m_size + length);
            MemCopy(m_data + m_size, text, length * sizeof(utf8char));
            m_size += length;
            m_data[m_size] = u8'\0';
        }

        void Append(const utf8char* cstr)
        {
            usize length = 0;
            while (cstr[length] != u8'\0')
            {
                ++length;
            }
            Append(cstr, length);
        }

        void Clear() noexcept
        {
            m_size = 0;
            if (m_data != nullptr)
            {
                m_data[0] = u8'\0';
            }
        }

        [[nodiscard]] const utf8char* Data() const noexcept
        {
            return m_data != nullptr ? m_data : u8"";
        }
        [[nodiscard]] const utf8char* CStr() const noexcept { return Data(); }
        [[nodiscard]] StringView View() const noexcept { return StringView{Data(), m_size}; }
        [[nodiscard]] usize Size() const noexcept { return m_size; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }

    private:
        void EnsureCapacity(usize required)
        {
            if (required + 1 <= m_capacity)
            {
                return;
            }

            usize newCapacity = (m_capacity == 0) ? 64 : m_capacity * 2;
            if (newCapacity < required + 1)
            {
                newCapacity = required + 1;
            }

            utf8char* newData = static_cast<utf8char*>(
                m_allocator->Allocate(newCapacity * sizeof(utf8char), alignof(utf8char)));
            DRACONIC_ASSERT_MSG(newData != nullptr, "FormatBuffer allocation failed");

            if (m_data != nullptr)
            {
                MemCopy(newData, m_data, (m_size + 1) * sizeof(utf8char));
                m_allocator->Free(m_data);
            }
            else
            {
                newData[0] = u8'\0';
            }
            m_data = newData;
            m_capacity = newCapacity;
        }

        utf8char* m_data = nullptr;
        usize m_size = 0;
        usize m_capacity = 0;
        IAllocator* m_allocator = nullptr;
    };

    // The appenders/format core are generic over the output sink: any type with
    // Append(utf8char), Append(const utf8char*) and Append(const utf8char*, usize)
    // works. FormatBuffer is one such sink; String is another (so formatting can
    // write straight into a String, no scratch buffer).
    namespace detail
    {
        // Append a run of ASCII bytes (digits/hex from <charconv>) to the sink.
        template <typename Sink>
        void AppendAsciiDigits(Sink& out, const char* text, usize length)
        {
            for (usize i = 0; i < length; ++i)
            {
                out.Append(static_cast<utf8char>(static_cast<unsigned char>(text[i])));
            }
        }

        // Append a (BMP-or-above) codepoint to a UTF-8 sink.
        template <typename Sink>
        void AppendCodepoint(Sink& out, u32 cp)
        {
            if (cp < 0x80u)
            {
                out.Append(static_cast<utf8char>(cp));
            }
            else if (cp < 0x800u)
            {
                out.Append(static_cast<utf8char>(0xC0u | (cp >> 6)));
                out.Append(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
            else if (cp < 0x10000u)
            {
                out.Append(static_cast<utf8char>(0xE0u | (cp >> 12)));
                out.Append(static_cast<utf8char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.Append(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
            else
            {
                out.Append(static_cast<utf8char>(0xF0u | (cp >> 18)));
                out.Append(static_cast<utf8char>(0x80u | ((cp >> 12) & 0x3Fu)));
                out.Append(static_cast<utf8char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.Append(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
        }
    }

    // --- per-type appenders ------------------------------------------------
    template <typename Sink>
    void AppendValue(Sink& out, bool value)
    {
        out.Append(value ? u8"true" : u8"false");
    }

    template <typename Sink>
    void AppendValue(Sink& out, char value)
    {
        out.Append(static_cast<utf8char>(static_cast<unsigned char>(value)));
    }

    template <typename Sink>
    void AppendValue(Sink& out, utf8char value)
    {
        out.Append(value);
    }

    // A wide code unit (UTF-16): encode to UTF-8.
    template <typename Sink>
    void AppendValue(Sink& out, widechar value)
    {
        detail::AppendCodepoint(out, static_cast<u32>(static_cast<u16>(value)));
    }

    template <typename Sink>
    void AppendValue(Sink& out, const utf8char* value)
    {
        out.Append(value != nullptr ? value : u8"(null)");
    }

    template <typename Sink, typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
                 !std::is_same_v<T, widechar> && !std::is_same_v<T, char8_t>)
    void AppendValue(Sink& out, T value)
    {
        char temp[32];
        const std::to_chars_result result = std::to_chars(temp, temp + sizeof(temp), value);
        detail::AppendAsciiDigits(out, temp, static_cast<usize>(result.ptr - temp));
    }

    template <typename Sink, typename T>
        requires std::is_floating_point_v<T>
    void AppendValue(Sink& out, T value)
    {
        char temp[48];
        const std::to_chars_result result = std::to_chars(temp, temp + sizeof(temp), value);
        detail::AppendAsciiDigits(out, temp, static_cast<usize>(result.ptr - temp));
    }

    // UTF-8 view: append directly.
    template <typename Sink>
    void AppendValue(Sink& out, StringView view)
    {
        out.Append(view.Data(), view.Size());
    }

    // Wide view (and WideString, via its implicit View conversion): transcode.
    template <typename Sink>
    void AppendValue(Sink& out, WideStringView view)
    {
        const String utf8 = ToUTF8(view);
        out.Append(utf8.Data(), utf8.Size());
    }

    template <typename Sink>
    void AppendValue(Sink& out, Guid value)
    {
        utf8char text[37];
        value.ToChars(text);
        out.Append(text, 36);
    }

    template <typename Sink>
    void AppendValue(Sink& out, const void* value)
    {
        out.Append(u8"0x");
        char temp[20];
        const auto address = static_cast<u64>(reinterpret_cast<uptr>(value));
        const std::to_chars_result result = std::to_chars(temp, temp + sizeof(temp), address, 16);
        detail::AppendAsciiDigits(out, temp, static_cast<usize>(result.ptr - temp));
    }

    // --- format core (generic over the output sink) ------------------------
    // --- runtime format (FormatToV): fmt is a UTF-8 const utf8char* --------
    template <typename Sink>
    void FormatToV(Sink& out, const utf8char* fmt)
    {
        // No remaining args: copy the rest, honouring {{ and }} escapes.
        while (*fmt != u8'\0')
        {
            if (fmt[0] == u8'{' && fmt[1] == u8'{')
            {
                out.Append(u8'{');
                fmt += 2;
            }
            else if (fmt[0] == u8'}' && fmt[1] == u8'}')
            {
                out.Append(u8'}');
                fmt += 2;
            }
            else
            {
                out.Append(*fmt++);
            }
        }
    }

    template <typename Sink, typename T, typename... Rest>
    void FormatToV(Sink& out, const utf8char* fmt, const T& value, const Rest&... rest)
    {
        while (*fmt != u8'\0')
        {
            if (fmt[0] == u8'{' && fmt[1] == u8'}')
            {
                AppendValue(out, value);
                FormatToV(out, fmt + 2, rest...);
                return;
            }
            if (fmt[0] == u8'{' && fmt[1] == u8'{')
            {
                out.Append(u8'{');
                fmt += 2;
                continue;
            }
            if (fmt[0] == u8'}' && fmt[1] == u8'}')
            {
                out.Append(u8'}');
                fmt += 2;
                continue;
            }
            out.Append(*fmt++);
        }
        // More args than `{}` placeholders: extra args are ignored.
    }

    // --- compile-time-checked format ---------------------------------------
    namespace detail
    {
        // Calling a non-constexpr function inside a consteval context is a
        // compile error; the name is what shows up in the diagnostic.
        inline void Format_argument_count_does_not_match_the_format_string() {}
    }

    // A format string whose `{}` count is validated at compile time against the
    // argument pack. Constructed implicitly from a UTF-8 string literal.
    template <typename... Args>
    struct BasicFormatString
    {
        const utf8char* data;

        template <usize N>
        consteval BasicFormatString(const utf8char (&str)[N]) : data(str)
        {
            usize placeholders = 0;
            usize i = 0;
            while (i + 1 < N)
            {
                const utf8char c0 = str[i];
                const utf8char c1 = str[i + 1];
                if (c0 == u8'{' && c1 == u8'{')
                {
                    i += 2;
                }
                else if (c0 == u8'}' && c1 == u8'}')
                {
                    i += 2;
                }
                else if (c0 == u8'{' && c1 == u8'}')
                {
                    ++placeholders;
                    i += 2;
                }
                else
                {
                    ++i;
                }
            }
            if (placeholders != sizeof...(Args))
            {
                detail::Format_argument_count_does_not_match_the_format_string();
            }
        }
    };

    template <typename... Args>
    using FormatString = BasicFormatString<std::type_identity_t<Args>...>;

    // Checked entry point: a literal format string's placeholder count must
    // match the argument count (verified by FormatString's consteval ctor).
    template <typename... Args>
    void FormatTo(FormatBuffer& out, FormatString<Args...> fmt, const Args&... args)
    {
        FormatToV(out, fmt.data, args...);
    }

    // Append formatted text straight into a String (no scratch buffer).
    template <typename... Args>
    void AppendFormat(String& out, FormatString<Args...> fmt, const Args&... args)
    {
        FormatToV(out, fmt.data, args...);
    }

    // Build a new String from a format string and arguments.
    template <typename... Args>
    [[nodiscard]] String Format(FormatString<Args...> fmt, const Args&... args)
    {
        String out;
        FormatToV(out, fmt.data, args...);
        return out;
    }
}
