// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's library (Documentation/Specs/model-viewer.md, P6), worked through its
// commands as the window and an agent work it: the round trip of the file; names unique across a
// top-level collection; deleting a collection keeps its models; the search's terms and scopes;
// undo; the shortlist brought in once; a moved file noticed and relinked; a second opener
// read-only.
#include <doctest/doctest.h>
#include <initializer_list>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import modelviewer;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace
{
    String Folder(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }

    String Model(StringView folder, StringView name)
    {
        const String path = PathJoin(folder, name);
        const StringView text = u8"model";
        REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())));
        return path;
    }

    struct Pair
    {
        StringView key;
        StringView value;
    };
    JsonValue Args(std::initializer_list<Pair> pairs)
    {
        JsonValue a = JsonValue::MakeObject();
        for (const Pair& p : pairs)
            a.Set(String(p.key), JsonValue::MakeString(String(p.value)));
        return a;
    }
    JsonValue List(Span<const String> items)
    {
        JsonValue list = JsonValue::MakeArray();
        for (const String& i : items)
            list.Add(JsonValue::MakeString(i));
        return list;
    }
    String Field(const JsonValue& v, StringView key) { return v.Get(String(key)).AsString(); }

    // A session opened on `folder`, the library's commands registered.
    struct Session
    {
        modelviewer::LibrarySession session{DefaultAllocator()};
        explicit Session(StringView folder)
        {
            String error;
            REQUIRE(session.Open(folder, error));
            modelviewer::RegisterLibraryCommands(session);
        }

        modelviewer::CommandResult Run(StringView name, JsonValue args = JsonValue::MakeObject())
        {
            return session.Commands().Execute(name, args);
        }
        // Runs a command that must go through; its result.
        JsonValue Ok(StringView name, JsonValue args = JsonValue::MakeObject())
        {
            modelviewer::CommandResult r = Run(name, foundation::core::Move(args));
            if (!r.ok)
                FAIL_CHECK(reinterpret_cast<const char*>(Format(u8"{} failed: {}", name, r.error.AsView()).CStr()));
            return r.value;
        }
        String Add(StringView path) { return Field(Ok(u8"entry.add", Args({{u8"path", path}})), u8"id"); }
        String Collection(StringView name, StringView parent = {})
        {
            JsonValue a = Args({{u8"name", name}});
            if (!parent.IsEmpty())
                a.Set(String(u8"parent"), JsonValue::MakeString(String(parent)));
            return Field(Ok(u8"collection.create", a), u8"id");
        }
        void Join(StringView collection, Span<const String> entries)
        {
            JsonValue a = Args({{u8"collection", collection}});
            a.Set(String(u8"entries"), List(entries));
            (void)Ok(u8"member.add", a);
        }
        void Tag(StringView entry, StringView tag)
        {
            JsonValue a = Args({{u8"tag", tag}});
            const String one(entry);
            a.Set(String(u8"entries"), List(Span<const String>(&one, 1)));
            (void)Ok(u8"tag.add", a);
        }
        modelviewer::CommandResult Name(StringView collection, StringView entry, StringView name)
        {
            return Run(u8"member.rename", Args({{u8"collection", collection}, {u8"entry", entry}, {u8"name", name}}));
        }
        usize Found(StringView query, StringView scope = {})
        {
            JsonValue a = Args({{u8"query", query}});
            if (!scope.IsEmpty())
                a.Set(String(u8"scope"), JsonValue::MakeString(String(scope)));
            return static_cast<usize>(Ok(u8"search", a).Count());
        }
        usize Members(StringView collection) { return static_cast<usize>(Ok(u8"collection.members", Args({{u8"id", collection}})).Count()); }
    };
}

TEST_CASE("model viewer library: entries, nested collections, names, tags and notes read back from the file")
{
    const String folder = Folder(u8"scratch_library_roundtrip");
    const String knight = Model(folder.AsView(), u8"Knight.fbx");
    const String crate = Model(folder.AsView(), u8"Crate.obj");
    String game;
    String k;
    {
        Session a(folder.AsView());
        k = a.Add(knight.AsView());
        CHECK(a.Add(knight.AsView()) == k); // added twice: one entry
        const String c = a.Add(crate.AsView());
        game = a.Collection(u8"Moonfall");
        const String act = a.Collection(u8"Act 1", game.AsView());
        const String both[] = {k, c};
        a.Join(act.AsView(), Span<const String>(both, 2));
        CHECK(a.Name(act.AsView(), k.AsView(), u8"hero").ok);
        a.Tag(k.AsView(), u8"Boss");
        (void)a.Ok(u8"entry.note.set", Args({{u8"id", k.AsView()}, {u8"note", u8"the one with the cape"}}));
    }
    Session b(folder.AsView()); // the first closed and its lock gone: this one writes
    CHECK_FALSE(b.session.ReadOnly());
    const JsonValue entry = b.Ok(u8"entry.get", Args({{u8"id", k.AsView()}}));
    CHECK(Field(entry, u8"note") == u8"the one with the cape");
    CHECK(entry.Get(String(u8"tags")).At(0).AsString() == u8"Boss");
    CHECK(Field(entry.Get(String(u8"collections")).At(0), u8"name") == u8"hero");
    const JsonValue tree = b.Ok(u8"collection.tree");
    REQUIRE(tree.Count() == 1);
    CHECK(Field(tree.At(0), u8"name") == u8"Moonfall");
    CHECK(Field(tree.At(0).Get(String(u8"children")).At(0), u8"name") == u8"Act 1");
    CHECK(b.Members(game.AsView()) == 2u); // with Act 1's
}

TEST_CASE("model viewer library: a name is unique across a top-level collection, and two games may share one")
{
    const String folder = Folder(u8"scratch_library_names");
    Session s(folder.AsView());
    const String knight = s.Add(Model(folder.AsView(), u8"Knight.fbx").AsView());
    const String rogue = s.Add(Model(folder.AsView(), u8"Rogue.fbx").AsView());
    const String moonfall = s.Collection(u8"Moonfall");
    const String act1 = s.Collection(u8"Act 1", moonfall.AsView());
    const String act2 = s.Collection(u8"Act 2", moonfall.AsView());
    const String other = s.Collection(u8"Ember Road");
    s.Join(act1.AsView(), Span<const String>(&knight, 1));
    s.Join(act2.AsView(), Span<const String>(&rogue, 1));
    s.Join(other.AsView(), Span<const String>(&rogue, 1));

    CHECK(s.Name(act1.AsView(), knight.AsView(), u8"hero").ok);
    const modelviewer::CommandResult clash = s.Name(act2.AsView(), rogue.AsView(), u8"HERO"); // any case, another act
    CHECK_FALSE(clash.ok);
    CHECK(clash.error.AsView().ContainsIgnoreCase(u8"Knight.fbx")); // says who holds it
    CHECK(s.Name(other.AsView(), rogue.AsView(), u8"hero").ok);   // another game: free
    CHECK(s.Name(act1.AsView(), knight.AsView(), u8"hero").ok);   // its own name again: fine

    // Ember Road moved under Moonfall would bring a second hero: refused, nothing moved.
    CHECK_FALSE(s.Run(u8"collection.move", Args({{u8"id", other.AsView()}, {u8"parent", moonfall.AsView()}})).ok);
    CHECK(s.Ok(u8"collection.tree").Count() == 2);
    // A collection cannot go inside itself.
    CHECK_FALSE(s.Run(u8"collection.move", Args({{u8"id", moonfall.AsView()}, {u8"parent", act1.AsView()}})).ok);
}

TEST_CASE("model viewer library: deleting a collection takes what is under it, and keeps the models")
{
    const String folder = Folder(u8"scratch_library_delete");
    Session s(folder.AsView());
    const String knight = s.Add(Model(folder.AsView(), u8"Knight.fbx").AsView());
    const String game = s.Collection(u8"Moonfall");
    const String act = s.Collection(u8"Act 1", game.AsView());
    s.Join(act.AsView(), Span<const String>(&knight, 1));
    (void)s.Ok(u8"collection.delete", Args({{u8"id", game.AsView()}}));
    CHECK(s.Ok(u8"collection.tree").Count() == 0);
    CHECK_FALSE(s.Run(u8"collection.members", Args({{u8"id", act.AsView()}})).ok); // gone with its parent
    CHECK(s.Ok(u8"entry.get", Args({{u8"id", knight.AsView()}})).Get(String(u8"collections")).Count() == 0); // still in the library
}

TEST_CASE("model viewer library: the search's terms all hold, over a collection or the whole library")
{
    const String folder = Folder(u8"scratch_library_search");
    Session s(folder.AsView());
    const String knight = s.Add(Model(folder.AsView(), u8"Knight.fbx").AsView());
    const String rogue = s.Add(Model(folder.AsView(), u8"Rogue.fbx").AsView());
    const String crate = s.Add(Model(folder.AsView(), u8"Crate.obj").AsView());
    const String game = s.Collection(u8"Moonfall");
    const String act = s.Collection(u8"Act One", game.AsView());
    const String props = s.Collection(u8"Props");
    const String heroes[] = {knight, rogue};
    s.Join(act.AsView(), Span<const String>(heroes, 2));
    s.Join(props.AsView(), Span<const String>(&crate, 1));
    CHECK(s.Name(act.AsView(), rogue.AsView(), u8"thief_01").ok);
    s.Tag(knight.AsView(), u8"night sky");
    (void)s.Ok(u8"entry.note.set", Args({{u8"id", crate.AsView()}, {u8"note", u8"wooden, for the tavern"}}));
    modelviewer::EntryStats rigged;
    rigged.known = true;
    rigged.rigged = true;
    rigged.clips = 3;
    Guid knightId;
    REQUIRE(Guid::TryParse(knight.AsView(), knightId));
    REQUIRE(s.session.Get().SetStats(knightId, rigged));

    CHECK(s.Found(u8"") == 3u);
    CHECK(s.Found(u8"knight") == 1u);                        // the file's name, any case
    CHECK(s.Found(u8"THIEF") == 1u);                          // a member's name
    CHECK(s.Found(u8"tavern") == 1u);                         // the note
    CHECK(s.Found(u8"tag:\"night sky\"") == 1u);              // a quoted tag
    CHECK(s.Found(u8"collection:moonfall") == 2u);            // with what is under it
    CHECK(s.Found(u8"collection:\"act one\" rigged") == 1u); // every term holds
    CHECK(s.Found(u8"animated") == 1u);
    CHECK(s.Found(u8"", props.AsView()) == 1u); // scoped to a collection
    CHECK(s.Found(u8"knight", props.AsView()) == 0u);
    CHECK(s.Found(u8"", game.AsView()) == 2u); // and those under it
}

TEST_CASE("model viewer library: undo steps back over each change, whoever made it")
{
    const String folder = Folder(u8"scratch_library_undo");
    {
        Session s(folder.AsView());
        const String knight = s.Add(Model(folder.AsView(), u8"Knight.fbx").AsView());
        const String game = s.Collection(u8"Moonfall");
        s.Join(game.AsView(), Span<const String>(&knight, 1));
        CHECK(s.Name(game.AsView(), knight.AsView(), u8"hero").ok);
        CHECK(s.Name(game.AsView(), knight.AsView(), u8"knight_red").ok);
        const JsonValue history = s.Ok(u8"history.list");
        REQUIRE(history.Count() == 5);
        CHECK(history.At(0).AsString() == u8"name a model");

        CHECK(Field(s.Ok(u8"history.undo"), u8"undone") == u8"name a model");
        CHECK(Field(s.Ok(u8"collection.members", Args({{u8"id", game.AsView()}})).At(0), u8"name") == u8"hero");
        (void)s.Ok(u8"history.undo"); // the first name
        (void)s.Ok(u8"history.undo"); // the membership
        CHECK(s.Members(game.AsView()) == 0u);
        (void)s.Ok(u8"history.undo"); // the collection
        CHECK(s.Ok(u8"collection.tree").Count() == 0);
        // A refused change leaves nothing new to undo, and nothing changed.
        CHECK_FALSE(s.Run(u8"collection.create", Args({{u8"name", u8""}})).ok);
        CHECK(s.Ok(u8"history.list").Count() == 1); // the model's add, still there
    }
    Session reopened(folder.AsView()); // the undone state is what was written
    CHECK(reopened.Ok(u8"collection.tree").Count() == 0);
    CHECK(reopened.Found(u8"") == 1u);
}

TEST_CASE("model viewer library: the P4 shortlist comes in once, as the Shortlist collection")
{
    const String folder = Folder(u8"scratch_library_shortlist");
    const String knight = Model(folder.AsView(), u8"Knight.fbx");
    const String crate = Model(folder.AsView(), u8"Crate.obj");
    modelviewer::Shortlist old(DefaultAllocator());
    old.SetNote(knight.AsView(), u8"cape\tand sword");
    old.SetStarred(crate.AsView(), true);
    old.SetStarred(u8"/no/such/model.fbx", true); // gone since: not brought in
    const String text = old.Write();
    REQUIRE(WriteFile(PathJoin(folder.AsView(), u8"shortlist.txt").AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())));
    {
        Session s(folder.AsView());
        const JsonValue tree = s.Ok(u8"collection.tree");
        REQUIRE(tree.Count() == 1);
        CHECK(Field(tree.At(0), u8"name") == u8"Shortlist");
        const JsonValue members = s.Ok(u8"collection.members", Args({{u8"id", Field(tree.At(0), u8"id").AsView()}}));
        REQUIRE(members.Count() == 2);
        CHECK(Field(members.At(0), u8"note") == u8"cape\tand sword");
    }
    CHECK_FALSE(FileExists(PathJoin(folder.AsView(), u8"shortlist.txt").AsView()));
    CHECK(FileExists(PathJoin(folder.AsView(), u8"shortlist.txt.imported").AsView()));
    Session again(folder.AsView());
    CHECK(again.Ok(u8"collection.tree").Count() == 1); // not twice
}

TEST_CASE("model viewer library: a moved file is noticed, and relinked")
{
    const String folder = Folder(u8"scratch_library_relink");
    Session s(folder.AsView());
    const String path = Model(folder.AsView(), u8"Knight.fbx");
    const String id = s.Add(path.AsView());
    const String moved = PathJoin(folder.AsView(), u8"Knight_v2.fbx");
    REQUIRE(FileMove(path.AsView(), moved.AsView()));
    CHECK(s.Ok(u8"entry.get", Args({{u8"id", id.AsView()}})).Get(String(u8"missing")).AsBool());
    CHECK(s.Found(u8"missing") == 1u);
    CHECK_FALSE(s.Run(u8"entry.relink", Args({{u8"id", id.AsView()}, {u8"path", u8"/no/such/file.fbx"}})).ok);
    (void)s.Ok(u8"entry.relink", Args({{u8"id", id.AsView()}, {u8"path", moved.AsView()}}));
    const JsonValue entry = s.Ok(u8"entry.get", Args({{u8"id", id.AsView()}}));
    CHECK_FALSE(entry.Get(String(u8"missing")).AsBool());
    CHECK(Field(entry, u8"file") == u8"Knight_v2.fbx");
}

TEST_CASE("model viewer library: a second opener is read-only while the first holds the lock")
{
    const String folder = Folder(u8"scratch_library_lock");
    const String knight = Model(folder.AsView(), u8"Knight.fbx");
    {
        Session first(folder.AsView());
        CHECK_FALSE(first.session.ReadOnly());
        // The system's lock is per open file: a second opener, even in this process, finds it held.
        Session second(folder.AsView());
        CHECK(second.session.ReadOnly());
        CHECK(second.session.File().LockHolder() == ProcessId()); // the lock file names the holder
        const modelviewer::CommandResult refused = second.Run(u8"entry.add", Args({{u8"path", knight.AsView()}}));
        CHECK_FALSE(refused.ok);
        CHECK(refused.error.AsView().ContainsIgnoreCase(u8"read-only"));
        CHECK(second.Found(u8"") == 0u); // reading still works
        first.Add(knight.AsView());
    }
    // The first closed (as a viewer ending, or crashing, releases it): the next one writes.
    Session third(folder.AsView());
    CHECK_FALSE(third.session.ReadOnly());
    CHECK(third.Found(u8"") == 1u);
}

TEST_CASE("model viewer library: a selection added at once lands in order, at the place asked")
{
    const String folder = Folder(u8"scratch_library_selection");
    Session s(folder.AsView());
    const String a = s.Add(Model(folder.AsView(), u8"A.fbx").AsView());
    const String b = s.Add(Model(folder.AsView(), u8"B.fbx").AsView());
    const String c = s.Add(Model(folder.AsView(), u8"C.fbx").AsView());
    const String game = s.Collection(u8"Moonfall");
    s.Join(game.AsView(), Span<const String>(&a, 1));
    JsonValue add = Args({{u8"collection", game.AsView()}});
    const String selection[] = {b, c, a}; // a is in already: it stays where it is
    add.Set(String(u8"entries"), List(Span<const String>(selection, 3)));
    add.Set(String(u8"at"), JsonValue::MakeNumber(0));
    (void)s.Ok(u8"member.add", add);
    const JsonValue members = s.Ok(u8"collection.members", Args({{u8"id", game.AsView()}}));
    REQUIRE(members.Count() == 3);
    CHECK(Field(members.At(0), u8"file") == u8"B.fbx");
    CHECK(Field(members.At(1), u8"file") == u8"C.fbx");
    CHECK(Field(members.At(2), u8"file") == u8"A.fbx");
}

TEST_CASE("model viewer library: renaming or deleting a tag does it on every model")
{
    const String folder = Folder(u8"scratch_library_tags");
    Session s(folder.AsView());
    const String a = s.Add(Model(folder.AsView(), u8"A.fbx").AsView());
    const String b = s.Add(Model(folder.AsView(), u8"B.fbx").AsView());
    s.Tag(a.AsView(), u8"boss");
    s.Tag(b.AsView(), u8"Boss"); // any case: one tag
    s.Tag(b.AsView(), u8"villain");
    JsonValue counts = s.Ok(u8"tag.list");
    REQUIRE(counts.Count() == 2);
    CHECK(counts.At(0).Get(String(u8"count")).AsNumber() == doctest::Approx(2.0));

    (void)s.Ok(u8"tag.rename", Args({{u8"from", u8"BOSS"}, {u8"to", u8"villain"}})); // b has both: merged
    counts = s.Ok(u8"tag.list");
    REQUIRE(counts.Count() == 1);
    CHECK(Field(counts.At(0), u8"tag") == u8"villain");
    CHECK(counts.At(0).Get(String(u8"count")).AsNumber() == doctest::Approx(2.0));
    CHECK(s.Ok(u8"entry.get", Args({{u8"id", b.AsView()}})).Get(String(u8"tags")).Count() == 1);

    (void)s.Ok(u8"tag.delete", Args({{u8"tag", u8"Villain"}}));
    CHECK(s.Ok(u8"tag.list").Count() == 0);
    CHECK(s.Found(u8"tag:villain") == 0u);
}

TEST_CASE("model viewer library: a change is announced to the views; a read or a refused change is not")
{
    const String folder = Folder(u8"scratch_library_announce");
    Session s(folder.AsView());
    Array<String> heard;
    s.session.Commands().AddListener([&heard](StringView name) { heard.PushBack(String(name)); });
    const String game = s.Collection(u8"Moonfall");
    (void)s.Ok(u8"collection.tree");                               // a read
    CHECK_FALSE(s.Run(u8"collection.create", Args({{u8"name", u8""}})).ok); // refused
    (void)s.Ok(u8"collection.rename", Args({{u8"id", game.AsView()}, {u8"name", u8"Moonfall II"}}));
    REQUIRE(heard.Size() == 2u);
    CHECK(heard[0] == u8"collection.create");
    CHECK(heard[1] == u8"collection.rename");
}
