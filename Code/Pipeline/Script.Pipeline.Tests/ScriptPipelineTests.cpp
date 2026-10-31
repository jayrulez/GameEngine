// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// script.pipeline's neutral cook helpers: the class-name/handler scan and the shared
// startCoroutine detection. These are pure text scanners in the backend-neutral Pipeline::Script
// library (no VM, no language), so they always build. The per-language neutral-builder cook
// round-trips (source through builder to factory) live in AngelScriptPipelineTests.cpp and
// LuauPipelineTests.cpp, each compiled only when its backend is on.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import script.pipeline;

using namespace foundation::core;
using namespace pipeline;

TEST_CASE("script.pipeline: class-name scan prefers the file stem, falls back to the "
          "first class, ignores comments")
{
    CHECK(FindScriptClassName(u8"class Mover {\n}\n", u8"Mover") == u8"Mover");
    CHECK(FindScriptClassName(u8"class Helper {\n}\nclass Mover {\n}\n", u8"Mover") == u8"Mover");
    CHECK(FindScriptClassName(u8"class Helper {\n}\nclass Other {\n}\n", u8"Mover") == u8"Helper");
    CHECK(FindScriptClassName(u8"// class Fake {\nclass Real {\n}\n", u8"nope") == u8"Real");
    CHECK(FindScriptClassName(u8"/* class Fake { */\nclass Real {\n}\n", u8"nope") == u8"Real");
    CHECK(FindScriptClassName(u8"var x = 1\n", u8"Mover").IsEmpty());
}

TEST_CASE("script.pipeline: handler scan finds declared handlers only (comments stripped)")
{
    const Array<String> handlers = ScanScriptHandlers(u8"class A {\n"
                                                      u8"    onStart() {}\n"
                                                      u8"    onUpdate(dt) {}\n"
                                                      u8"    // onDestroy() would be nice\n"
                                                      u8"    /* onEnable() {} */\n"
                                                      u8"}\n");
    auto has = [&handlers](StringView name)
    {
        for (const String& h : handlers)
        {
            if (h.AsView() == name)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has(u8"onStart"));
    CHECK(has(u8"onUpdate"));
    CHECK_FALSE(has(u8"onDestroy"));
    CHECK_FALSE(has(u8"onEnable"));
    CHECK_FALSE(has(u8"onDisable"));
}

TEST_CASE("script.pipeline: handler scan captures the whole on<Upper>(...) convention - "
          "custom message + event handlers, not lookalikes (P2)")
{
    const Array<String> handlers =
        ScanScriptHandlers(u8"class A {\n"
                           u8"    onStart() {}\n"
                           u8"    onHeal(amount) {}\n" // custom message handler (entity.send)
                           u8"    onContactBegin(o, p, n) {}\n" // physics event handler
                           u8"    onlyOnce() {}\n" // lowercase after 'on' - NOT a handler
                           u8"    onFoo {}\n"      // getter (no parens) - NOT a handler
                           u8"    speed=(v) {}\n"  // setter - NOT a handler
                           u8"}\n");
    auto has = [&handlers](StringView name)
    {
        for (const String& h : handlers)
        {
            if (h.AsView() == name)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has(u8"onStart"));
    CHECK(has(u8"onHeal"));
    CHECK(has(u8"onContactBegin"));
    CHECK_FALSE(has(u8"onlyOnce"));
    CHECK_FALSE(has(u8"onFoo"));
    CHECK(handlers.Size() == 3u);
}

TEST_CASE("script.pipeline: the shared startCoroutine( surface is detected neutrally "
          "(comments ignored) - both languages reuse it")
{
    // The shared coroutine-start token (a facade convention, not language syntax).
    CHECK(ScriptReferencesCoroutineStart(
        u8"class Mover {\n    onStart() { startCoroutine(Fn.new {}) }\n}\n"));
    CHECK(ScriptReferencesCoroutineStart(u8"void begin() { startCoroutine(@this.Run); }\n"));
    // A plain behavior does not reference it.
    CHECK_FALSE(ScriptReferencesCoroutineStart(u8"class Mover {\n    onUpdate(dt) {}\n}\n"));
    // Only in real code, not a comment.
    CHECK_FALSE(ScriptReferencesCoroutineStart(u8"// startCoroutine(nope)\nclass Mover {\n}\n"));
}
