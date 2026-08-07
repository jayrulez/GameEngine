// BuildScriptApiTree: the API browser's tree model over a bound-API surface - alphabetical
// type + member order, signature-or-name labels, member-name insert text, and the
// case-insensitive filter (type match keeps the whole type; member match keeps its type row).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.script;
import draconic.editor.script;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace script = draconic::script;

namespace
{
    Array<script::ScriptApiType> MakeSurface()
    {
        Array<script::ScriptApiType> types;

        script::ScriptApiType zeta;
        zeta.scriptName = String(u8"Zeta");
        zeta.members.PushBack(script::ScriptApiMember{String(u8"beta"), String(u8"beta(_)"),
                                                      false,
                                                      script::ScriptApiMemberKind::Method});
        zeta.members.PushBack(script::ScriptApiMember{String(u8"alpha"), String(),
                                                      false,
                                                      script::ScriptApiMemberKind::Property});
        types.PushBack(Move(zeta));

        script::ScriptApiType math;
        math.scriptName = String(u8"Math");
        math.isNamespace = true;
        math.members.PushBack(script::ScriptApiMember{String(u8"Dot"), String(u8"Dot(_,_)"),
                                                      true,
                                                      script::ScriptApiMemberKind::Method});
        math.members.PushBack(script::ScriptApiMember{String(u8"Cross"), String(u8"Cross(_,_)"),
                                                      true,
                                                      script::ScriptApiMemberKind::Method});
        types.PushBack(Move(math));

        return types;
    }
}

TEST_CASE("editor-script: API browser tree - order, labels, insert text")
{
    const Array<script::ScriptApiType> surface = MakeSurface();
    const ScriptApiTree tree = BuildScriptApiTree(surface, StringView(u8""));

    // Types alphabetical: Math before Zeta.
    REQUIRE(tree.roots.Size() == 2);
    const ScriptApiTreeNode& math = tree.nodes[static_cast<usize>(tree.roots[0])];
    const ScriptApiTreeNode& zeta = tree.nodes[static_cast<usize>(tree.roots[1])];
    CHECK(math.label == u8"Math");
    CHECK(zeta.label == u8"Zeta");
    CHECK(math.depth == 0);
    CHECK(math.insertText == u8"Math");

    // Members alphabetical under each type; label = signature, falling back to the name.
    REQUIRE(math.children.Size() == 2);
    CHECK(tree.nodes[static_cast<usize>(math.children[0])].label == u8"Cross(_,_)");
    CHECK(tree.nodes[static_cast<usize>(math.children[1])].label == u8"Dot(_,_)");
    REQUIRE(zeta.children.Size() == 2);
    CHECK(tree.nodes[static_cast<usize>(zeta.children[0])].label == u8"alpha"); // no signature
    CHECK(tree.nodes[static_cast<usize>(zeta.children[1])].label == u8"beta(_)");

    // A member row inserts its bare NAME (what a script types), never the signature.
    const ScriptApiTreeNode& dot = tree.nodes[static_cast<usize>(math.children[1])];
    CHECK(dot.insertText == u8"Dot");
    CHECK(dot.depth == 1);
}

TEST_CASE("editor-script: API browser tree - case-insensitive filter")
{
    const Array<script::ScriptApiType> surface = MakeSurface();

    // A member match keeps its type row with ONLY the matching members.
    {
        const ScriptApiTree tree = BuildScriptApiTree(surface, StringView(u8"dot"));
        REQUIRE(tree.roots.Size() == 1);
        const ScriptApiTreeNode& math = tree.nodes[static_cast<usize>(tree.roots[0])];
        CHECK(math.label == u8"Math");
        REQUIRE(math.children.Size() == 1);
        CHECK(tree.nodes[static_cast<usize>(math.children[0])].label == u8"Dot(_,_)");
    }

    // A TYPE match keeps the whole type, members untouched.
    {
        const ScriptApiTree tree = BuildScriptApiTree(surface, StringView(u8"zeta"));
        REQUIRE(tree.roots.Size() == 1);
        CHECK(tree.nodes[static_cast<usize>(tree.roots[0])].children.Size() == 2);
    }

    // No match: empty tree.
    {
        const ScriptApiTree tree = BuildScriptApiTree(surface, StringView(u8"nothing"));
        CHECK(tree.roots.Size() == 0);
        CHECK(tree.nodes.Size() == 0);
    }
}

TEST_CASE("editor-script: API browser tree - editor-only bindings are marked")
{
    // A type registered under the "Editor" domain (as every */Editor/* asset module now
    // does) gets the " [editor]" row marker; insert text stays the bare name.
    struct EditorOnlyThing
    {
    };
    static const TypeInfo editorType =
        MakeTypeInfo<EditorOnlyThing>("EditorOnlyThing", "draconic::test", nullptr);
    GlobalTypeRegistry().Register(editorType, TypeDomain(u8"Editor"));

    Array<script::ScriptApiType> surface;
    script::ScriptApiType marked;
    marked.scriptName = String(u8"EditorOnlyThing");
    marked.typeId = editorType.id;
    surface.PushBack(Move(marked));
    script::ScriptApiType plain;
    plain.scriptName = String(u8"RuntimeThing"); // typeId 0: no registry identity, no marker
    surface.PushBack(Move(plain));

    const ScriptApiTree tree = BuildScriptApiTree(surface, StringView(u8""));
    REQUIRE(tree.roots.Size() == 2);
    CHECK(tree.nodes[static_cast<usize>(tree.roots[0])].label == u8"EditorOnlyThing [editor]");
    CHECK(tree.nodes[static_cast<usize>(tree.roots[0])].insertText == u8"EditorOnlyThing");
    CHECK(tree.nodes[static_cast<usize>(tree.roots[1])].label == u8"RuntimeThing");

    CHECK(IsEditorOnlyBinding(editorType.id));
    CHECK(!IsEditorOnlyBinding(0));
}
