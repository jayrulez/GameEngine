// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the code languages: each of the shared definitions (Data/Editor/Code) read by
// its id the first time it is asked for and kept, one with no file none (and not read again).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui.code;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace
{
    // The data root's files, as the editor's resources read them; `reads` counts the asks.
    Function<bool(StringView, String&)> ReadData(u32& reads)
    {
        return [&reads](StringView path, String& out)
        {
            ++reads;
            const String full = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), path);
            Result<Array<byte>> bytes = ReadFile(full.AsView());
            if (!bytes.HasValue())
                return false;
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return true;
        };
    }
}

TEST_CASE("code languages: each shared definition read by its id once, an unknown one none")
{
    u32 reads = 0;
    editor::app::CodeLanguages languages(DefaultAllocator(), ReadData(reads));
    constexpr StringView kIds[] = {u8"angelscript", u8"luau", u8"hlsl", u8"xml", u8"css"};
    for (const StringView id : kIds)
    {
        const gui::CodeLanguage* language = languages.Find(id);
        REQUIRE(language != nullptr);
        CHECK_FALSE(language->Contexts.IsEmpty());
        CHECK(languages.Find(id) == language); // kept: the same, not read again
    }
    CHECK(reads == 5u);
    CHECK(languages.Find(u8"cobol") == nullptr);
    CHECK(languages.Find(u8"cobol") == nullptr);
    CHECK(reads == 6u); // a miss is kept too
    CHECK(languages.Find(u8"") == nullptr);
}
