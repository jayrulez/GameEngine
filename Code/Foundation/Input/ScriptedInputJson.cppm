// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - :scripted_input_json partition
//
// A scripted input timeline read from JSON, as the playtest tools take one (pie_run into a Game tab,
// viewport_run into a scene view): each entry `{at, key, down}`, `{at, mouseButton, down}`,
// `{at, mouseMove: [x, y]}`, `{at, wheel: [x, y]}`, `{at, gamepad, button, down}` or
// `{at, gamepad, axis, value}`, the names the enums' reflected case names in any case. One reader,
// so the tools take the same timeline and say the same of a wrong one.

module;
#include "Core/Prelude.h"

export module foundation.input:scripted_input_json;

import foundation.core;
import foundation.json;
import :scripted_input;

using namespace foundation::core;

export namespace foundation::input
{
    /// One timeline entry; the error names it as `input[index]`.
    [[nodiscard]] Result<ScriptedInput, String> ParseScriptedInput(const json::JsonValue& entry, usize index);

    /// Every entry of `timeline` (a JSON array) into `source`, none after `duration` seconds; the
    /// first wrong entry's error, nothing added past it.
    [[nodiscard]] Result<usize, String> AddScriptedTimeline(const json::JsonValue& timeline, f64 duration, ScriptedInputSource& source);
}
