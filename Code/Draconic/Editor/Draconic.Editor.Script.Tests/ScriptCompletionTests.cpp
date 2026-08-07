// ScriptApiCompletionProvider over the SHARED ScriptApiSurface (built once through a
// throwaway Wren manager - the runtime's registration sequence): type names at top level
// + a type's members after `Type.`.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui.toolkit;
import draconic.script;
import draconic.script.wren;
import draconic.editor.script;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace toolkit = draconic::ui::toolkit;

namespace
{
    bool Contains(const Array<toolkit::CompletionCandidate>& out, const char8_t* label)
    {
        for (usize i = 0; i < out.Size(); ++i)
        {
            if (out[i].label.AsView() == StringView(label))
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("editor-script: bound-API completion (wren)")
{
    draconic::script::wren::RegisterWrenScriptBackend();

    ScriptApiSurface surface;
    surface.SetLanguage(u8"wren");
    ScriptApiCompletionProvider provider;
    provider.SetSurface(&surface);

    toolkit::CodeDocument doc;
    Array<toolkit::CompletionCandidate> out;

    // Top level: bound type names (the reflected Core math surface is always registered).
    doc.SetText(u8"Flo");
    provider.Collect(doc, toolkit::CodePosition{0, 3}, StringView(u8"Flo"), out);
    CHECK(Contains(out, u8"Float3"));

    // Member context: `Float3.` offers that type's members (spelled the backend's way).
    doc.SetText(u8"Float3.");
    out.Clear();
    provider.Collect(doc, toolkit::CodePosition{0, 7}, StringView(u8""), out);
    REQUIRE(out.Size() > 0);
    CHECK(Contains(out, u8"Dot"));

    // Unknown receiver: nothing from this provider (the word provider still runs).
    doc.SetText(u8"nonsense.");
    out.Clear();
    provider.Collect(doc, toolkit::CodePosition{0, 9}, StringView(u8""), out);
    CHECK(out.Size() == 0);

    // Unknown language: silently empty.
    ScriptApiSurface unknownSurface;
    unknownSurface.SetLanguage(u8"cobol");
    ScriptApiCompletionProvider unknown;
    unknown.SetSurface(&unknownSurface);
    out.Clear();
    doc.SetText(u8"x");
    unknown.Collect(doc, toolkit::CodePosition{0, 1}, StringView(u8"x"), out);
    CHECK(out.Size() == 0);
}
