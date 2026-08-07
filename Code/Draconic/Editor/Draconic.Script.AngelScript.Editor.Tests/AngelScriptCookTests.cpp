// draconic.script.angelscript.editor tests: the AngelScript cook - compile-check in a
// cooker-owned AngelScript VM (resolved by language), the SHARED handler scan, the starter
// template, and [metadata] PROPERTY HARVEST (typed member field + `[default, "desc"]`).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.script;
import draconic.script.resource;
import draconic.script.editor;
import draconic.script.angelscript.editor;

using namespace draconic::foundation;
using namespace draconic::script;

namespace
{
    [[nodiscard]] IScriptLanguageCook* AngelScriptCook()
    {
        RegisterAngelScriptScriptCook(); // registers the AS backend + cook (idempotent)
        return ScriptLanguageCookRegistry::Get().FindByLanguage(u8"angelscript");
    }
}

TEST_CASE("as.cook: registered + resolvable by language; supplies a starter template")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CHECK_FALSE(cook->NewAssetTemplate(ScriptTier::Behavior).IsEmpty());
}

TEST_CASE("as.cook: the Level + Game tier starters cook to their contract classes")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    const StringView behavior = cook->NewAssetTemplate(ScriptTier::Behavior);
    const StringView level = cook->NewAssetTemplate(ScriptTier::Level);
    const StringView game = cook->NewAssetTemplate(ScriptTier::Game);
    CHECK_FALSE(level.IsEmpty());
    CHECK_FALSE(game.IsEmpty());
    CHECK(level != behavior);
    CHECK(game != behavior);
    CHECK(level != game);

    {
        CookScriptErrorSink sink;
        ScriptClassSource out;
        REQUIRE(cook->Cook(level, u8"NewLevel.as", sink, out));
        CHECK(out.className == u8"Level");
    }
    {
        CookScriptErrorSink sink;
        ScriptClassSource out;
        REQUIRE(cook->Cook(game, u8"Game.as", sink, out));
        CHECK(out.className == u8"Game");
    }
}

TEST_CASE("as.cook: an AngelScript behavior compile-checks, scans its on<Upper>(...) "
          "handlers, and a field WITHOUT metadata is NOT a property")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(u8"class Bouncer\n"
                               u8"{\n"
                               u8"    private Entity@ self;\n"
                               u8"    Bouncer(Entity@ e) { @self = e; }\n"
                               u8"    void onStart() {}\n"
                               u8"    void onUpdate(double dt) {}\n"
                               u8"    void onHeal(double amount) {}\n"
                               u8"}\n",
                               u8"bouncer.as", sink, out);
    REQUIRE(ok);
    CHECK(out.language == u8"angelscript");
    CHECK(out.className == u8"Bouncer");
    auto scanned = [&out](StringView name)
    {
        for (const String& h : out.handlers)
        {
            if (h.AsView() == name)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(scanned(u8"onStart"));
    CHECK(scanned(u8"onUpdate"));
    CHECK(scanned(u8"onHeal"));
    CHECK(out.handlers.Size() == 3u);
    CHECK(out.properties.IsEmpty()); // no metadata anywhere -> no inspector properties
}

namespace
{
    // Finds a harvested property by name (order-independent assertions).
    [[nodiscard]] const ScriptPropertyDesc* FindProp(const ScriptClassSource& out, StringView name)
    {
        for (const ScriptPropertyDesc& p : out.properties)
        {
            if (p.name.AsView() == name)
            {
                return &p;
            }
        }
        return nullptr;
    }
    bool Near(f64 a, f64 b) { return (a < b ? b - a : a - b) < 1e-5; }
}

TEST_CASE("as.cook: harvests [metadata] member fields of every supported type into "
          "ScriptPropertyDesc (name, type, default, description, asset type)")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(u8"class Kitchen\n"
                               u8"{\n"
                               u8"    [4.0, \"units per second\"] float   speed;\n"
                               u8"    [7, \"hit points\"]         int     hp;\n"
                               u8"    [true]                     bool    active;\n"
                               u8"    [\"hello\", \"greeting\"]     string  greeting;\n"
                               u8"    [(1, 0.5, 0.25, 1), \"tint\"] Color@  tint;\n"
                               u8"    [(0, 1, 0)]                Float3@ dir;\n"
                               u8"    [null, \"the target\"]       Entity@ target;\n"
                               u8"    [\"asset:AudioClip\", \"sfx\"] Guid@   clip;\n"
                               u8"    float                      noMeta;\n"
                               u8"    void onUpdate(double dt) {}\n"
                               u8"}\n",
                               u8"kitchen.as", sink, out);
    REQUIRE(ok);
    CHECK(out.className == u8"Kitchen");
    CHECK(out.properties.Size() == 8u); // noMeta is not a property
    CHECK(FindProp(out, u8"noMeta") == nullptr);

    const ScriptPropertyDesc* speed = FindProp(out, u8"speed");
    REQUIRE(speed != nullptr);
    CHECK(speed->type == ScriptPropertyType::Float);
    CHECK(speed->hash == ScriptPropertyNameHash(u8"speed"));
    CHECK(Near(speed->defaultValue.number, 4.0));
    CHECK(speed->description == u8"units per second");

    const ScriptPropertyDesc* hp = FindProp(out, u8"hp");
    REQUIRE(hp != nullptr);
    CHECK(hp->type == ScriptPropertyType::Int);
    CHECK(Near(hp->defaultValue.number, 7.0));
    CHECK(hp->description == u8"hit points");

    const ScriptPropertyDesc* active = FindProp(out, u8"active");
    REQUIRE(active != nullptr);
    CHECK(active->type == ScriptPropertyType::Bool);
    CHECK(active->defaultValue.boolean == true);
    CHECK(active->description.IsEmpty());

    const ScriptPropertyDesc* greeting = FindProp(out, u8"greeting");
    REQUIRE(greeting != nullptr);
    CHECK(greeting->type == ScriptPropertyType::String);
    CHECK(greeting->defaultValue.text == u8"hello");
    CHECK(greeting->description == u8"greeting");

    const ScriptPropertyDesc* tint = FindProp(out, u8"tint");
    REQUIRE(tint != nullptr);
    CHECK(tint->type == ScriptPropertyType::Color);
    CHECK(Near(tint->defaultValue.color.r, 1.0));
    CHECK(Near(tint->defaultValue.color.g, 0.5));
    CHECK(Near(tint->defaultValue.color.b, 0.25));
    CHECK(Near(tint->defaultValue.color.a, 1.0));
    CHECK(tint->description == u8"tint");

    const ScriptPropertyDesc* dir = FindProp(out, u8"dir");
    REQUIRE(dir != nullptr);
    CHECK(dir->type == ScriptPropertyType::Vec3);
    CHECK(Near(dir->defaultValue.vector.x, 0.0));
    CHECK(Near(dir->defaultValue.vector.y, 1.0));
    CHECK(Near(dir->defaultValue.vector.z, 0.0));

    const ScriptPropertyDesc* target = FindProp(out, u8"target");
    REQUIRE(target != nullptr);
    CHECK(target->type == ScriptPropertyType::Entity);
    CHECK(target->defaultValue.guid.IsNil()); // null default
    CHECK(target->description == u8"the target");

    const ScriptPropertyDesc* clip = FindProp(out, u8"clip");
    REQUIRE(clip != nullptr);
    CHECK(clip->type == ScriptPropertyType::Asset);
    CHECK(clip->assetType == u8"AudioClip");
    CHECK(clip->description == u8"sfx");
}

TEST_CASE("as.cook: a metadata'd field of an unsupported type FAILS the cook")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CookScriptErrorSink sink;
    ScriptClassSource out;
    // A Guid without the asset:<TypeName> tag is not a resolvable property type.
    CHECK_FALSE(cook->Cook(u8"class Bad { [42] Guid@ mystery; void onUpdate(double dt) {} }\n",
                           u8"bad.as", sink, out));
}

// game-ready-scripting.md Section 16 (Fable's option-1+3 ruling): a reflected/resource property is a
// reference type; declared as a VALUE member it silently drops its value at runtime, so the cook
// REJECTS it with the exact fix ("declare it 'Guid@ mesh'"). The idiomatic handle form cooks fine.
TEST_CASE("as.cook: a reflected/resource property declared as a VALUE member FAILS the cook")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CookScriptErrorSink sink;
    ScriptClassSource out;
    // asset:Mesh on a VALUE Guid (no @): a reference type the runtime can't fill - rejected.
    CHECK_FALSE(cook->Cook(u8"class B1 { [\"asset:Mesh\"] Guid mesh; void onUpdate(double dt){} }\n",
                           u8"b1.as", sink, out));
    // A value Color / Entity member is equally rejected.
    CHECK_FALSE(cook->Cook(u8"class B2 { [(1,1,1,1)] Color tint; void onUpdate(double dt){} }\n",
                           u8"b2.as", sink, out));
    CHECK_FALSE(cook->Cook(u8"class B3 { [null] Entity target; void onUpdate(double dt){} }\n",
                           u8"b3.as", sink, out));
    // The idiomatic handle form cooks clean.
    CHECK(cook->Cook(u8"class Good { [\"asset:Mesh\"] Guid@ mesh; void onUpdate(double dt){} }\n",
                     u8"good.as", sink, out));
}

TEST_CASE("as.cook: the starter template itself compiles clean")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CookScriptErrorSink sink;
    ScriptClassSource out;
    CHECK(cook->Cook(cook->NewAssetTemplate(ScriptTier::Behavior), u8"NewBehavior.as", sink, out));
    CHECK(out.className == u8"NewBehavior");
}

TEST_CASE("as.cook: a compile error FAILS the cook (the last good record is untouched)")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CookScriptErrorSink sink;
    ScriptClassSource out;
    CHECK_FALSE(cook->Cook(u8"class Broken { void foo( }\n", u8"broken.as", sink, out));
}

TEST_CASE("as.cook: the New-Asset starter template compiles clean (its example calls resolve)")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(cook->NewAssetTemplate(ScriptTier::Behavior), u8"NewBehavior.as", sink, out);
    REQUIRE(ok); // the starter MUST compile - it teaches the API by example
    CHECK(out.className == u8"NewBehavior");
}
