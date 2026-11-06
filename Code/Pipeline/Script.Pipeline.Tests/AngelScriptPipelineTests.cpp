// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The neutral-builder cook round-trip driven with AngelScript sources: from source through the
// builder's cooker VM, the cooked record and the factory to runtime metadata; the compile-error
// path (the cook fails, the last good record survives); backend-neutrality (the builder resolves
// its cook through the registry by the asset's language); and the ScriptPage save-and-recook
// loop. The Luau leg of the same
// neutral path lives in LuauPipelineTests.cpp; the pure text scanners in ScriptPipelineTests.cpp.

#include "CookBed.h"

import editor.core;       // ScriptSourceDocument (the ScriptPage save-and-recook loop)
import pipeline.importer; // ScriptFileImporter (extension acceptance, the class an import records)
import foundation.xml.serialization; // a script asset as a project stores it, at data version 0

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::script;
namespace content = foundation::content;
using scriptpipe::CookBed;

namespace
{
    constexpr StringView kMoverSource =
        u8"// A behavior with the full property spread.\n"
        u8"class Mover\n"
        u8"{\n"
        u8"    [4.5, \"units per second\"] float   speed;\n"
        u8"    [3]                        int     count;\n"
        u8"    [true]                     bool    active;\n"
        u8"    [\"hi\"]                     string  label;\n"
        u8"    [(0.25, 0.5, 0.75, 1)]     Color@  tint;\n"
        u8"    [(1, 2, 3)]                Float3@ offset;\n"
        u8"    [null]                     Entity@ target;\n"
        u8"    [\"asset:AudioClip\"]        Guid@   clip;\n"
        u8"    Mover(Entity@ entity) {}\n"
        u8"    void onStart() {}\n"
        u8"    void onUpdate(double dt) {}\n"
        u8"    void onDestroy() {}\n"
        u8"}\n";
}

TEST_CASE("script.pipeline: full harvest round-trip (angelscript) - source -> cook -> factory -> "
          "typed metadata (sorted, hashed, described)")
{
    CookBed bed(u8"as_harvest");
    bed.WriteSource(u8"mover.as", kMoverSource);
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"mover.as", u8"angelscript", instance).IsOk());

    ScriptClassFactory factory(DefaultAllocator());
    foundation::resource::ResourceManager manager(foundation::core::DefaultAllocator(), *bed.outputDb);
    manager.AddFactory(&factory);
    foundation::resource::Proxy<ScriptClass> product = manager.Bind<ScriptClass>(instance->Id());
    REQUIRE(product);
    CHECK(product->language == u8"angelscript");
    CHECK(product->className == u8"Mover");
    CHECK(product->source == kMoverSource);
    REQUIRE(product->properties.Size() == 8u);

    // Every declared property is present and typed (looked up by hash, so declaration order is
    // irrelevant to the record's contents).
    const ScriptPropertyDesc* speed = product->FindProperty(ScriptPropertyNameHash(u8"speed"));
    REQUIRE(speed != nullptr);
    CHECK(speed->type == ScriptPropertyType::Float);
    CHECK(speed->defaultValue.number == doctest::Approx(4.5));
    CHECK(speed->description == u8"units per second");

    const ScriptPropertyDesc* count = product->FindProperty(ScriptPropertyNameHash(u8"count"));
    REQUIRE(count != nullptr);
    CHECK(count->type == ScriptPropertyType::Int);
    CHECK(count->defaultValue.number == doctest::Approx(3.0));

    const ScriptPropertyDesc* active = product->FindProperty(ScriptPropertyNameHash(u8"active"));
    REQUIRE(active != nullptr);
    CHECK(active->type == ScriptPropertyType::Bool);
    CHECK(active->defaultValue.boolean);

    const ScriptPropertyDesc* label = product->FindProperty(ScriptPropertyNameHash(u8"label"));
    REQUIRE(label != nullptr);
    CHECK(label->type == ScriptPropertyType::String);
    CHECK(label->defaultValue.text == u8"hi");

    const ScriptPropertyDesc* tint = product->FindProperty(ScriptPropertyNameHash(u8"tint"));
    REQUIRE(tint != nullptr);
    CHECK(tint->type == ScriptPropertyType::Color);
    CHECK(tint->defaultValue.color.g == doctest::Approx(0.5f));

    const ScriptPropertyDesc* offset = product->FindProperty(ScriptPropertyNameHash(u8"offset"));
    REQUIRE(offset != nullptr);
    CHECK(offset->type == ScriptPropertyType::Vec3);
    CHECK(offset->defaultValue.vector.z == doctest::Approx(3.0f));

    const ScriptPropertyDesc* target = product->FindProperty(ScriptPropertyNameHash(u8"target"));
    REQUIRE(target != nullptr);
    CHECK(target->type == ScriptPropertyType::Entity);
    CHECK(target->defaultValue.guid.IsNil());

    const ScriptPropertyDesc* clip = product->FindProperty(ScriptPropertyNameHash(u8"clip"));
    REQUIRE(clip != nullptr);
    CHECK(clip->type == ScriptPropertyType::Asset);
    CHECK(clip->assetType == u8"AudioClip");

    CHECK(product->HasHandler(u8"onStart"));
    CHECK(product->HasHandler(u8"onUpdate"));
    CHECK(product->HasHandler(u8"onDestroy"));
    CHECK_FALSE(product->HasHandler(u8"onEnable"));
}

TEST_CASE("script.pipeline: facade-using behaviors compile at cook (the cook VM "
          "mirrors the runtime's \"main\" surface, prelude included)")
{
    CookBed bed(u8"as_facades");
    bed.WriteSource(u8"mover.as", u8"class Mover {\n"
                                    u8"    Entity@ self;\n"
                                    u8"    Mover(Entity@ entity) { @self = entity; }\n"
                                    u8"    void onUpdate(double dt) {\n"
                                    u8"        double t = Time::now();\n"
                                    u8"        float r = Random::value();\n"
                                    u8"        Float3 p = self.position();\n"
                                    u8"        self.setPosition(p.x + r, p.y, p.z);\n"
                                    u8"        Log::info(\"tick\");\n"
                                    u8"    }\n"
                                    u8"}\n");
    content::Instance* instance = nullptr;
    CHECK(bed.Cook(u8"mover.as", u8"angelscript", instance).IsOk());
}

TEST_CASE("script.pipeline: a ScriptClass-less utility module cooks with empty metadata")
{
    CookBed bed(u8"as_util");
    bed.WriteSource(u8"util.as", u8"int Helper = 42;\n");
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"util.as", u8"angelscript", instance).IsOk());
    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr);
    CHECK(cooked->className.IsEmpty());
    CHECK(cooked->properties.IsEmpty());
    CHECK(cooked->handlers.IsEmpty());
}

TEST_CASE("script.pipeline: compile errors FAIL the cook and the last good record "
          "survives (old class keeps running)")
{
    CookBed bed(u8"as_errors");
    bed.WriteSource(u8"mover.as", kMoverSource);
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"mover.as", u8"angelscript", instance).IsOk());

    // Break the source; the rebuild must fail without touching the cooked record.
    bed.WriteSource(u8"mover.as", u8"class Mover {\n  this is not valid code at all(\n");
    CHECK_FALSE(bed.Cook(u8"mover.as", u8"angelscript", instance).IsOk());

    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr); // the last good record
    CHECK(cooked->className == u8"Mover");
    CHECK(cooked->source == kMoverSource);
    CHECK(cooked->properties.Size() == 8u);
}

TEST_CASE("script.pipeline: B3 - the neutral builder resolves a per-language COOK "
          "through the registry by the asset's language (never a named cook type)")
{
    // A fake language cook: counts calls, accepts anything, harvests no metadata.
    static int cooked = 0;
    cooked = 0;
    struct FakeCook final : IScriptLanguageCook
    {
        [[nodiscard]] String NewAssetTemplate(ScriptTier, StringView) const override
        {
            return String(u8"// fake\n");
        }
        using IScriptLanguageCook::Cook;
        [[nodiscard]] bool Cook(StringView source, StringView, StringView, CookScriptErrorSink&,
                                ScriptClassSource& out) override
        {
            ++cooked;
            out.language = String(u8"faketest");
            out.source = String(source);
            return true;
        }
        [[nodiscard]] bool Check(StringView, StringView, StringView, CookScriptErrorSink&, String&, Span<const TypeId>) override
        {
            return true;
        }
    };
    ScriptLanguageCookRegistry::Get().Register(
        String(u8"faketest"),
        UniquePtr<IScriptLanguageCook>(DefaultAllocator().New<FakeCook>(), DefaultAllocator()));

    // A backend claims the extension so the drop-importer recognises it (language-clean).
    ScriptBackendDesc fake;
    fake.languageId = String(u8"faketest");
    fake.displayName = String(u8"FakeTest");
    fake.fileExtensions.PushBack(String(u8"ftl"));
    fake.create = [](IAllocator&) -> RefPtr<IScriptManager> { return {}; };
    ScriptBackendRegistry::Get().Register(Move(fake));

    CookBed bed(u8"as_b3");
    bed.WriteSource(u8"fake.ftl", u8"anything goes - the fake cook accepts it\n");
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"fake.ftl", u8"faketest", instance).IsOk());
    CHECK(cooked == 1); // the cook came from the registry, by language

    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* record = Cast<ScriptClassSource>(object.Get());
    REQUIRE(record != nullptr);
    CHECK(record->language == u8"faketest");
    CHECK(record->properties.IsEmpty()); // the fake cook harvests nothing

    // An asset naming an unregistered language fails the cook cleanly (no cook resolves).
    content::Instance* second =
        bed.outputDb->RootGroup()->CreateInstance(u8"cooked2", ScriptClassSource::StaticType());
    ScriptClassAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"fake.ftl");
    asset.language = String(u8"nosuchlang");
    ScriptClassAssetBuilder builder;
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = bed.sources.Get();
    ctx.output = second;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    // The importer accepts exactly the registered extensions (language-clean).
    ScriptFileImporter importer;
    CHECK(importer.Accepts(u8"as"));
    CHECK(importer.Accepts(u8"ftl"));
    CHECK_FALSE(importer.Accepts(u8"lua"));
}

TEST_CASE("script.pipeline: the New Asset starter template cooks with its declared "
          "property + handlers")
{
    CookBed bed(u8"as_starter");
    bed.WriteSource(u8"starter.as", u8"class NewBehavior {\n"
                                    u8"    [1.0, \"units per second\"] float speed;\n"
                                    u8"    Entity@ self;\n"
                                    u8"    NewBehavior(Entity@ entity) { @self = entity; }\n"
                                    u8"    void onStart() {}\n"
                                    u8"    void onUpdate(double dt) {}\n"
                                    u8"    void onDestroy() {}\n"
                                    u8"}\n");
    content::Instance* instance = nullptr;
    REQUIRE(bed.Cook(u8"starter.as", u8"angelscript", instance).IsOk());
    RefPtr<ISerializable> object = instance->ReadObject();
    ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
    REQUIRE(cooked != nullptr);
    CHECK(cooked->className == u8"NewBehavior");
    REQUIRE(cooked->properties.Size() == 1u);
    CHECK(cooked->properties[0].name == u8"speed");
    CHECK(cooked->properties[0].type == ScriptPropertyType::Float);
    CHECK(cooked->handlers.Size() == 3u); // onStart, onUpdate, onDestroy
}

TEST_CASE("script.pipeline: ScriptSourceDocument is the ScriptPage save->recook seam - it "
          "round-trips source, cooks an updated product, and surfaces compile errors while "
          "the last-good product survives")
{
    CookBed bed(u8"as_scriptpage");
    bed.WriteSource(u8"mover.as", kMoverSource);

    // Load: the document reads the bound source file into its edit buffer.
    ScriptSourceDocument doc;
    doc.Bind(bed.srcDir.AsView(), u8"mover.as", u8"angelscript");
    REQUIRE(doc.Load().IsOk());
    CHECK(doc.Source() == kMoverSource);
    CHECK_FALSE(doc.IsModified());

    // Validate: a compile-check through the same language cook, with no product write.
    CHECK(doc.Validate());
    CHECK(doc.LastCompileOk());
    CHECK(doc.ClassName() == u8"Mover");
    CHECK(doc.Errors().Size() == 0u);

    // The recook (the builder over the saved file) produces an updated cooked ScriptClass.
    content::Instance* product = nullptr;
    REQUIRE(bed.Cook(u8"mover.as", u8"angelscript", product).IsOk());
    {
        RefPtr<ISerializable> object = product->ReadObject();
        ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->className == u8"Mover");
    }

    // Edit to a broken script through the document and save it to disk.
    constexpr StringView kBroken = u8"class Mover {\n"
                                   u8"    onStart() { this is not valid code )( }\n";
    doc.SetSource(kBroken);
    CHECK(doc.IsModified());
    REQUIRE(doc.Save().IsOk());
    CHECK_FALSE(doc.IsModified());

    // The bytes persisted (a fresh document reads them back).
    ScriptSourceDocument reopened;
    reopened.Bind(bed.srcDir.AsView(), u8"mover.as", u8"angelscript");
    REQUIRE(reopened.Load().IsOk());
    CHECK(reopened.Source() == kBroken);

    // Validate now surfaces compile errors (file/line and message) and compiles false.
    CHECK_FALSE(doc.Validate());
    CHECK_FALSE(doc.LastCompileOk());
    REQUIRE(doc.Errors().Size() >= 1u);
    CHECK_FALSE(doc.Errors().Data()[0].message.IsEmpty());

    // The recook of the now-broken source fails, and the last good cooked product survives.
    CHECK_FALSE(bed.Cook(u8"mover.as", u8"angelscript", product).IsOk());
    {
        RefPtr<ISerializable> object = product->ReadObject();
        ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->className == u8"Mover"); // unchanged: the failed cook never wrote
    }
}

// A behaviour's class takes its asset's name, as an identifier.
TEST_CASE("script.pipeline: a class name from an asset name, and the starter that spells it")
{
    CHECK(ClassNameFor(u8"Player Controller", u8"NewBehavior") == u8"PlayerController");
    CHECK(ClassNameFor(u8"2d-mover", u8"NewBehavior") == u8"_2dmover");
    CHECK(ClassNameFor(u8"!!", u8"NewBehavior") == u8"NewBehavior");
    CHECK(FillStarterClassName(u8"class {ClassName} { {ClassName}() {} }", u8"Mover") ==
          u8"class Mover { Mover() {} }");
    CHECK(FillStarterClassName(u8"class {ClassName}", {}) == u8"class NewBehavior");
}

TEST_CASE("script.pipeline: ScriptSourceDocument::Validate compiles without running - a global "
          "initializer that faults passes the check, and an error carries its column")
{
    CookBed bed(u8"as_validate_check");
    // Cook builds with the initializers run, so this one fails at cook; the check never runs it.
    constexpr StringView kFaulting = u8"int Zero() { return 0; }\n"
                                     u8"int boom = 10 / Zero();\n"
                                     u8"class Mover\n"
                                     u8"{\n"
                                     u8"    [4.5] float speed;\n"
                                     u8"    Mover(Entity@ entity) {}\n"
                                     u8"}\n";
    bed.WriteSource(u8"mover.as", kFaulting);
    ScriptSourceDocument doc;
    doc.Bind(bed.srcDir.AsView(), u8"mover.as", u8"angelscript");
    REQUIRE(doc.Load().IsOk());
    CHECK(doc.Validate());
    CHECK(doc.ClassName() == u8"Mover");
    CHECK(doc.Errors().Size() == 0u);
    content::Instance* product = nullptr;
    CHECK_FALSE(bed.Cook(u8"mover.as", u8"angelscript", product).IsOk());

    doc.SetSource(u8"class Mover\n"
                  u8"{\n"
                  u8"    void onStart() { int b = ; }\n"
                  u8"}\n");
    CHECK_FALSE(doc.Validate());
    REQUIRE(doc.Errors().Size() >= 1u);
    CHECK(doc.Errors().Data()[0].line == 3);
    CHECK(doc.Errors().Data()[0].column == 30);
}

// An imported script plays the class its file is named for, wherever its copy lands: kept beside
// another file's ("Battle.2.as") it still plays Battle, where the file's stem names no class and
// the first one (EffectDef) used to be played instead. A class set by hand survives a re-import of
// the same file; with no class recorded, a file declaring several and none named like it does not
// cook.
TEST_CASE("script.pipeline: an imported script plays the class its file is named for, wherever its copy lands")
{
    CookBed bed(u8"as_import_class");
    const String root = String(u8"scratch_scriptpipe_import_class");
    CookBed::RemoveTree(root.AsView());
    const String sources = PathJoin(root.AsView(), u8"Sources");
    const String one = PathJoin(root.AsView(), u8"one");
    const String two = PathJoin(root.AsView(), u8"two");
    const String db = PathJoin(root.AsView(), u8"db");
    for (const String* d : {&sources, &one, &two, &db})
        REQUIRE(CreateDirectories(d->AsView()));
    auto write = [](const String& folder, StringView text)
    {
        const String path = PathJoin(folder.AsView(), u8"Battle.as");
        REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
        return path;
    };
    const String first = write(one, u8"class EffectDef { int turns; }\nclass Battle { void onStart() {} }\n");
    const String second = write(two, u8"class EffectDef { int turns; }\nclass Fighter { int hp; }\nclass Battle { void onUpdate(double dt) {} }\n");

    foundation::vfs::NativeFileSystem mount(db.AsView(), DefaultAllocator());
    content::ContentDatabase sourceDb(DefaultAllocator(), mount, BinarySerializerFactory(), u8".asset");
    content::Group& group = *sourceDb.RootGroup();
    ImportContext context{DefaultAllocator(), sources.AsView()};
    context.ledgerPath = ImportLedgerPath(PathJoin(root.AsView(), u8".cache").AsView());
    ScriptFileImporter importer;
    // A copy of an instance's asset (the asset type is not copyable: field by field).
    auto copyOf = [](const ScriptClassAsset& from, ScriptClassAsset& to)
    {
        to.fileName = from.fileName;
        to.language = from.language;
        to.className = from.className;
    };
    auto assetOf = [&](content::Instance& instance, ScriptClassAsset& to)
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        const ScriptClassAsset* asset = Cast<ScriptClassAsset>(object.Get());
        REQUIRE(asset != nullptr);
        copyOf(*asset, to);
    };
    foundation::vfs::NativeFileSystem sourcesMount(sources.AsView(), DefaultAllocator());
    auto cookClass = [&](const ScriptClassAsset& asset, String& className)
    {
        content::Instance* out = bed.outputDb->RootGroup()->GetInstance(u8"cooked");
        if (out == nullptr)
            out = bed.outputDb->RootGroup()->CreateInstance(u8"cooked", ScriptClassSource::StaticType());
        ScriptClassAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = out;
        const bool ok = builder.Build(asset, ctx).IsOk();
        RefPtr<ISerializable> object = out->ReadObject();
        if (const ScriptClassSource* cooked = Cast<ScriptClassSource>(object.Get()); ok && cooked != nullptr)
            className = cooked->className;
        return ok;
    };

    Result<content::Instance*> made = importer.Import(first.AsView(), context, group, nullptr, nullptr, nullptr);
    REQUIRE(made.HasValue());
    ScriptClassAsset madeAsset;
    assetOf(*made.Value(), madeAsset);
    CHECK(madeAsset.className == u8"Battle");

    context.onConflict = ImportConflictChoice::KeepBoth;
    Result<content::Instance*> beside = importer.Import(second.AsView(), context, group, nullptr, nullptr, nullptr);
    REQUIRE(beside.HasValue());
    ScriptClassAsset kept;
    assetOf(*beside.Value(), kept);
    CHECK(kept.fileName.View() == StringView(u8"Battle.2.as"));
    CHECK(kept.className == u8"Battle");
    String played;
    REQUIRE(cookClass(kept, played));
    CHECK(played == u8"Battle");

    // Nothing recorded: Battle.2 names no class of the three, and the cook says so.
    ScriptClassAsset unrecorded;
    copyOf(kept, unrecorded);
    unrecorded.className = String{};
    String none;
    CHECK_FALSE(cookClass(unrecorded, none));

    // A class chosen by hand survives a re-import of the same file (under its asset's name, as the
    // editor's reimport asks for it).
    ScriptClassAsset chosen;
    copyOf(kept, chosen);
    chosen.className = String(u8"Fighter");
    REQUIRE(beside.Value()->WriteObject(chosen).IsOk());
    context.onConflict = ImportConflictChoice::Refuse;
    RefPtr<ImportOptions> named = MakeRef<ImportOptions>(DefaultAllocator());
    named->selection = SingleAssetPlan(second.AsView());
    named->selection.entries[0].targetName = String(beside.Value()->Name());
    Result<content::Instance*> again = importer.Import(second.AsView(), context, group, named.Get(), nullptr, nullptr);
    REQUIRE(again.HasValue());
    CHECK(again.Value() == beside.Value());
    ScriptClassAsset reimported;
    assetOf(*again.Value(), reimported);
    CHECK(reimported.className == u8"Fighter");
    CookBed::RemoveTree(root.AsView());
}

// A script asset stored before it recorded a class (data version 0, as the sample projects keep
// them) still reads, with no class, so it plays the one named like its file as before; written
// again, it carries the field.
TEST_CASE("script.pipeline: a version 0 script asset reads with no class recorded")
{
    const String root = String(u8"scratch_scriptpipe_v0");
    CookBed::RemoveTree(root.AsView());
    REQUIRE(CreateDirectories(root.AsView()));
    RegisterScriptAssets();
    const String text = Format(u8"<root>\n"
                               u8"\t<string name=\"guid\">20ce951c-b789-4c02-b3b0-63e3ba3993ef</string>\n"
                               u8"\t<string name=\"typeNamespace\">rtti::pipeline::script</string>\n"
                               u8"\t<string name=\"typeName\">ScriptClassAsset</string>\n"
                               u8"\t<array name=\"dataVersions\" count=\"1\">\n"
                               u8"\t\t<u64 name=\"type\">{}</u64>\n"
                               u8"\t\t<u32 name=\"version\">0</u32>\n"
                               u8"\t</array>\n"
                               u8"\t<object name=\"payload\">\n"
                               u8"\t\t<string name=\"fileName\">Paper.as</string>\n"
                               u8"\t\t<string name=\"language\">angelscript</string>\n"
                               u8"\t</object>\n"
                               u8"</root>\n",
                               ScriptClassAsset::StaticType().id);
    const String file = PathJoin(root.AsView(), u8"Paper.xasset");
    REQUIRE(WriteFile(file.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    {
        foundation::vfs::NativeFileSystem mount(root.AsView(), DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        content::Instance* instance = db.RootGroup()->GetInstance(u8"Paper");
        REQUIRE(instance != nullptr);
        RefPtr<ISerializable> object = instance->ReadObject();
        const ScriptClassAsset* asset = Cast<ScriptClassAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName.View() == StringView(u8"Paper.as"));
        CHECK(asset->className.IsEmpty());
        CHECK(PreferredScriptClass(*asset) == u8"Paper");
        ScriptClassAsset updated;
        updated.fileName = asset->fileName;
        updated.language = asset->language;
        updated.className = String(u8"Paper");
        REQUIRE(instance->WriteObject(updated).IsOk());
    }
    Result<Array<byte>> written = ReadFile(file.AsView());
    REQUIRE(written.HasValue());
    const StringView saved(reinterpret_cast<const utf8char*>(written.Value().Data()), written.Value().Size());
    CHECK(saved.Contains(u8"name=\"className\""));
    CHECK(saved.Contains(u8"<u32 name=\"version\">1</u32>"));
    CookBed::RemoveTree(root.AsView());
}
