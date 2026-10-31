// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The neutral-builder cook round-trip driven with Luau sources: the same path from source through
// builder and factory to metadata as the AngelScript leg (AngelScriptPipelineTests.cpp), showing
// the builder resolves the Luau cook by the asset's language. Luau harvests by construct-and-walk,
// so the metadata is the scalar subset (number/bool/string/Float3), sorted by name. The per-kind
// harvest detail is tested in Script.Luau.Pipeline.Tests; here it goes through the neutral builder.

#include "CookBed.h"

import editor.core; // ScriptSourceDocument (the ScriptPage save-and-recook loop)

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::script;
namespace content = foundation::content;
using scriptpipe::CookBed;

namespace
{
    // A behavior whose constructor sets one field of each harvestable scalar kind plus two that must
    // be skipped (a table, and the nil owner Lua drops from the instance).
    constexpr StringView kMultiSource = u8R"lua(
Multi = {}
Multi.__index = Multi
function Multi.new(entity)
    local self = setmetatable({}, Multi)
    self.speed = 2.5
    self.enabled = true
    self.label = "hello"
    self.offset = Float3.new(1, 2, 3)
    self.data = {}
    self.owner = entity
    return self
end
function Multi:onStart() end
function Multi:onUpdate(dt) end
function Multi:onDestroy() end
)lua";
}

TEST_CASE("script.pipeline: full harvest round-trip (luau) - source -> cook -> factory -> "
          "scalar metadata (sorted, hashed)")
{
    CookBed bed(u8"luau_harvest");
    bed.WriteSource(u8"multi.luau", kMultiSource);
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"multi.luau", u8"luau", instance).IsOk());

    ScriptClassFactory factory(DefaultAllocator());
    foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), *bed.outputDb);
    manager.AddFactory(&factory);
    foundation::resource::Proxy<ScriptClass> product = manager.Bind<ScriptClass>(instance->Id());
    REQUIRE(product);
    CHECK(product->language == u8"luau");
    CHECK(product->className == u8"Multi");

    // Four scalars harvested (speed/enabled/label/offset); `data` (table) and `owner` (nil) skipped.
    // Non-scalar/typed properties (Int, Color, Entity, Asset) have no Luau harvest form.
    const ScriptPropertyDesc* speed = product->FindProperty(ScriptPropertyNameHash(u8"speed"));
    REQUIRE(speed != nullptr);
    CHECK(speed->type == ScriptPropertyType::Float);
    CHECK(speed->defaultValue.number == doctest::Approx(2.5));

    const ScriptPropertyDesc* enabled = product->FindProperty(ScriptPropertyNameHash(u8"enabled"));
    REQUIRE(enabled != nullptr);
    CHECK(enabled->type == ScriptPropertyType::Bool);
    CHECK(enabled->defaultValue.boolean);

    const ScriptPropertyDesc* label = product->FindProperty(ScriptPropertyNameHash(u8"label"));
    REQUIRE(label != nullptr);
    CHECK(label->type == ScriptPropertyType::String);
    CHECK(label->defaultValue.text == u8"hello");

    const ScriptPropertyDesc* offset = product->FindProperty(ScriptPropertyNameHash(u8"offset"));
    REQUIRE(offset != nullptr);
    CHECK(offset->type == ScriptPropertyType::Vec3);
    CHECK(offset->defaultValue.vector.z == doctest::Approx(3.0f));

    CHECK(product->FindProperty(ScriptPropertyNameHash(u8"data")) == nullptr);  // table skipped
    CHECK(product->FindProperty(ScriptPropertyNameHash(u8"owner")) == nullptr); // nil skipped

    CHECK(product->HasHandler(u8"onStart"));
    CHECK(product->HasHandler(u8"onUpdate"));
    CHECK(product->HasHandler(u8"onDestroy"));
}

TEST_CASE("script.pipeline: a ScriptClass-less utility module cooks with empty metadata (luau)")
{
    CookBed bed(u8"luau_util");
    bed.WriteSource(u8"util.luau", u8"local Helper = 42\nreturn Helper\n");
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"util.luau", u8"luau", instance).IsOk());
    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr);
    CHECK(cooked->className.IsEmpty());
    CHECK(cooked->properties.IsEmpty());
    CHECK(cooked->handlers.IsEmpty());
}

TEST_CASE("script.pipeline: compile errors FAIL the cook and the last good record "
          "survives (luau)")
{
    CookBed bed(u8"luau_errors");
    bed.WriteSource(u8"multi.luau", kMultiSource);
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"multi.luau", u8"luau", instance).IsOk());

    // Break the source; the rebuild must fail without touching the cooked record.
    bed.WriteSource(u8"multi.luau", u8"function (\n");
    CHECK_FALSE(bed.Cook(u8"multi.luau", u8"luau", instance).IsOk());

    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr); // the last good record
    CHECK(cooked->className == u8"Multi");
    CHECK(cooked->properties.Size() == 4u);
}

TEST_CASE("script.pipeline: a starter behavior cooks with its declared property + handlers (luau)")
{
    CookBed bed(u8"luau_starter");
    bed.WriteSource(u8"starter.luau", u8R"lua(
NewBehavior = {}
NewBehavior.__index = NewBehavior
function NewBehavior.new(entity)
    local self = setmetatable({}, NewBehavior)
    self.speed = 1.0
    return self
end
function NewBehavior:onStart() end
function NewBehavior:onUpdate(dt) end
function NewBehavior:onDestroy() end
)lua");
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"starter.luau", u8"luau", instance).IsOk());
    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr);
    CHECK(cooked->className == u8"NewBehavior");
    REQUIRE(cooked->properties.Size() == 1u);
    CHECK(cooked->properties[0].name == u8"speed");
    CHECK(cooked->properties[0].type == ScriptPropertyType::Float);
    CHECK(cooked->handlers.Size() == 3u); // onStart, onUpdate, onDestroy
}

TEST_CASE("script.pipeline: ScriptSourceDocument save->recook seam (luau) - round-trips source, "
          "validates, and surfaces compile errors while the last-good product survives")
{
    constexpr StringView kMover = u8R"lua(
Mover = {}
Mover.__index = Mover
function Mover.new(entity)
    return setmetatable({}, Mover)
end
function Mover:onStart() end
function Mover:onUpdate(dt) end
)lua";

    CookBed bed(u8"luau_scriptpage");
    bed.WriteSource(u8"mover.luau", kMover);

    // Load: the document reads the bound source file into its edit buffer.
    ScriptSourceDocument doc;
    doc.Bind(bed.srcDir.AsView(), u8"mover.luau", u8"luau");
    REQUIRE(doc.Load().IsOk());
    CHECK(doc.Source() == kMover);
    CHECK_FALSE(doc.IsModified());

    // Validate: a compile-check through the same language cook, with no product write.
    CHECK(doc.Validate());
    CHECK(doc.LastCompileOk());
    CHECK(doc.ClassName() == u8"Mover");
    CHECK(doc.Errors().Size() == 0u);

    // The recook (the builder over the saved file) produces an updated cooked ScriptClass.
    content::Instance* product = nullptr;
    REQUIRE(bed.Cook(u8"mover.luau", u8"luau", product).IsOk());
    {
        RefPtr<ISerializable> object = product->ReadObject();
        ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->className == u8"Mover");
    }

    // Edit to a broken script through the document and save it to disk.
    constexpr StringView kBroken = u8"function (\n";
    doc.SetSource(kBroken);
    CHECK(doc.IsModified());
    REQUIRE(doc.Save().IsOk());
    CHECK_FALSE(doc.IsModified());

    // The bytes persisted (a fresh document reads them back).
    ScriptSourceDocument reopened;
    reopened.Bind(bed.srcDir.AsView(), u8"mover.luau", u8"luau");
    REQUIRE(reopened.Load().IsOk());
    CHECK(reopened.Source() == kBroken);

    // Validate now surfaces compile errors and compiles false.
    CHECK_FALSE(doc.Validate());
    CHECK_FALSE(doc.LastCompileOk());
    REQUIRE(doc.Errors().Size() >= 1u);
    CHECK_FALSE(doc.Errors().Data()[0].message.IsEmpty());

    // The recook of the now-broken source fails, and the last good cooked product survives.
    CHECK_FALSE(bed.Cook(u8"mover.luau", u8"luau", product).IsOk());
    {
        RefPtr<ISerializable> object = product->ReadObject();
        ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->className == u8"Mover"); // unchanged: the failed cook never wrote
    }
}

TEST_CASE("script.pipeline: luau ScriptSourceDocument::Validate compiles without running the "
          "top level, and a syntax error carries its column")
{
    CookBed bed(u8"luau_validate_check");
    // Cook loads (runs) the chunk, so this fails at cook; the check never runs it.
    constexpr StringView kRaising = u8"error(\"ran\")\n"
                                    u8"Mover = {}\n"
                                    u8"Mover.__index = Mover\n"
                                    u8"function Mover.new(entity)\n"
                                    u8"    return setmetatable({}, Mover)\n"
                                    u8"end\n";
    bed.WriteSource(u8"mover.luau", kRaising);
    ScriptSourceDocument doc;
    doc.Bind(bed.srcDir.AsView(), u8"mover.luau", u8"luau");
    REQUIRE(doc.Load().IsOk());
    CHECK(doc.Validate());
    CHECK(doc.ClassName() == u8"Mover");
    CHECK(doc.Errors().Size() == 0u);
    content::Instance* product = nullptr;
    CHECK_FALSE(bed.Cook(u8"mover.luau", u8"luau", product).IsOk());

    doc.SetSource(u8"Mover = {}\nlocal c = = =\n");
    CHECK_FALSE(doc.Validate());
    REQUIRE(doc.Errors().Size() >= 1u);
    CHECK(doc.Errors().Data()[0].line == 2);
    CHECK(doc.Errors().Data()[0].column == 11);
}
