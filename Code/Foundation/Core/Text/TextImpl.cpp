// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :string implementation unit
//
// Holds the one place Core turns a floating-point value into ASCII. It lives here rather
// than in the :string / :format interfaces because std::to_chars' floating-point path
// instantiates STL lookup tables whose static data members cannot cross a shared-library
// boundary: once that instantiation is reachable through a module interface, a consumer
// references std::_General_precision_tables_2<double>::_Max_P (and siblings) and expects
// the producing DLL to provide them, but exported data is only readable through a
// declaration that says dllimport, which the single-BMI model cannot express. Every
// consumer of a shared Core would then fail to link on those symbols.
//
// Keeping the instantiation in this unit means Core owns them privately and no consumer
// needs them at all. Integral to_chars has no such tables and stays inline in the
// interface.

module;
#include "Core/Prelude.h"

#include <charconv>

module foundation.core;

import :base;

namespace foundation::core::detail
{
    usize FloatToChars(char* buffer, usize capacity, f64 value) noexcept
    {
        const std::to_chars_result result = std::to_chars(buffer, buffer + capacity, value);
        if (result.ec != std::errc{})
        {
            return 0;
        }
        return static_cast<usize>(result.ptr - buffer);
    }

    usize FloatToChars(char* buffer, usize capacity, f32 value) noexcept
    {
        const std::to_chars_result result = std::to_chars(buffer, buffer + capacity, value);
        if (result.ec != std::errc{})
        {
            return 0;
        }
        return static_cast<usize>(result.ptr - buffer);
    }

    bool FloatFromChars(const char* begin, const char* end, f64& out) noexcept
    {
        const std::from_chars_result result = std::from_chars(begin, end, out);
        // All-or-nothing, matching ParseFloat's contract: trailing junk is a parse failure.
        return result.ec == std::errc{} && result.ptr == end;
    }

    usize FloatToCharsFixed(char* buffer, usize capacity, f64 value, i32 decimals) noexcept
    {
        const std::to_chars_result result =
            std::to_chars(buffer, buffer + capacity, value, std::chars_format::fixed,
                          decimals < 0 ? 0 : decimals);
        if (result.ec != std::errc{})
        {
            return 0;
        }
        return static_cast<usize>(result.ptr - buffer);
    }
}
