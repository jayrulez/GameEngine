// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - :scripted_input_json partition (implementation).

module;
#include "Core/Prelude.h"

module foundation.input;

import foundation.core;
import foundation.json;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace foundation::input
{
    namespace
    {
        Result<Float2, String> ReadPair(const JsonValue& value, usize i, StringView name)
        {
            if (!value.IsArray() || value.Count() != 2 || !value.At(0).IsNumber() || !value.At(1).IsNumber())
                return Err(Format(u8"input[{}]: `{}` takes [x, y]", i, name));
            return Float2{static_cast<f32>(value.At(0).AsNumber()), static_cast<f32>(value.At(1).AsNumber())};
        }
    }

    Result<ScriptedInput, String> ParseScriptedInput(const JsonValue& entry, usize i)
    {
        ScriptedInput out;
        if (!entry.IsObject())
            return Err(Format(u8"input[{}]: an object", i));
        const JsonValue at = entry.Get(u8"at");
        if (!at.IsNumber() || at.AsNumber() < 0.0)
            return Err(Format(u8"input[{}]: `at` takes run seconds from 0", i));
        out.at = at.AsNumber();
        out.down = entry.Get(u8"down").AsBool(true);
        out.gamepad = static_cast<i32>(entry.Get(u8"gamepad").AsInt(0));

        // The names are the enums' reflected case names, any case; type_info lists them.
        if (entry.Has(u8"key"))
        {
            const String name = entry.Get(u8"key").AsString();
            out.kind = ScriptedInputKind::Key;
            if (!ScriptedInputSource::ParseKey(name.AsView(), out.key))
                return Err(Format(u8"input[{}]: no key '{}' (the KeyCode case names: type_info KeyCode lists them)", i, name.AsView()));
            return out;
        }
        if (entry.Has(u8"mouseButton"))
        {
            const String name = entry.Get(u8"mouseButton").AsString();
            out.kind = ScriptedInputKind::MouseButton;
            if (!ScriptedInputSource::ParseMouseButton(name.AsView(), out.button))
                return Err(Format(u8"input[{}]: no mouse button '{}' (type_info MouseButton lists them)", i, name.AsView()));
            return out;
        }
        if (entry.Has(u8"mouseMove") || entry.Has(u8"wheel"))
        {
            const bool move = entry.Has(u8"mouseMove");
            const StringView name = move ? StringView(u8"mouseMove") : StringView(u8"wheel");
            out.kind = move ? ScriptedInputKind::MouseMove : ScriptedInputKind::MouseWheel;
            Result<Float2, String> pair = ReadPair(entry.Get(String(name)), i, name);
            if (!pair.HasValue())
                return Err(Move(pair.Error()));
            out.x = pair.Value().x;
            out.y = pair.Value().y;
            return out;
        }
        if (entry.Has(u8"button"))
        {
            const String name = entry.Get(u8"button").AsString();
            out.kind = ScriptedInputKind::PadButton;
            if (!ScriptedInputSource::ParsePadButton(name.AsView(), out.padButton))
                return Err(Format(u8"input[{}]: no gamepad button '{}' (type_info GamepadButton lists them)", i, name.AsView()));
            return out;
        }
        if (entry.Has(u8"axis"))
        {
            const String name = entry.Get(u8"axis").AsString();
            out.kind = ScriptedInputKind::PadAxis;
            if (!ScriptedInputSource::ParsePadAxis(name.AsView(), out.padAxis))
                return Err(Format(u8"input[{}]: no gamepad axis '{}' (type_info GamepadAxis lists them)", i, name.AsView()));
            const JsonValue value = entry.Get(u8"value");
            if (!value.IsNumber() || value.AsNumber() < -1.0 || value.AsNumber() > 1.0)
                return Err(Format(u8"input[{}]: an axis takes `value` from -1 to 1", i));
            out.value = static_cast<f32>(value.AsNumber());
            return out;
        }
        return Err(Format(u8"input[{}]: name one of `key`, `mouseButton`, `mouseMove`, `wheel`, `button` or `axis`", i));
    }

    Result<usize, String> AddScriptedTimeline(const JsonValue& timeline, f64 duration, ScriptedInputSource& source)
    {
        if (!timeline.IsArray())
            return Err(String(u8"`input` takes an array of timeline entries"));
        for (i64 i = 0; i < timeline.Count(); ++i)
        {
            Result<ScriptedInput, String> entry = ParseScriptedInput(timeline.At(i), static_cast<usize>(i));
            if (!entry.HasValue())
                return Err(Move(entry.Error()));
            if (entry.Value().at > duration)
                return Err(Format(u8"input[{}]: `at` {} is after the run's end ({})", i, entry.Value().at, duration));
            if (!source.Add(entry.Value()))
                return Err(Format(u8"input[{}]: `gamepad` takes 0 to {}", i, ScriptedInputSource::kMaxGamepads - 1));
        }
        return source.Count();
    }
}
