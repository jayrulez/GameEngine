// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A collection taken out of the model viewer's library (Documentation/Specs/model-viewer.md,
// P11), through its commands: the CSV's rows with and without what is under the collection, its
// fields quoted where they must be; the copy's layout (a subfolder per sub-collection), a model
// renamed to its name there with its extension kept, a glTF's buffer and image beside it where
// they were, a missing model reported, and a second copy over the first neither clashing nor
// copying again, while a different file there already is left alone and said so.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.vfs;
import modelviewer;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace vfs = foundation::vfs;

namespace
{
    String TestModel(StringView relative)
    {
        static const String root = vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return vfs::DataPath(root.AsView(), relative);
    }

    String Folder(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }

    JsonValue Obj() { return JsonValue::MakeObject(); }
    JsonValue With(JsonValue v, StringView key, JsonValue value)
    {
        v.Set(String(key), foundation::core::Move(value));
        return v;
    }
    JsonValue Str(StringView s) { return JsonValue::MakeString(String(s)); }

    // A library of: Moonfall (the fox, named "hero", tagged and noted) and under it Act 1 (oozi,
    // unnamed, and a model whose file has gone).
    struct Fixture
    {
        modelviewer::LibrarySession session{DefaultAllocator()};
        String moonfall;
        String act1;
        String fox = TestModel(u8"Tests/Models/FoxNested/glTF/Fox.gltf");
        String oozi = TestModel(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
        String gone;

        explicit Fixture(StringView folder)
        {
            String error;
            REQUIRE(session.Open(PathJoin(folder, u8"library").AsView(), error));
            modelviewer::RegisterLibraryCommands(session);
            modelviewer::RegisterExportCommands(session, []() { return true; });
            modelviewer::CommandSet& set = session.Commands();
            moonfall = set.Execute(u8"collection.create", With(Obj(), u8"name", Str(u8"Moonfall"))).value.Get(String(u8"id")).AsString();
            act1 = set.Execute(u8"collection.create", With(With(Obj(), u8"name", Str(u8"Act 1")), u8"parent", Str(moonfall.AsView())))
                       .value.Get(String(u8"id"))
                       .AsString();
            const String foxId = Add(fox.AsView());
            const String ooziId = Add(oozi.AsView());
            gone = PathJoin(folder, u8"gone.glb");
            REQUIRE(FileCopyPreserving(oozi.AsView(), gone.AsView()));
            const String goneId = Add(gone.AsView());
            REQUIRE(FileDelete(gone.AsView()));
            Member(moonfall.AsView(), foxId.AsView());
            Member(act1.AsView(), ooziId.AsView());
            Member(act1.AsView(), goneId.AsView());
            REQUIRE(set.Execute(u8"member.rename", With(With(With(Obj(), u8"collection", Str(moonfall.AsView())), u8"entry", Str(foxId.AsView())),
                                                         u8"name", Str(u8"hero")))
                        .ok);
            JsonValue ids = JsonValue::MakeArray();
            ids.Add(Str(foxId.AsView()));
            REQUIRE(set.Execute(u8"tag.add", With(With(Obj(), u8"entries", ids), u8"tag", Str(u8"boss"))).ok);
            REQUIRE(set.Execute(u8"entry.note.set", With(With(Obj(), u8"id", Str(foxId.AsView())), u8"note", Str(u8"red, \"quick\""))).ok);
        }
        String Add(StringView path)
        {
            const modelviewer::CommandResult added = session.Commands().Execute(u8"entry.add", With(Obj(), u8"path", Str(path)));
            REQUIRE(added.ok);
            return added.value.Get(String(u8"id")).AsString();
        }
        void Member(StringView collection, StringView entry)
        {
            JsonValue ids = JsonValue::MakeArray();
            ids.Add(Str(entry));
            REQUIRE(session.Commands().Execute(u8"member.add", With(With(Obj(), u8"collection", Str(collection)), u8"entries", ids)).ok);
        }
    };

    usize Lines(StringView text)
    {
        usize n = 0;
        for (const utf8char c : text)
            n += c == u8'\n' ? 1u : 0u;
        return n;
    }
}

TEST_CASE("model viewer exports: a collection's CSV, with and without what is under it")
{
    const String folder = Folder(u8"scratch_viewer_export_csv");
    Fixture f(folder.AsView());
    modelviewer::CommandSet& set = f.session.Commands();

    const modelviewer::CommandResult all = set.Execute(u8"export.csv", With(Obj(), u8"id", Str(f.moonfall.AsView())));
    REQUIRE(all.ok);
    CHECK(all.value.Get(String(u8"rows")).AsInt() == 3);
    const String csv = all.value.Get(String(u8"csv")).AsString();
    CHECK(Lines(csv.AsView()) == 4); // the header and three rows
    CHECK(csv.AsView().StartsWith(u8"collection,name,file,tags,note\n"));
    // The fox's row: its name, its tag, its note quoted (a comma and quotes in it).
    CHECK(csv.AsView().ContainsIgnoreCase(Format(u8"Moonfall,hero,{},boss,\"red, \"\"quick\"\"\"\n", f.fox.AsView()).AsView()));
    CHECK(csv.AsView().ContainsIgnoreCase(Format(u8"Moonfall/Act 1,,{},,\n", f.oozi.AsView()).AsView()));

    // Without what is under it: the fox alone.
    const modelviewer::CommandResult top =
        set.Execute(u8"export.csv", With(With(Obj(), u8"id", Str(f.moonfall.AsView())), u8"recursive", JsonValue::MakeBool(false)));
    CHECK(top.value.Get(String(u8"rows")).AsInt() == 1);

    // To a file.
    const String path = PathJoin(folder.AsView(), u8"out/moonfall.csv");
    const modelviewer::CommandResult written = set.Execute(u8"export.csv", With(With(Obj(), u8"id", Str(f.moonfall.AsView())), u8"path", Str(path.AsView())));
    REQUIRE(written.ok);
    Result<Array<byte>> bytes = ReadFile(path.AsView());
    REQUIRE(bytes.HasValue());
    CHECK(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()) == csv.AsView());

    CHECK_FALSE(set.Execute(u8"export.csv", With(Obj(), u8"id", Str(u8"not an id"))).ok);
}

TEST_CASE("model viewer exports: files copied out, renamed, with what they need, missing ones reported")
{
    const String folder = Folder(u8"scratch_viewer_export_copy");
    Fixture f(folder.AsView());
    const String out = PathJoin(folder.AsView(), u8"Game");
    modelviewer::CommandSet& set = f.session.Commands();
    const JsonValue args = With(With(Obj(), u8"id", Str(f.moonfall.AsView())), u8"folder", Str(out.AsView()));
    const modelviewer::CommandResult copied = set.Execute(u8"export.copy_files", args);
    REQUIRE(copied.ok);

    // The fox at the top, renamed, its buffer and its image where the glTF names them.
    CHECK(FileExists(PathJoin(out.AsView(), u8"hero.gltf").AsView()));
    CHECK(FileExists(PathJoin(out.AsView(), u8"Fox.bin").AsView()));
    CHECK(FileExists(PathJoin(out.AsView(), u8"textures/Texture.png").AsView()));
    CHECK_FALSE(FileExists(PathJoin(out.AsView(), u8"Fox.gltf").AsView()));
    // Act 1's in a subfolder of its own, unnamed so its file's name.
    CHECK(FileExists(PathJoin(out.AsView(), u8"Act 1/character-oozi.glb").AsView()));
    // Its texture is outside the GLB, under Textures beside it: taken along to the same place.
    CHECK(FileExists(PathJoin(out.AsView(), u8"Act 1/Textures/colormap.png").AsView()));
    CHECK(copied.value.Get(String(u8"copied")).Count() == 5);
    // The model whose file has gone: reported, nothing made for it.
    REQUIRE(copied.value.Get(String(u8"missing")).Count() == 1);
    CHECK(copied.value.Get(String(u8"missing")).At(0).Get(String(u8"file")).AsString() == f.gone);
    CHECK_FALSE(FileExists(PathJoin(out.AsView(), u8"Act 1/gone.glb").AsView()));
    CHECK(copied.value.Get(String(u8"clashes")).Count() == 0);

    // Again: the same files there already, nothing copied and nothing clashing.
    const modelviewer::CommandResult again = set.Execute(u8"export.copy_files", args);
    REQUIRE(again.ok);
    CHECK(again.value.Get(String(u8"copied")).Count() == 0);
    CHECK(again.value.Get(String(u8"alreadyThere")).AsInt() == 5);
    CHECK(again.value.Get(String(u8"clashes")).Count() == 0);

    // A different file where one would go: left as it was, and said so.
    const StringView other = u8"not the fox's buffer";
    REQUIRE(WriteFile(PathJoin(out.AsView(), u8"Fox.bin").AsView(), Span<const byte>(reinterpret_cast<const byte*>(other.Data()), other.Size())).IsOk());
    const modelviewer::CommandResult clashing = set.Execute(u8"export.copy_files", args);
    REQUIRE(clashing.ok);
    REQUIRE(clashing.value.Get(String(u8"clashes")).Count() == 1);
    CHECK(clashing.value.Get(String(u8"clashes")).At(0).AsString().AsView().ContainsIgnoreCase(u8"Fox.bin"));
    Result<Array<byte>> kept = ReadFile(PathJoin(out.AsView(), u8"Fox.bin").AsView());
    REQUIRE(kept.HasValue());
    CHECK(kept.Value().Size() == other.Size());

    // Without what is under it: the top's files alone.
    const String topOnly = PathJoin(folder.AsView(), u8"TopOnly");
    const modelviewer::CommandResult top =
        set.Execute(u8"export.copy_files", With(With(args, u8"folder", Str(topOnly.AsView())), u8"recursive", JsonValue::MakeBool(false)));
    REQUIRE(top.ok);
    CHECK(FileExists(PathJoin(topOnly.AsView(), u8"hero.gltf").AsView()));
    CHECK_FALSE(DirectoryExists(PathJoin(topOnly.AsView(), u8"Act 1").AsView()));
}
