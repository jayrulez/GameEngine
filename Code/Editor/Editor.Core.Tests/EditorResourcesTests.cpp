// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - the editor's own files and type icons (editor-gui X2): EditorResources reads
// text fresh, keeps images and SVGs, and on a change sets the old aside, says which files changed
// and runs what tracks them again; EditorIcons finds a type's icon up its bases, else a generic
// one, and its tint from the nearest category it declares.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.gui; // Effect
import editor.core;

using namespace foundation::core;
using namespace editor;

namespace
{
    // A scratch folder under the harness's working directory (.test-scratch).
    constexpr StringView kRoot = u8"editor-resources";

    void Write(StringView relative, StringView text)
    {
        String full(kRoot);
        full += u8"/";
        full += relative;
        for (usize i = full.Size(); i-- > 0;)
            if (full[i] == u8'/')
            {
                (void)CreateDirectories(full.AsView().SubStr(0, i));
                break;
            }
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }

    constexpr StringView kSquare = u8R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><rect x="2" y="2" width="12" height="12"/></svg>)";
    constexpr StringView kCircle = u8R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/><circle cx="8" cy="8" r="2"/></svg>)";

    // Types to ask for icons: a component category on the base, an icon on the derived one only.
    class IconBase : public Object
    {
        RTTI_OBJECT(IconBase, Object)
    };
    class IconLight : public IconBase
    {
        RTTI_OBJECT(IconLight, IconBase)
    };
    class IconBare : public Object
    {
        RTTI_OBJECT(IconBare, Object)
    };
}

REFLECT_MEMBERS(IconBase, "rtti::editortest")
{
    builder.Attribute("category", String(u8"Visual Effects"));
}
REFLECT_MEMBERS(IconLight, "rtti::editortest")
{
}
REFLECT_MEMBERS(IconBare, "rtti::editortest")
{
}

TEST_CASE("editor resources: text is read fresh; a changed SVG is set aside and loaded anew; a change runs what tracks it")
{
    Write(u8"Editor/editor.css", u8"a { color: red; }");
    Write(u8"Editor/Icons/save.svg", kSquare);
    EditorResources resources(DefaultAllocator(), kRoot);

    String text;
    REQUIRE(resources.LoadText(u8"Editor/editor.css", text));
    CHECK(text == String(u8"a { color: red; }"));
    const auto* square = resources.LoadSvg(u8"Editor/Icons/save.svg");
    REQUIRE(square != nullptr);
    CHECK(resources.LoadSvg(u8"Editor/Icons/save.svg") == square); // kept: the GUI borrows it
    CHECK(resources.Exists(u8"Editor/Icons/save.svg"));
    CHECK_FALSE(resources.Exists(u8"Editor/Icons/none.svg"));

    int runs = 0;
    foundation::gui::Effect reload([&]() { resources.TrackChanges(); ++runs; });
    CHECK(runs == 1);
    CHECK_FALSE(resources.Poll()); // nothing changed yet

    Write(u8"Editor/editor.css", u8"a { color: blue; text-align: center; }");
    Write(u8"Editor/Icons/save.svg", kCircle);
    REQUIRE(resources.Poll());
    CHECK(runs == 2);
    CHECK(resources.LastChanged().Size() == 2u);
    REQUIRE(resources.LoadText(u8"Editor/editor.css", text));
    CHECK(text == String(u8"a { color: blue; text-align: center; }"));
    const auto* circle = resources.LoadSvg(u8"Editor/Icons/save.svg");
    REQUIRE(circle != nullptr);
    CHECK(circle != square); // loaded anew; the old one is still alive for what draws it
}

TEST_CASE("editor icons: a type's own icon, else its base's, else the fallback; tinted by the nearest category")
{
    const char8_t* const there[] = {u8"Editor/Icons/Types/IconLight.svg", u8"Editor/Icons/Types/IconBase.svg",
                                    u8"Editor/Icons/Types/Component.svg"};
    EditorIcons icons(DefaultAllocator(), [&there](StringView path)
                      {
                          for (const char8_t* p : there)
                              if (path == StringView(p))
                                  return true;
                          return false;
                      });

    const TypeIcon light = icons.For(IconLight::StaticType(), u8"Component");
    CHECK(light.Source == String(u8"Editor/Icons/Types/IconLight.svg"));
    CHECK(light.Class == String(u8"cat-visualeffects")); // its base's category

    const TypeIcon bare = icons.For(IconBare::StaticType(), u8"Component");
    CHECK(bare.Source == String(u8"Editor/Icons/Types/Component.svg")); // the fallback
    CHECK(bare.Class.IsEmpty()); // no category: the plain icon colour

    CHECK(icons.For(IconBare::StaticType(), u8"Asset").Source.IsEmpty()); // no icon anywhere
    CHECK(EditorIcons::CategoryClass(u8"Rendering") == String(u8"cat-rendering"));
    CHECK(EditorIcons::CategoryClass(u8"UI") == String(u8"cat-ui"));
    CHECK(EditorIcons::CategoryClass(u8"--").IsEmpty());
}
