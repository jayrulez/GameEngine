// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.script.angelscript.editor tests: the AngelScript cook - compile-check in a
// cooker-owned AngelScript VM (resolved by language), the SHARED handler scan, the starter
// template, and [metadata] PROPERTY HARVEST (typed member field + `[default, "desc"]`).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import foundation.script.resource;
import foundation.script.angelscript; // CreateScriptManager + AngelScriptBytecodeVersion (player path)
import foundation.script.facades;      // RegisterScriptFacadeReflection (match the cook's surface)
import script.pipeline;
import script.angelscript.pipeline;
import engine.composition;            // RegisterAllScriptFacades (the COMPLETE engine facade surface)

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::script;

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
    CHECK_FALSE(cook->NewAssetTemplate(ScriptTier::Behavior, {}).IsEmpty());
}

TEST_CASE("as.cook: the Level + Game tier starters cook to their contract classes")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    const String behavior = cook->NewAssetTemplate(ScriptTier::Behavior, {});
    const String level = cook->NewAssetTemplate(ScriptTier::Level, {});
    const String game = cook->NewAssetTemplate(ScriptTier::Game, {});
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
    // The reason reaches the sink, so script_validate (and the asset cook) can say why.
    REQUIRE(sink.errors.Size() == 1u);
    CHECK(sink.errors[0].message.AsView().ContainsIgnoreCase(u8"'mystery' has unsupported type"));
}

TEST_CASE("as.cook: a `replicated` word marks a property; a stray word or a replicated reference fails")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    {
        CookScriptErrorSink sink;
        ScriptClassSource out;
        REQUIRE(cook->Cook(u8"class Guest {\n"
                           u8"  [0, \"What the guest is doing\", replicated] int activity;\n"
                           u8"  [1.5, replicated] float pace;\n"
                           u8"  [true, \"Local only\"] bool chatty;\n"
                           u8"  void onUpdate(double dt) {} }\n",
                           u8"Guest.as", sink, out));
        REQUIRE(out.properties.Size() == 3u);
        for (const ScriptPropertyDesc& p : out.properties)
        {
            if (p.name == u8"activity")
            {
                CHECK(p.replicated);
                CHECK(p.description == u8"What the guest is doing");
            }
            else if (p.name == u8"pace")
            {
                CHECK(p.replicated);
                CHECK(p.defaultValue.number == doctest::Approx(1.5));
            }
            else
            {
                CHECK_FALSE(p.replicated);
            }
        }
    }
    {
        CookScriptErrorSink sink;
        ScriptClassSource out;
        CHECK_FALSE(cook->Cook(u8"class Guest { [0, replicate] int activity; void onUpdate(double dt) {} }\n",
                               u8"Guest.as", sink, out));
        REQUIRE(sink.errors.Size() == 1u);
        CHECK(sink.errors[0].message.AsView().ContainsIgnoreCase(u8"unknown word 'replicate'"));
    }
    {
        CookScriptErrorSink sink;
        ScriptClassSource out;
        CHECK_FALSE(cook->Cook(u8"class Guest { [null, replicated] Entity@ partner; void onUpdate(double dt) {} }\n",
                               u8"Guest.as", sink, out));
        REQUIRE(sink.errors.Size() == 1u);
        CHECK(sink.errors[0].message.AsView().ContainsIgnoreCase(u8"cannot be replicated"));
    }
}

// A reflected/resource property is a
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
    // Each refusal reached the sink with its fix.
    REQUIRE(sink.errors.Size() == 3u);
    CHECK(sink.errors[0].message.AsView().ContainsIgnoreCase(u8"declare it 'Guid@ mesh'"));
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
    CHECK(cook->Cook(cook->NewAssetTemplate(ScriptTier::Behavior, {}), u8"NewBehavior.as", sink, out));
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
    const bool ok = cook->Cook(cook->NewAssetTemplate(ScriptTier::Behavior, {}), u8"NewBehavior.as", sink, out);
    REQUIRE(ok); // the starter MUST compile - it teaches the API by example
    CHECK(out.className == u8"NewBehavior");
}

TEST_CASE("as.cook: the cook stores loadable bytecode in the pack (the player path)")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    // A module-level function so the reloaded module is callable without instantiating a class.
    CookScriptErrorSink sink;
    ScriptClassSource out;
    REQUIRE(cook->Cook(u8"double twice(double v) { return v * 2.0; }\n", u8"Twice.as", sink, out));
    REQUIRE_FALSE(out.bytecode.IsEmpty());
    CHECK_FALSE(out.source.IsEmpty());

    // Player path: a fresh AngelScript VM (SAME reflected registration - the shared registry)
    // reconstructs the blob from the stored bytes and LoadByteCodes it, no recompile.
    RefPtr<IScriptManager> manager = foundation::script::angelscript::CreateScriptManager(DefaultAllocator());
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptBlob> blob = manager->CreateBlob();
    REQUIRE(blob.Get() != nullptr);
    {
        MemoryStream stream;
        (void)stream.Write(out.bytecode.Data(), out.bytecode.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(stream, SerializeMode::Read);
        blob->Serialize(reader);
    }
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->LoadBlob(*blob).IsOk());
    Variant args[] = {Variant::From<f64>(21.0)};
    CHECK(scriptContext->Call(u8"twice", Span<Variant>{args, 1}).Value().Get<f64>() ==
          doctest::Approx(42.0));
}

// Regression (facade-surface completeness - the cook/validation "No matching symbol" bug): the cook
// compiles a script against whatever sits in the GLOBAL type registry (RegisterReflectedTypes copies
// it into the cook's fresh compile manager). The cook's own registration installs ONLY the
// foundation facades (Log/Time/Entity/Scene), so a script that calls an ENGINE-level facade (run::,
// gui::, physics::, ...) would fail to cook. The cooking TOOLS cure this by calling
// engine::RegisterAllScriptFacades() ONCE at startup, completing the global registry before any cook
// runs (metadata-only, no device/GPU/world, idempotent). This test mirrors that startup step: with
// the full surface registered, a Game class driving run:: and gui:: cooks clean. WITHOUT the call the
// global registry lacks run/gui and this same cook FAILS with "No matching symbol" - which is exactly
// what games hit at cook/live-validation time before the tool fix.
TEST_CASE("as.cook: with the full engine surface registered, a Game using run:: / gui:: cooks clean")
{
    engine::RegisterAllScriptFacades(); // the tools' one-time startup registration - the whole fix
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(u8"class Game\n"
                               u8"{\n"
                               u8"    void launch()\n"
                               u8"    {\n"
                               u8"        run::loadScene(Guid(1, 2));\n"
                               u8"        Node@ root = gui::root();\n"
                               u8"    }\n"
                               u8"    void update(float dt) {}\n"
                               u8"    void exit() {}\n"
                               u8"}\n",
                               u8"Game.as", sink, out);
    REQUIRE(ok); // an Engine-level facade resolves ONLY because the global registry is complete
    CHECK(out.className == u8"Game");
}

// The PaperKid driving-slice behaviors (Sources/Bike.as, Sources/FollowCamera.as) cook against the
// FULL engine surface: Bike drives a CharacterComponent from the Input axis and the reflected
// Quaternion/Math ops; FollowCamera targets another entity through an [null] Entity@ picker property
// and aims with atan2/asin. Kept faithful to the sample scripts so a facade/spelling regression that
// would break the game at live-validation is caught here at build time.
TEST_CASE("as.cook: the PaperKid Bike behavior cooks with tunable properties")
{
    engine::RegisterAllScriptFacades();
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(
        u8"class Bike\n"
        u8"{\n"
        u8"    private Entity@ self;\n"
        u8"    [9.0, \"Top forward speed (m/s)\"]           float maxSpeed;\n"
        u8"    [3.5, \"Top reverse speed (m/s)\"]           float reverseSpeed;\n"
        u8"    [14.0, \"Throttle ramp (m/s^2)\"]            float acceleration;\n"
        u8"    [22.0, \"Active brake / reverse ramp (m/s^2)\"] float braking;\n"
        u8"    [8.0, \"Roll-down when coasting (m/s^2)\"]   float coastDeceleration;\n"
        u8"    [130.0, \"Yaw rate at full speed (deg/s)\"]  float turnSpeedDegrees;\n"
        u8"    [0.25, \"Steering authority floor (0..1)\"]  float minSteerFraction;\n"
        u8"    private float m_heading = 0.0f;\n"
        u8"    private float m_speed = 0.0f;\n"
        u8"    Bike(Entity@ entity) { @self = entity; }\n"
        u8"    void onUpdate(double dt)\n"
        u8"    {\n"
        u8"        float d = float(dt);\n"
        u8"        if (d <= 0.0f) { return; }\n"
        u8"        float throttle = Input::valueY(\"Move\");\n"
        u8"        float steer = Input::valueX(\"Move\");\n"
        u8"        if (throttle > 0.0f) { m_speed += throttle * acceleration * d; }\n"
        u8"        if (m_speed > maxSpeed) { m_speed = maxSpeed; }\n"
        u8"        if (m_speed < -reverseSpeed) { m_speed = -reverseSpeed; }\n"
        u8"        float speedFraction = Math::Abs(m_speed) / maxSpeed;\n"
        u8"        if (speedFraction < minSteerFraction) { speedFraction = minSteerFraction; }\n"
        u8"        m_heading += steer * Math::DegreesToRadians(turnSpeedDegrees) * speedFraction * d;\n"
        u8"        Quaternion facing = Quaternion::FromAxisAngle(Float3(0.0f, 1.0f, 0.0f), m_heading);\n"
        u8"        Float3 forward = Quaternion::RotateVector(facing, Float3(0.0f, 0.0f, 1.0f));\n"
        u8"        CharacterComponent::of(self).move(forward.x * m_speed, forward.z * m_speed);\n"
        u8"        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_heading), 0.0f);\n"
        u8"    }\n"
        u8"}\n",
        u8"Bike.as", sink, out);
    REQUIRE(ok);
    CHECK(out.className == u8"Bike");
    CHECK(out.properties.Size() == 7u); // self/m_heading/m_speed carry no metadata

    const ScriptPropertyDesc* maxSpeed = FindProp(out, u8"maxSpeed");
    REQUIRE(maxSpeed != nullptr);
    CHECK(maxSpeed->type == ScriptPropertyType::Float);
    CHECK(Near(maxSpeed->defaultValue.number, 9.0));
    CHECK(FindProp(out, u8"turnSpeedDegrees") != nullptr);
    CHECK(FindProp(out, u8"m_heading") == nullptr);
}

TEST_CASE("as.cook: the PaperKid FollowCamera behavior cooks with an Entity@ target picker")
{
    engine::RegisterAllScriptFacades();
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);

    CookScriptErrorSink sink;
    ScriptClassSource out;
    const bool ok = cook->Cook(
        u8"class FollowCamera\n"
        u8"{\n"
        u8"    private Entity@ self;\n"
        u8"    [null, \"The entity to follow (the bike)\"] Entity@ target;\n"
        u8"    [7.0, \"Distance behind the target (m)\"]   float distance;\n"
        u8"    [3.5, \"Height above the target (m)\"]      float height;\n"
        u8"    [1.0, \"Aim this far above the target (m)\"] float lookHeight;\n"
        u8"    [4.0, \"Position spring rate\"]             float positionSmoothing;\n"
        u8"    FollowCamera(Entity@ entity) { @self = entity; }\n"
        u8"    void onUpdate(double dt)\n"
        u8"    {\n"
        u8"        float d = float(dt);\n"
        u8"        if (d <= 0.0f || target is null || !target.isValid()) { return; }\n"
        u8"        Float3 targetPos = target.worldPosition();\n"
        u8"        Float3 camPos = self.position();\n"
        u8"        Float3 desired = Float3(targetPos.x, targetPos.y + height, targetPos.z - distance);\n"
        u8"        Float3 newPos = Float3::Lerp(camPos, desired, positionSmoothing * d);\n"
        u8"        self.setPosition(newPos.x, newPos.y, newPos.z);\n"
        u8"        Float3 dir = Float3::Sub(Float3(targetPos.x, targetPos.y + lookHeight, targetPos.z), newPos);\n"
        u8"        float len = Float3::Length(dir);\n"
        u8"        if (len < 0.0001f) { return; }\n"
        u8"        float yaw = Math::RadiansToDegrees(Math::Atan2(dir.x, dir.z));\n"
        u8"        float pitch = Math::RadiansToDegrees(-Math::Asin(dir.y / len));\n"
        u8"        self.setRotationEuler(pitch, yaw, 0.0f);\n"
        u8"    }\n"
        u8"}\n",
        u8"FollowCamera.as", sink, out);
    REQUIRE(ok);
    CHECK(out.className == u8"FollowCamera");
    CHECK(out.properties.Size() == 5u);

    const ScriptPropertyDesc* target = FindProp(out, u8"target");
    REQUIRE(target != nullptr);
    CHECK(target->type == ScriptPropertyType::Entity);
    CHECK(target->defaultValue.guid.IsNil());
    CHECK(FindProp(out, u8"positionSmoothing") != nullptr);
}

TEST_CASE("as.cook: the fingerprint carries the AngelScript library version")
{
    IScriptLanguageCook* cook = AngelScriptCook();
    REQUIRE(cook != nullptr);
    CHECK(foundation::script::angelscript::AngelScriptBytecodeVersion() != 0u);
    CHECK(cook->CookVersion() == foundation::script::angelscript::AngelScriptBytecodeVersion());
    CHECK(ScriptLanguageCookRegistry::Get().CombinedCookVersion() >=
          foundation::script::angelscript::AngelScriptBytecodeVersion());
}
