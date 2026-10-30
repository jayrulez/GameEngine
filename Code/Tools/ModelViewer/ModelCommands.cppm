// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:commands`: every operation as a command (Documentation/Specs/
// model-viewer.md, P6). A command has a name (`collection.create`), its arguments as JSON with a
// schema, a JSON result, and whether it changes anything; CommandSet runs them by name, and tells
// its listeners after each change, so the window refreshes whoever ran it (a button or an agent
// through MCP, which serves the same set). LibrarySession holds the open library: its file, the
// undo history (the library as it was before each change, the last 50) and the library's commands.

module;
#include "Core/Prelude.h"

export module modelviewer:commands;

import foundation.core;
import foundation.json;
import :library;
import :store;
import :search;
import :shortlist;

using namespace foundation::core;

export namespace modelviewer
{
    using foundation::json::JsonValue;

    struct CommandResult
    {
        bool ok = true;
        JsonValue value;
        String error;

        [[nodiscard]] static CommandResult Ok(JsonValue value = JsonValue::MakeObject())
        {
            CommandResult r;
            r.value = foundation::core::Move(value);
            return r;
        }
        [[nodiscard]] static CommandResult Fail(StringView error)
        {
            CommandResult r;
            r.ok = false;
            r.error = String(error);
            return r;
        }
    };

    /// What a command does to the library or the window, as MCP's tool annotations say it: an
    /// agent's client runs reads freely and confirms the rest. Everything but Reads is a change
    /// (announced, and for the library undoable).
    enum class CommandKind : u8
    {
        Reads,   // changes nothing
        Creates, // adds something new (a model, a collection, a membership)
        Changes, // replaces what is there (a name, a note, a place); the same call again lands the same
        Deletes, // takes something away
        Adjusts, // the window's state (a tab, the camera), nothing kept
    };

    struct Command
    {
        String name;
        String description;
        JsonValue schema; // a JSON Schema object for the arguments
        CommandKind kind = CommandKind::Reads;
        Function<CommandResult(const JsonValue&)> run;

        [[nodiscard]] bool Changes() const noexcept { return kind != CommandKind::Reads; }
    };

    /// One argument of a command's schema.
    struct CommandArg
    {
        StringView name;
        StringView type; // string, number, integer, boolean, array (of strings), object
        StringView description;
        bool required = true;
    };

    /// A JSON Schema object for `args`.
    [[nodiscard]] inline JsonValue ArgsSchema(Span<const CommandArg> args)
    {
        JsonValue schema = JsonValue::MakeObject();
        schema.Set(String(u8"type"), JsonValue::MakeString(String(u8"object")));
        JsonValue properties = JsonValue::MakeObject();
        JsonValue required = JsonValue::MakeArray();
        for (const CommandArg& arg : args)
        {
            JsonValue property = JsonValue::MakeObject();
            property.Set(String(u8"type"), JsonValue::MakeString(String(arg.type)));
            property.Set(String(u8"description"), JsonValue::MakeString(String(arg.description)));
            if (arg.type == StringView(u8"array"))
            {
                JsonValue items = JsonValue::MakeObject();
                items.Set(String(u8"type"), JsonValue::MakeString(String(u8"string")));
                property.Set(String(u8"items"), foundation::core::Move(items));
            }
            properties.Set(String(arg.name), foundation::core::Move(property));
            if (arg.required)
                required.Add(JsonValue::MakeString(String(arg.name)));
        }
        schema.Set(String(u8"properties"), foundation::core::Move(properties));
        schema.Set(String(u8"required"), foundation::core::Move(required));
        return schema;
    }

    class CommandSet
    {
    public:
        explicit CommandSet(IAllocator& allocator) : m_commands(allocator), m_listeners(allocator) {}

        /// Adds a command; a name registered twice is refused (false).
        bool Register(Command command)
        {
            if (Find(command.name.AsView()) != nullptr)
                return false;
            m_commands.PushBack(foundation::core::Move(command));
            return true;
        }
        [[nodiscard]] const Command* Find(StringView name) const
        {
            for (const Command& c : m_commands)
                if (c.name.AsView() == name)
                    return &c;
            return nullptr;
        }
        [[nodiscard]] Span<const Command> All() const { return m_commands.AsSpan(); }

        /// Runs `name` with `args` (an object; null reads as none); after a change that went
        /// through, each listener hears the command's name.
        CommandResult Execute(StringView name, const JsonValue& args = JsonValue::MakeObject())
        {
            const Command* command = Find(name);
            if (command == nullptr)
                return CommandResult::Fail(Format(u8"no command named {}", name).AsView());
            const JsonValue object = args.IsObject() ? args : JsonValue::MakeObject();
            CommandResult result = command->run(object);
            if (result.ok && command->Changes())
                for (const auto& listener : m_listeners)
                    listener(name);
            return result;
        }

        void AddListener(Function<void(StringView)> listener) { m_listeners.PushBack(foundation::core::Move(listener)); }

    private:
        Array<Command> m_commands;
        Array<Function<void(StringView)>> m_listeners;
    };

    /// The library open in the viewer: its file, its undo history and its commands.
    class LibrarySession
    {
    public:
        static constexpr usize kHistory = 50;
        static constexpr StringView kShortlistName = u8"shortlist.txt";

        explicit LibrarySession(IAllocator& allocator)
            : m_library(allocator), m_commands(allocator), m_history(allocator)
        {
        }

        /// Opens the library in `folder`; a shortlist found there (P4's) comes in as the Shortlist
        /// collection once, its file renamed `.imported`. False when the library does not read.
        bool Open(StringView folder, String& error)
        {
            if (!m_file.Open(folder, m_library, error))
                return false;
            const String shortlist = PathJoin(folder, kShortlistName);
            if (!m_file.ReadOnly() && FileExists(shortlist.AsView()))
                if (Result<Array<byte>> bytes = ReadFile(shortlist.AsView()); bytes.HasValue())
                {
                    (void)ImportShortlistText(m_library, StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
                    (void)m_file.Save(m_library);
                    (void)FileMove(shortlist.AsView(), Format(u8"{}.imported", shortlist.AsView()).AsView());
                }
            return true;
        }

        [[nodiscard]] Library& Get() noexcept { return m_library; }
        [[nodiscard]] const Library& Get() const noexcept { return m_library; }
        [[nodiscard]] bool ReadOnly() const noexcept { return m_file.ReadOnly(); }
        [[nodiscard]] CommandSet& Commands() noexcept { return m_commands; }
        [[nodiscard]] LibraryFile& File() noexcept { return m_file; }

        /// A change under `label`: the library as it was kept for undo, `change` run, the file
        /// written when it went through (the library put back as it was when it did not).
        CommandResult Change(StringView label, const Function<CommandResult()>& change)
        {
            if (m_file.ReadOnly())
                return CommandResult::Fail(
                    Format(u8"the library is open read-only: another viewer (process {}) writes it; work through its MCP port", m_file.LockHolder())
                        .AsView());
            String before = m_library.Write();
            CommandResult result = change();
            if (!result.ok)
            {
                String ignored;
                (void)m_library.Read(before.AsView(), ignored);
                return result;
            }
            if (m_history.Size() >= kHistory)
                m_history.RemoveAt(0);
            m_history.PushBack(Step{String(label), foundation::core::Move(before)});
            (void)m_file.Save(m_library);
            return result;
        }

        /// Steps back over the last change; false when there is none (or read-only).
        bool Undo(String& label)
        {
            if (m_history.IsEmpty() || m_file.ReadOnly())
                return false;
            Step step = foundation::core::Move(m_history.Back());
            m_history.PopBack();
            String error;
            if (!m_library.Read(step.before.AsView(), error))
                return false;
            (void)m_file.Save(m_library);
            label = foundation::core::Move(step.label);
            return true;
        }
        /// The changes undo would step back over, the last first.
        void HistoryLabels(Array<String>& out) const
        {
            for (usize i = m_history.Size(); i-- > 0;)
                out.PushBack(m_history[i].label);
        }

    private:
        struct Step
        {
            String label;
            String before;
        };

        Library m_library;
        LibraryFile m_file;
        CommandSet m_commands;
        Array<Step> m_history;
    };

    namespace command_detail
    {
        [[nodiscard]] inline String Text(const JsonValue& args, StringView key) { return args.Get(String(key)).AsString(); }
        [[nodiscard]] inline bool Has(const JsonValue& args, StringView key) { return args.Has(String(key)) && !args.Get(String(key)).IsNull(); }

        /// An id argument; false (and `error` set) when it is missing or not a guid.
        [[nodiscard]] inline bool Id(const JsonValue& args, StringView key, Guid& out, String& error, bool required = true)
        {
            out = Guid{};
            if (!Has(args, key))
            {
                if (required)
                    error = Format(u8"{} is required", key);
                return !required;
            }
            if (!Guid::TryParse(Text(args, key).AsView(), out))
            {
                error = Format(u8"{} is not an id", key);
                return false;
            }
            return true;
        }
        /// A list of ids (an array of strings).
        [[nodiscard]] inline bool Ids(const JsonValue& args, StringView key, Array<Guid>& out, String& error)
        {
            const JsonValue list = args.Get(String(key));
            if (!list.IsArray())
            {
                error = Format(u8"{} is required: a list of ids", key);
                return false;
            }
            for (const JsonValue& item : list.Items())
            {
                Guid g;
                if (!Guid::TryParse(item.AsString().AsView(), g))
                {
                    error = Format(u8"{} holds {}, which is not an id", key, item.AsString().AsView());
                    return false;
                }
                out.PushBack(g);
            }
            return true;
        }

        [[nodiscard]] inline JsonValue IdJson(const Guid& g) { return JsonValue::MakeString(Format(u8"{}", g)); }

        [[nodiscard]] inline JsonValue EntryJson(const Library& library, const Entry& e)
        {
            JsonValue v = JsonValue::MakeObject();
            v.Set(String(u8"id"), IdJson(e.id));
            v.Set(String(u8"path"), JsonValue::MakeString(e.path));
            v.Set(String(u8"file"), JsonValue::MakeString(FileTitle(e.path.AsView())));
            JsonValue tags = JsonValue::MakeArray();
            for (const String& t : e.tags)
                tags.Add(JsonValue::MakeString(t));
            v.Set(String(u8"tags"), foundation::core::Move(tags));
            v.Set(String(u8"note"), JsonValue::MakeString(e.note));
            v.Set(String(u8"missing"), JsonValue::MakeBool(Library::IsMissing(e)));
            v.Set(String(u8"changed"), JsonValue::MakeBool(Library::IsChanged(e)));
            if (e.stats.known)
            {
                v.Set(String(u8"rigged"), JsonValue::MakeBool(e.stats.rigged));
                v.Set(String(u8"clips"), JsonValue::MakeNumber(e.stats.clips));
                v.Set(String(u8"triangles"), JsonValue::MakeNumber(e.stats.triangles));
            }
            JsonValue collections = JsonValue::MakeArray();
            Array<Guid> in;
            library.CollectionsOf(e.id, in);
            for (const Guid& g : in)
            {
                JsonValue c = JsonValue::MakeObject();
                c.Set(String(u8"id"), IdJson(g));
                c.Set(String(u8"collection"), JsonValue::MakeString(library.FindCollection(g)->name));
                c.Set(String(u8"name"), JsonValue::MakeString(String(library.MemberName(g, e.id))));
                collections.Add(foundation::core::Move(c));
            }
            v.Set(String(u8"collections"), foundation::core::Move(collections));
            return v;
        }

        [[nodiscard]] inline JsonValue TreeJson(const Library& library, const Guid& parent)
        {
            JsonValue list = JsonValue::MakeArray();
            for (const Collection& c : library.Collections())
            {
                if (c.parent != parent)
                    continue;
                JsonValue node = JsonValue::MakeObject();
                node.Set(String(u8"id"), IdJson(c.id));
                node.Set(String(u8"name"), JsonValue::MakeString(c.name));
                node.Set(String(u8"members"), JsonValue::MakeNumber(static_cast<f64>(c.members.Size())));
                node.Set(String(u8"children"), TreeJson(library, c.id));
                list.Add(foundation::core::Move(node));
            }
            return list;
        }
    }

    /// `viewer.info`: what the host says about itself (`info`'s object: its process, version, window
    /// or headless), with the library's folder and whether it is writable added.
    inline void RegisterInfoCommand(LibrarySession& session, StringView libraryFolder, Function<JsonValue()> info)
    {
        LibrarySession* s = &session;
        const String folder(libraryFolder);
        Command c;
        c.name = String(u8"viewer.info");
        c.description = String(u8"The viewer: its process, its version, window or headless, where the library is, whether it is writable "
                               u8"(read-only when another viewer holds it, named by its process).");
        c.schema = ArgsSchema(Span<const CommandArg>{});
        c.kind = CommandKind::Reads;
        c.run = [s, folder, info = foundation::core::Move(info)](const JsonValue&)
        {
            JsonValue v = info ? info() : JsonValue::MakeObject();
            v.Set(String(u8"library"), JsonValue::MakeString(folder));
            v.Set(String(u8"writable"), JsonValue::MakeBool(!s->ReadOnly()));
            if (s->ReadOnly() && s->File().LockHolder() != 0)
                v.Set(String(u8"heldBy"), JsonValue::MakeNumber(static_cast<f64>(s->File().LockHolder())));
            return CommandResult::Ok(foundation::core::Move(v));
        };
        (void)session.Commands().Register(foundation::core::Move(c));
    }

    /// The library's commands into `session`'s set: `entry.*`, `collection.*`, `member.*`,
    /// `tag.*`, `search`, `history.*`.
    inline void RegisterLibraryCommands(LibrarySession& session)
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
