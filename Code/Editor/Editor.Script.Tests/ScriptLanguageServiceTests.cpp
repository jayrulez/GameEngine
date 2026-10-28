// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script page's completion and hover over the SHARED ScriptApiSurface (built once through a
// throwaway backend manager, the runtime's registration sequence): the bound types at the top
// level, a type's members after `Type.`, a variable's type's members after `name.`, nothing after
// a name it cannot place; hover saying what a type is, or a paused run's value. Proven on both
// backends (the surface resolves by language).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.script;
import foundation.gui.code;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif
import editor.script;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    bool Contains(const Array<gui::CodeCompletionItem>& out, StringView label)
    {
        for (const gui::CodeCompletionItem& item : out)
            if (item.Label.AsView() == label)
                return true;
        return false;
    }

    // What the service offers at the end of `text` (one line).
    Array<gui::CodeCompletionItem> CompleteAtEnd(ScriptLanguageService& service, StringView text)
    {
        gui::CodeDocument doc(DefaultAllocator());
        doc.SetText(text);
        u32 start = static_cast<u32>(text.Size());
        while (start > 0 && ((text[start - 1] >= u8'a' && text[start - 1] <= u8'z') || (text[start - 1] >= u8'A' && text[start - 1] <= u8'Z')))
            --start;
        gui::CodeCompletionRequest request;
        request.At = gui::CodePosition{0, static_cast<u32>(text.Size())};
        request.WordStart = gui::CodePosition{0, start};
        Array<gui::CodeCompletionItem> out;
        service.Complete(doc, request, out);
        return out;
    }

    void CheckBoundApi(StringView language)
    {
        ScriptApiSurface surface;
        surface.SetLanguage(language);
        ScriptLanguageService service(surface, nullptr);

        // Top level: the bound type names (the reflected Core math surface is always registered).
        CHECK(Contains(CompleteAtEnd(service, u8"Flo"), u8"Float3"));
        // After the type's name, its statics (the binding's functions: `Float3::Dot`, `Float3.Dot`).
        const Array<gui::CodeCompletionItem> statics = CompleteAtEnd(service, u8"Float3.");
        CHECK(Contains(statics, u8"Dot"));
        CHECK_FALSE(Contains(statics, u8"x"));
        CHECK_FALSE(Contains(statics, u8"Float3"));
        CHECK(Contains(CompleteAtEnd(service, u8"Float3::"), u8"Dot"));
        // After a variable's name, the value's own members, of the type it is declared with (read
        // from the text).
        const Array<gui::CodeCompletionItem> own = CompleteAtEnd(service, u8"Float3 where; where.");
        CHECK(Contains(own, u8"x"));
        CHECK_FALSE(Contains(own, u8"Dot"));
        // A name it cannot place: nothing.
        CHECK(CompleteAtEnd(service, u8"nonsense.").IsEmpty());

        // Hover: what a type is; while paused, what a name holds.
        gui::CodeDocument doc(DefaultAllocator());
        doc.SetText(u8"Float3");
        String said;
        REQUIRE(service.Hover(doc, gui::CodePosition{0, 2}, said));
        CHECK(said.AsView().StartsWith(u8"type Float3"));
        service.SetValueProbe([](StringView name) { return name == StringView(u8"Float3") ? String(u8"(1, 2, 3)") : String{}; });
        said.Clear();
        REQUIRE(service.Hover(doc, gui::CodePosition{0, 2}, said));
        CHECK(said == StringView(u8"(1, 2, 3)"));

        // An unknown language: nothing bound.
        ScriptApiSurface unknown;
        unknown.SetLanguage(u8"cobol");
        ScriptLanguageService none(unknown, nullptr);
        CHECK_FALSE(Contains(CompleteAtEnd(none, u8"Flo"), u8"Float3"));
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("script language service: the bound API's completion and hover (angelscript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    CheckBoundApi(u8"angelscript");
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("script language service: the bound API's completion and hover (luau)")
{
    foundation::script::RegisterLuauScriptBackend();
    CheckBoundApi(u8"luau");
}
#endif // OPTION_HAS_LUAU
