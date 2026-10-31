// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App: the token reference. Every token the editor's tokens sheet and its density sheets
// declare is named in Documentation/Systems/editor-theme.md, so the reference stays in step with the
// sheets.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;

using namespace foundation::core;

namespace
{
    String ReadSource(StringView relative)
    {
        const String path = PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    bool Contains(StringView text, StringView part)
    {
        for (usize i = 0; i + part.Size() <= text.Size(); ++i)
            if (text.SubStr(i, part.Size()) == part)
                return true;
        return false;
    }
    bool IsNameChar(char8_t c) { return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'-' || c == u8'_'; }
    // The custom properties `sheet` declares (`--name:`), skipping comments.
    Array<String> Declared(StringView sheet)
    {
        Array<String> names;
        usize i = 0;
        while (i + 1 < sheet.Size())
        {
            if (sheet[i] == u8'/' && sheet[i + 1] == u8'*')
            {
                i += 2;
                while (i + 1 < sheet.Size() && !(sheet[i] == u8'*' && sheet[i + 1] == u8'/'))
                    ++i;
                i += 2;
                continue;
            }
            if (sheet[i] == u8'-' && sheet[i + 1] == u8'-' && (i == 0 || !IsNameChar(sheet[i - 1])))
            {
                usize end = i + 2;
                while (end < sheet.Size() && IsNameChar(sheet[end]))
                    ++end;
                usize colon = end;
                while (colon < sheet.Size() && (sheet[colon] == u8' ' || sheet[colon] == u8'\t'))
                    ++colon;
                if (colon < sheet.Size() && sheet[colon] == u8':')
                    names.PushBack(String(sheet.SubStr(i, end - i)));
                i = end;
                continue;
            }
            ++i;
        }
        return names;
    }
}

TEST_CASE("editor theme: the token reference names every token the editor's sheets declare")
{
    const String reference = ReadSource(u8"Documentation/Systems/editor-theme.md");
    usize checked = 0;
    for (StringView sheet : {StringView(u8"Data/Editor/tokens.css"), StringView(u8"Data/Editor/density-compact.css"),
                             StringView(u8"Data/Editor/density-default.css"), StringView(u8"Data/Editor/density-comfortable.css")})
    {
        const String text = ReadSource(sheet);
        for (const String& name : Declared(text.AsView()))
        {
            CAPTURE(sheet);
            CAPTURE(name);
            CHECK(Contains(reference.AsView(), Format(u8"`{}`", name).AsView()));
            ++checked;
        }
    }
    CHECK(checked > 100); // the sheets were read and their tokens found
}
