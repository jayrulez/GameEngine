// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:commands`: every operation as a command. A command has a name
// (`collection.create`), JSON arguments with a schema, a JSON result, and a kind saying whether it
// changes anything. CommandSet runs commands by name and notifies its listeners after each change,
// so the window refreshes whether a button or an agent ran it (MCP serves the same set).
// LibrarySession holds the open library: its file, the undo history (a copy of the library before
// each of the last 50 changes) and the library's commands.

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

    /// What a command does to the library or the window, mapped onto MCP's tool annotations: an
    /// agent's client runs reads freely and asks to confirm the rest. Every kind but Reads is a
    /// change, announced to listeners and, for the library, undoable.
    enum class CommandKind : u8
    {
        Reads,   // changes nothing
        Creates, // adds something new (a model, a collection, a membership)
        Changes, // replaces what is there (a name, a note, a place); repeating the call has no further effect
        Deletes, // takes something away
        Adjusts, // the window's state (a tab, the camera); nothing is saved
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

        /// Runs `name` with `args` (an object; null counts as no arguments). After a change that
        /// succeeds, each listener is told the command's name.
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

        /// Opens the library in `folder`. A shortlist file found there is imported once as the
        /// Shortlist collection and renamed `.imported`. False when the library cannot be read.
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

        /// Runs `change` as an undoable step named `label`. The library is copied for undo first;
        /// if the change succeeds the file is written, otherwise the library is restored.
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

        /// Undoes the last change; false when there is none or the library is read-only.
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
        /// The labels of the changes undo can revert, most recent first.
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

    /// `viewer.info`: the object `info` returns (the host's process, version, and whether it has a
    /// window), with the library's folder and whether it is writable added.
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
    // Defined in ModelCommandsImpl.cpp: inline in this interface, gcc 15 emits some of its lambdas
    // twice in one object ("symbol already defined" at assembly), so it lives in one unit.
    void RegisterLibraryCommands(LibrarySession& session);
}
