// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - the library's commands (RegisterLibraryCommands, declared in `modelviewer:commands`).

module;
#include "Core/Prelude.h"

module modelviewer;

import foundation.core;
import foundation.json;

using namespace foundation::core;

namespace modelviewer
{
    void RegisterLibraryCommands(LibrarySession& session)
    {
        using namespace command_detail;
        LibrarySession* s = &session;
        CommandSet& set = session.Commands();
        auto add = [&](StringView name, StringView description, Span<const CommandArg> args, CommandKind kind, Function<CommandResult(const JsonValue&)> run)
        {
            Command c;
            c.name = String(name);
            c.description = String(description);
            c.schema = ArgsSchema(args);
            c.kind = kind;
            c.run = foundation::core::Move(run);
            (void)set.Register(foundation::core::Move(c));
        };
        // A change: its label, the library kept for undo, the file written (LibrarySession::Change).
        auto change = [s](StringView label, Function<CommandResult()> body) { return s->Change(label, body); };

        // --- entries ---
        {
            const CommandArg args[] = {{u8"path", u8"string", u8"the model file"}};
            add(u8"entry.add", u8"Adds a model file to the library (its entry when it has one); returns its id.", args, CommandKind::Creates,
                [s, change](const JsonValue& a)
                {
                    const String path = Text(a, u8"path");
                    return change(u8"add a model", [s, path]()
                                  {
                                      const bool known = s->Get().FindEntryByPath(path.AsView()) != nullptr;
                                      Guid id;
                                      if (!s->Get().AddEntry(path.AsView(), id))
                                          return CommandResult::Fail(Format(u8"{} is not there", path.AsView()).AsView());
                                      JsonValue v = JsonValue::MakeObject();
                                      v.Set(String(u8"id"), IdJson(id));
                                      v.Set(String(u8"added"), JsonValue::MakeBool(!known));
                                      return CommandResult::Ok(foundation::core::Move(v));
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the model's id"}};
            add(u8"entry.remove", u8"Removes a model from the library, and from every collection it is in.", args, CommandKind::Deletes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    return change(u8"remove a model", [s, id]()
                                  { return s->Get().RemoveEntry(id) ? CommandResult::Ok() : CommandResult::Fail(u8"no such model"); });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the model's id"}, {u8"path", u8"string", u8"where the file is now"}};
            add(u8"entry.relink", u8"Points a model at its file's new place (or a changed file), taking its size and time anew.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    const String path = Text(a, u8"path");
                    return change(u8"relink a model", [s, id, path]()
                                  {
                                      String why;
                                      return s->Get().Relink(id, path.AsView(), why) ? CommandResult::Ok() : CommandResult::Fail(why.AsView());
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the model's id", false}, {u8"path", u8"string", u8"or its file", false}};
            add(u8"entry.get", u8"A model in the library: its file, tags, note, numbers, and the collections it is in with its name in each.", args, CommandKind::Reads,
                [s](const JsonValue& a)
                {
                    const Entry* e = nullptr;
                    Guid id;
                    String error;
                    if (Has(a, u8"id"))
                    {
                        if (!Id(a, u8"id", id, error))
                            return CommandResult::Fail(error.AsView());
                        e = s->Get().FindEntry(id);
                    }
                    else
                        e = s->Get().FindEntryByPath(Text(a, u8"path").AsView());
                    return e != nullptr ? CommandResult::Ok(EntryJson(s->Get(), *e)) : CommandResult::Fail(u8"no such model in the library");
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the model's id"}, {u8"note", u8"string", u8"the note (empty clears it)"}};
            add(u8"entry.note.set", u8"Sets a model's note.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    const String note = Text(a, u8"note");
                    return change(u8"set a note", [s, id, note]()
                                  { return s->Get().SetNote(id, note.AsView()) ? CommandResult::Ok() : CommandResult::Fail(u8"no such model"); });
                });
        }

        // --- collections ---
        {
            const CommandArg args[] = {{u8"name", u8"string", u8"the collection's name"},
                                       {u8"parent", u8"string", u8"the collection it goes under (none: the top)", false}};
            add(u8"collection.create", u8"Creates a collection, at the top or under another; returns its id.", args, CommandKind::Creates,
                [s, change](const JsonValue& a)
                {
                    Guid parent;
                    String error;
                    if (!Id(a, u8"parent", parent, error, false))
                        return CommandResult::Fail(error.AsView());
                    const String name = Text(a, u8"name");
                    return change(u8"create a collection", [s, name, parent]()
                                  {
                                      Guid id;
                                      String why;
                                      if (!s->Get().CreateCollection(name.AsView(), parent, id, why))
                                          return CommandResult::Fail(why.AsView());
                                      JsonValue v = JsonValue::MakeObject();
                                      v.Set(String(u8"id"), IdJson(id));
                                      return CommandResult::Ok(foundation::core::Move(v));
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the collection"}, {u8"name", u8"string", u8"its new name"}};
            add(u8"collection.rename", u8"Renames a collection.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    const String name = Text(a, u8"name");
                    return change(u8"rename a collection", [s, id, name]()
                                  {
                                      String why;
                                      return s->Get().RenameCollection(id, name.AsView(), why) ? CommandResult::Ok() : CommandResult::Fail(why.AsView());
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the collection"},
                                       {u8"parent", u8"string", u8"the collection it goes under (none: the top)", false}};
            add(u8"collection.move", u8"Moves a collection under another, or to the top; refused inside itself or where its names would clash.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    Guid parent;
                    String error;
                    if (!Id(a, u8"id", id, error) || !Id(a, u8"parent", parent, error, false))
                        return CommandResult::Fail(error.AsView());
                    return change(u8"move a collection", [s, id, parent]()
                                  {
                                      String why;
                                      return s->Get().MoveCollection(id, parent, why) ? CommandResult::Ok() : CommandResult::Fail(why.AsView());
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the collection"}};
            add(u8"collection.delete", u8"Deletes a collection and those under it; the models stay in the library.", args, CommandKind::Deletes,
                [s, change](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    return change(u8"delete a collection", [s, id]()
                                  { return s->Get().DeleteCollection(id) ? CommandResult::Ok() : CommandResult::Fail(u8"no such collection"); });
                });
        }
        add(u8"collection.tree", u8"The collections, nested: each with its id, name, member count and children.", Span<const CommandArg>{}, CommandKind::Reads,
            [s](const JsonValue&) { return CommandResult::Ok(TreeJson(s->Get(), Guid{})); });
        {
            const CommandArg args[] = {{u8"id", u8"string", u8"the collection"},
                                       {u8"recursive", u8"boolean", u8"with the members of the collections under it (default true)", false}};
            add(u8"collection.members", u8"A collection's models in order, each with its name there.", args, CommandKind::Reads,
                [s](const JsonValue& a)
                {
                    Guid id;
                    String error;
                    if (!Id(a, u8"id", id, error))
                        return CommandResult::Fail(error.AsView());
                    const Library& library = s->Get();
                    if (library.FindCollection(id) == nullptr)
                        return CommandResult::Fail(u8"no such collection");
                    Array<Guid> scope;
                    if (!Has(a, u8"recursive") || a.Get(String(u8"recursive")).AsBool(true))
                        library.Subtree(id, scope);
                    else
                        scope.PushBack(id);
                    JsonValue list = JsonValue::MakeArray();
                    for (const Guid& g : scope)
                        for (const Member& m : library.FindCollection(g)->members)
                            if (const Entry* e = library.FindEntry(m.entry))
                            {
                                JsonValue v = EntryJson(library, *e);
                                v.Set(String(u8"name"), JsonValue::MakeString(m.name));
                                v.Set(String(u8"in"), IdJson(g));
                                list.Add(foundation::core::Move(v));
                            }
                    return CommandResult::Ok(foundation::core::Move(list));
                });
        }

        // --- members ---
        {
            const CommandArg args[] = {{u8"collection", u8"string", u8"the collection"},
                                       {u8"entries", u8"array", u8"the models' ids"},
                                       {u8"at", u8"integer", u8"the place among its members (default: last)", false}};
            add(u8"member.add", u8"Adds models to a collection (one there already stays where it is).", args, CommandKind::Creates,
                [s, change](const JsonValue& a)
                {
                    Guid collection;
                    Array<Guid> entries;
                    String error;
                    if (!Id(a, u8"collection", collection, error) || !Ids(a, u8"entries", entries, error))
                        return CommandResult::Fail(error.AsView());
                    const i32 at = Has(a, u8"at") ? static_cast<i32>(a.Get(String(u8"at")).AsInt(-1)) : -1;
                    return change(u8"add to a collection", [s, collection, entries, at]()
                                  {
                                      String why;
                                      return s->Get().AddMembers(collection, entries.AsSpan(), at, why) ? CommandResult::Ok() : CommandResult::Fail(why.AsView());
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"collection", u8"string", u8"the collection"}, {u8"entries", u8"array", u8"the models' ids"}};
            add(u8"member.remove", u8"Takes models out of a collection (they stay in the library).", args, CommandKind::Deletes,
                [s, change](const JsonValue& a)
                {
                    Guid collection;
                    Array<Guid> entries;
                    String error;
                    if (!Id(a, u8"collection", collection, error) || !Ids(a, u8"entries", entries, error))
                        return CommandResult::Fail(error.AsView());
                    return change(u8"take out of a collection", [s, collection, entries]()
                                  { return s->Get().RemoveMembers(collection, entries.AsSpan()) ? CommandResult::Ok() : CommandResult::Fail(u8"no such collection"); });
                });
        }
        {
            const CommandArg args[] = {{u8"collection", u8"string", u8"the collection"},
                                       {u8"entry", u8"string", u8"the model's id"},
                                       {u8"name", u8"string", u8"its name there, unique across the top-level collection (empty clears it)"}};
            add(u8"member.rename", u8"Names a model in a collection: the id a game knows it by. A name another model holds there is refused.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid collection;
                    Guid entry;
                    String error;
                    if (!Id(a, u8"collection", collection, error) || !Id(a, u8"entry", entry, error))
                        return CommandResult::Fail(error.AsView());
                    const String name = Text(a, u8"name");
                    return change(u8"name a model", [s, collection, entry, name]()
                                  {
                                      String why;
                                      return s->Get().RenameMember(collection, entry, name.AsView(), why) ? CommandResult::Ok() : CommandResult::Fail(why.AsView());
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"collection", u8"string", u8"the collection"},
                                       {u8"entry", u8"string", u8"the model's id"},
                                       {u8"at", u8"integer", u8"its new place among the members"}};
            add(u8"member.move", u8"Moves a model to another place in a collection's order.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Guid collection;
                    Guid entry;
                    String error;
                    if (!Id(a, u8"collection", collection, error) || !Id(a, u8"entry", entry, error))
                        return CommandResult::Fail(error.AsView());
                    const i32 at = static_cast<i32>(a.Get(String(u8"at")).AsInt(-1));
                    return change(u8"reorder a collection", [s, collection, entry, at]()
                                  { return s->Get().MoveMember(collection, entry, at) ? CommandResult::Ok() : CommandResult::Fail(u8"that model is not in this collection"); });
                });
        }

        // --- tags ---
        {
            const CommandArg args[] = {{u8"entries", u8"array", u8"the models' ids"}, {u8"tag", u8"string", u8"the tag"}};
            add(u8"tag.add", u8"Tags models.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    Array<Guid> entries;
                    String error;
                    if (!Ids(a, u8"entries", entries, error))
                        return CommandResult::Fail(error.AsView());
                    const String tag = Text(a, u8"tag");
                    if (tag.IsEmpty())
                        return CommandResult::Fail(u8"tag is required");
                    return change(u8"tag models", [s, entries, tag]()
                                  {
                                      s->Get().AddTag(entries.AsSpan(), tag.AsView());
                                      return CommandResult::Ok();
                                  });
                });
            add(u8"tag.remove", u8"Takes a tag off models.", args, CommandKind::Deletes,
                [s, change](const JsonValue& a)
                {
                    Array<Guid> entries;
                    String error;
                    if (!Ids(a, u8"entries", entries, error))
                        return CommandResult::Fail(error.AsView());
                    const String tag = Text(a, u8"tag");
                    return change(u8"untag models", [s, entries, tag]()
                                  {
                                      s->Get().RemoveTag(entries.AsSpan(), tag.AsView());
                                      return CommandResult::Ok();
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"from", u8"string", u8"the tag"}, {u8"to", u8"string", u8"its new name"}};
            add(u8"tag.rename", u8"Renames a tag on every model that has it.", args, CommandKind::Changes,
                [s, change](const JsonValue& a)
                {
                    const String from = Text(a, u8"from");
                    const String to = Text(a, u8"to");
                    if (from.IsEmpty() || to.IsEmpty())
                        return CommandResult::Fail(u8"from and to are required");
                    return change(u8"rename a tag", [s, from, to]()
                                  {
                                      s->Get().RenameTag(from.AsView(), to.AsView());
                                      return CommandResult::Ok();
                                  });
                });
        }
        {
            const CommandArg args[] = {{u8"tag", u8"string", u8"the tag"}};
            add(u8"tag.delete", u8"Takes a tag off every model.", args, CommandKind::Deletes,
                [s, change](const JsonValue& a)
                {
                    const String tag = Text(a, u8"tag");
                    return change(u8"delete a tag", [s, tag]()
                                  {
                                      s->Get().DeleteTag(tag.AsView());
                                      return CommandResult::Ok();
                                  });
                });
        }
        add(u8"tag.list", u8"Every tag, with how many models have it.", Span<const CommandArg>{}, CommandKind::Reads,
            [s](const JsonValue&)
            {
                Array<String> tags;
                Array<u32> counts;
                s->Get().TagCounts(tags, counts);
                JsonValue list = JsonValue::MakeArray();
                for (usize i = 0; i < tags.Size(); ++i)
                {
                    JsonValue v = JsonValue::MakeObject();
                    v.Set(String(u8"tag"), JsonValue::MakeString(tags[i]));
                    v.Set(String(u8"count"), JsonValue::MakeNumber(counts[i]));
                    list.Add(foundation::core::Move(v));
                }
                return CommandResult::Ok(foundation::core::Move(list));
            });

        // --- search and history ---
        {
            const CommandArg args[] = {
                {u8"query", u8"string", u8"words (a name, file, note or tag), tag:x, collection:x, rigged, animated, missing; all must hold"},
                {u8"scope", u8"string", u8"a collection to search (with those under it); none: the whole library", false},
                {u8"limit", u8"integer", u8"at most this many (default all)", false}};
            add(u8"search", u8"Finds models in the library.", args, CommandKind::Reads,
                [s](const JsonValue& a)
                {
                    Guid scope;
                    String error;
                    if (!Id(a, u8"scope", scope, error, false))
                        return CommandResult::Fail(error.AsView());
                    const usize limit = Has(a, u8"limit") ? static_cast<usize>(Max<i64>(a.Get(String(u8"limit")).AsInt(0), 0)) : 0;
                    JsonValue list = JsonValue::MakeArray();
                    for (const Guid& g : Search(s->Get(), ParseQuery(Text(a, u8"query").AsView()), scope, limit))
                        list.Add(EntryJson(s->Get(), *s->Get().FindEntry(g)));
                    return CommandResult::Ok(foundation::core::Move(list));
                });
        }
        add(u8"history.undo", u8"Steps back over the last change to the library (whoever made it); says what it was.", Span<const CommandArg>{}, CommandKind::Changes,
            [s](const JsonValue&)
            {
                String label;
                if (!s->Undo(label))
                    return CommandResult::Fail(s->ReadOnly() ? StringView(u8"the library is open read-only") : StringView(u8"nothing to undo"));
                JsonValue v = JsonValue::MakeObject();
                v.Set(String(u8"undone"), JsonValue::MakeString(label));
                return CommandResult::Ok(foundation::core::Move(v));
            });
        add(u8"history.list", u8"The changes undo would step back over, the last first.", Span<const CommandArg>{}, CommandKind::Reads,
            [s](const JsonValue&)
            {
                Array<String> labels;
                s->HistoryLabels(labels);
                JsonValue list = JsonValue::MakeArray();
                for (const String& l : labels)
                    list.Add(JsonValue::MakeString(l));
                return CommandResult::Ok(foundation::core::Move(list));
            });
    }
}
