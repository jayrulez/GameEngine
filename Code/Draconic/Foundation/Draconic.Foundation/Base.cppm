// Draconic Foundation - :base partition
//
// The foundation: fundamental exported types, widely-used utilities, and the
// project-wide error vocabulary (Status / Result). Lives at the Foundation root.
// Macros live in Prelude.h, not here - modules cannot export macros.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h" // classic header - no module cycle (see Assert.h)
#include <bit>                 // std::byteswap
#include <cstdint>
#include <cstddef>
#include <new> // placement new
#include <type_traits>

export module draconic.foundation:base;

export namespace draconic::foundation
{
    // =======================================================================
    // Fundamental integer / floating types
    // =======================================================================
    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    using f32 = float;
    using f64 = double;

    using usize = std::size_t;
    using isize = std::ptrdiff_t;

    using iptr = std::intptr_t;
    using uptr = std::uintptr_t;

    using byte = std::byte;

    // Wide character unit for the primary String type (UTF-16); String uses
    // char8_t. See Documentation/Planning/Core.md §4.5 / §7.
    using widechar = char16_t;
    using utf8char = char8_t;

    // =======================================================================
    // Move / Forward / Swap
    // (our own, to avoid pulling <utility> into every consumer)
    // =======================================================================
    template <typename T>
    [[nodiscard]] constexpr std::remove_reference_t<T>&& Move(T&& value) noexcept
    {
        return static_cast<std::remove_reference_t<T>&&>(value);
    }

    template <typename T>
    [[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>& value) noexcept
    {
        return static_cast<T&&>(value);
    }

    template <typename T>
    [[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>&& value) noexcept
    {
        static_assert(!std::is_lvalue_reference_v<T>,
                      "Forward must not be used to forward an rvalue as an lvalue.");
        return static_cast<T&&>(value);
    }

    template <typename T>
    constexpr void Swap(T& a, T& b) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                             std::is_nothrow_move_assignable_v<T>)
    {
        T tmp = Move(a);
        a = Move(b);
        b = Move(tmp);
    }

    // =======================================================================
    // Small utilities
    // =======================================================================
    template <typename T>
    [[nodiscard]] constexpr const T& Min(const T& a, const T& b)
    {
        return (b < a) ? b : a;
    }

    template <typename T>
    [[nodiscard]] constexpr const T& Max(const T& a, const T& b)
    {
        return (a < b) ? b : a;
    }

    template <typename T>
    [[nodiscard]] constexpr const T& Clamp(const T& v, const T& lo, const T& hi)
    {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }

    template <typename T, usize N>
    [[nodiscard]] constexpr usize ArrayCount(const T (&)[N]) noexcept
    {
        return N;
    }

    // =======================================================================
    // Byte order
    // =======================================================================
    inline constexpr bool kIsLittleEndian = DRACONIC_LITTLE_ENDIAN != 0;

    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] constexpr T ByteSwap(T value) noexcept
    {
        return std::byteswap(value);
    }

    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] constexpr T NativeToLittle(T value) noexcept
    {
        if constexpr (kIsLittleEndian)
        {
            return value;
        }
        else
        {
            return ByteSwap(value);
        }
    }

    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] constexpr T NativeToBig(T value) noexcept
    {
        if constexpr (kIsLittleEndian)
        {
            return ByteSwap(value);
        }
        else
        {
            return value;
        }
    }

    // Conversions are symmetric (swap-or-not), so reuse them by name.
    template <typename T>
    [[nodiscard]] constexpr T LittleToNative(T value) noexcept
    {
        return NativeToLittle(value);
    }
    template <typename T>
    [[nodiscard]] constexpr T BigToNative(T value) noexcept
    {
        return NativeToBig(value);
    }

    // =======================================================================
    // Ownership mixins
    // =======================================================================
    class NonCopyable
    {
    protected:
        constexpr NonCopyable() = default;
        ~NonCopyable() = default;

    public:
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
    };

    class NonMovable
    {
    protected:
        constexpr NonMovable() = default;
        ~NonMovable() = default;

    public:
        NonMovable(const NonMovable&) = delete;
        NonMovable& operator=(const NonMovable&) = delete;
        NonMovable(NonMovable&&) = delete;
        NonMovable& operator=(NonMovable&&) = delete;
    };

    // =======================================================================
    // Error vocabulary (exceptions are disabled engine-wide)
    //   Status      - success or an error code, no payload.
    //   Result<T,E> - a value (T) or an error (E). Use Err(e) to build the
    //                 error case; a T converts implicitly to the value case.
    // =======================================================================
    enum class ErrorCode : u32
    {
        Ok = 0,
        Unknown,
        InvalidArgument,
        OutOfRange,
        OutOfMemory,
        NotFound,
        NotSupported,
        AlreadyExists,
        Internal,
    };

    class Status
    {
    public:
        constexpr Status() = default;
        constexpr Status(ErrorCode code) : m_code(code) {}

        [[nodiscard]] constexpr ErrorCode Code() const { return m_code; }
        [[nodiscard]] constexpr bool IsOk() const { return m_code == ErrorCode::Ok; }
        [[nodiscard]] constexpr explicit operator bool() const { return IsOk(); }

        // NB: explicit, not `= default` - GCC 15 ICEs on defaulted comparison
        // operators inside a module.
        friend constexpr bool operator==(Status a, Status b) { return a.m_code == b.m_code; }

    private:
        ErrorCode m_code = ErrorCode::Ok;
    };

    // Tag wrapper that disambiguates the error case of Result.
    template <typename E>
    struct Failure
    {
        E error;
    };

    template <typename E>
    [[nodiscard]] constexpr Failure<std::remove_cvref_t<E>> Err(E&& error)
    {
        return Failure<std::remove_cvref_t<E>>{Forward<E>(error)};
    }

    template <typename T, typename E = ErrorCode>
    class Result
    {
        static_assert(!std::is_void_v<T>, "Use Status for operations that return no value.");

    public:
        using ValueType = T;
        using ErrorType = E;

        // Value case (implicit from T).
        Result(const T& value) : m_hasValue(true) { ::new (&m_value) T(value); }
        Result(T&& value) : m_hasValue(true) { ::new (&m_value) T(Move(value)); }

        // Error case (from Err(...)).
        Result(Failure<E> failure) : m_hasValue(false) { ::new (&m_error) E(Move(failure.error)); }

        Result(const Result& other) : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                ::new (&m_value) T(other.m_value);
            }
            else
            {
                ::new (&m_error) E(other.m_error);
            }
        }

        Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                        std::is_nothrow_move_constructible_v<E>)
            : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                ::new (&m_value) T(Move(other.m_value));
            }
            else
            {
                ::new (&m_error) E(Move(other.m_error));
            }
        }

        Result& operator=(const Result& other)
        {
            if (this != &other)
            {
                Destroy();
                m_hasValue = other.m_hasValue;
                if (m_hasValue)
                {
                    ::new (&m_value) T(other.m_value);
                }
                else
                {
                    ::new (&m_error) E(other.m_error);
                }
            }
            return *this;
        }

        Result& operator=(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                   std::is_nothrow_move_constructible_v<E>)
        {
            if (this != &other)
            {
                Destroy();
                m_hasValue = other.m_hasValue;
                if (m_hasValue)
                {
                    ::new (&m_value) T(Move(other.m_value));
                }
                else
                {
                    ::new (&m_error) E(Move(other.m_error));
                }
            }
            return *this;
        }

        ~Result() { Destroy(); }

        [[nodiscard]] bool HasValue() const { return m_hasValue; }
        [[nodiscard]] explicit operator bool() const { return m_hasValue; }

        [[nodiscard]] T& Value() &
        {
            DRACONIC_ASSERT_MSG(m_hasValue, "Result::Value() called on an error Result");
            return m_value;
        }
        [[nodiscard]] const T& Value() const&
        {
            DRACONIC_ASSERT_MSG(m_hasValue, "Result::Value() called on an error Result");
            return m_value;
        }
        [[nodiscard]] T&& Value() &&
        {
            DRACONIC_ASSERT_MSG(m_hasValue, "Result::Value() called on an error Result");
            return Move(m_value);
        }

        [[nodiscard]] E& Error() &
        {
            DRACONIC_ASSERT_MSG(!m_hasValue, "Result::Error() called on a value Result");
            return m_error;
        }
        [[nodiscard]] const E& Error() const&
        {
            DRACONIC_ASSERT_MSG(!m_hasValue, "Result::Error() called on a value Result");
            return m_error;
        }

        [[nodiscard]] T ValueOr(T fallback) const& { return m_hasValue ? m_value : Move(fallback); }

    private:
        void Destroy()
        {
            if (m_hasValue)
            {
                m_value.~T();
            }
            else
            {
                m_error.~E();
            }
        }

        bool m_hasValue;
        union
        {
            T m_value;
            E m_error;
        };
    };

    // =======================================================================
    // Optional<T> - a value that may be absent. (Use Result when the absence
    // carries an error; Optional when absence is ordinary.)
    // =======================================================================
    struct NullOptType
    {
        explicit constexpr NullOptType() = default;
    };
    inline constexpr NullOptType NullOpt{};

    template <typename T>
    class Optional
    {
    public:
        Optional() noexcept : m_hasValue(false) {}
        Optional(NullOptType) noexcept : m_hasValue(false) {}

        Optional(const T& value) : m_hasValue(true) { ::new (&m_value) T(value); }
        Optional(T&& value) : m_hasValue(true) { ::new (&m_value) T(Move(value)); }

        Optional(const Optional& other) : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                ::new (&m_value) T(other.m_value);
            }
        }

        Optional(Optional&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
            : m_hasValue(other.m_hasValue)
        {
            if (m_hasValue)
            {
                ::new (&m_value) T(Move(other.m_value));
            }
        }

        Optional& operator=(const Optional& other)
        {
            if (this != &other)
            {
                Reset();
                m_hasValue = other.m_hasValue;
                if (m_hasValue)
                {
                    ::new (&m_value) T(other.m_value);
                }
            }
            return *this;
        }

        Optional& operator=(Optional&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (this != &other)
            {
                Reset();
                m_hasValue = other.m_hasValue;
                if (m_hasValue)
                {
                    ::new (&m_value) T(Move(other.m_value));
                }
            }
            return *this;
        }

        ~Optional() { Reset(); }

        void Reset() noexcept
        {
            if (m_hasValue)
            {
                m_value.~T();
                m_hasValue = false;
            }
        }

        template <typename... Args>
        T& Emplace(Args&&... args)
        {
            Reset();
            ::new (&m_value) T(Forward<Args>(args)...);
            m_hasValue = true;
            return m_value;
        }

        [[nodiscard]] bool HasValue() const noexcept { return m_hasValue; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_hasValue; }

        [[nodiscard]] T& Value() & { return m_value; }
        [[nodiscard]] const T& Value() const& { return m_value; }
        [[nodiscard]] T&& Value() && { return Move(m_value); }

        [[nodiscard]] T* operator->() noexcept { return &m_value; }
        [[nodiscard]] const T* operator->() const noexcept { return &m_value; }
        [[nodiscard]] T& operator*() & noexcept { return m_value; }
        [[nodiscard]] const T& operator*() const& noexcept { return m_value; }

        [[nodiscard]] T ValueOr(T fallback) const& { return m_hasValue ? m_value : Move(fallback); }

    private:
        bool m_hasValue;
        union
        {
            T m_value;
        };
    };

    /// Two Optionals are equal iff both empty, or both engaged with equal values (requires T to be
    /// equality-comparable; instantiated only where used).
    template <typename T>
    [[nodiscard]] inline bool operator==(const Optional<T>& a, const Optional<T>& b)
    {
        if (a.HasValue() != b.HasValue())
        {
            return false;
        }
        return !a.HasValue() || a.Value() == b.Value();
    }
    template <typename T>
    [[nodiscard]] inline bool operator!=(const Optional<T>& a, const Optional<T>& b)
    {
        return !(a == b);
    }
}
