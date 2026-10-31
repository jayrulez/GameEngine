// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:exports`: exporting a collection from the library, either as CSV or
// as files. The CSV has a row per member (the collection's path, the member's name, the file, the
// tags, the note), optionally including sub-collections. The file copy puts a collection's models
// into a folder for a game project, with a subfolder per sub-collection. Each model is renamed to
// its member name when it has one (keeping its extension) and its companion files are copied with
// it at the same relative place: the images its materials name (or the loose texture rule would
// pick) and a glTF's buffers. The copy reports what was copied, what was missing and what clashed;
// an existing different file is never overwritten. The `export.csv` and `export.copy_files`
// commands run these.

module;
#include "Core/Prelude.h"

export module modelviewer:exports;

import foundation.core;
import foundation.json;
import foundation.model;
import :library;
import :store;
import :commands;

using namespace foundation::core;

export namespace modelviewer
{
    using foundation::json::JsonValue;

    namespace export_detail
    {
        // A name made safe for a file or folder: characters a file system cannot hold become _.
        [[nodiscard]] inline String SafeName(StringView name)
        {
            String safe(name);
            for (usize i = 0; i < safe.Size(); ++i)
            {
                const utf8char c = safe[i];
                if (c == u8'/' || c == u8'\\' || c == u8':' || c == u8'*' || c == u8'?' || c == u8'"' || c == u8'<' || c == u8'>' || c == u8'|')
                    safe[i] = u8'_';
            }
            return safe.IsEmpty() ? String(u8"_") : safe;
        }
        [[nodiscard]] inline StringView Extension(StringView file)
        {
            for (usize i = file.Size(); i > 0 && file[i - 1] != u8'/' && file[i - 1] != u8'\\'; --i)
                if (file[i - 1] == u8'.')
                    return file.SubStr(i - 1, file.Size() - i + 1);
            return {};
        }
        [[nodiscard]] inline StringView Folder(StringView path)
        {
            usize slash = path.Size();
            while (slash > 0 && path[slash - 1] != u8'/' && path[slash - 1] != u8'\\')
                --slash;
            return slash > 0 ? path.SubStr(0, slash - 1) : StringView{};
        }
        // A CSV field, quoted when it holds a comma, a quote or a line break, with quotes doubled.
        [[nodiscard]] inline String Field(StringView text)
        {
            bool quote = false;
            for (const utf8char c : text)
                quote = quote || c == u8',' || c == u8'"' || c == u8'\n' || c == u8'\r';
            if (!quote)
                return String(text);
            String out(u8"\"");
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == u8'"')
                    out += u8"\"";
                out += text.SubStr(i, 1);
            }
            out += u8"\"";
            return out;
        }
        [[nodiscard]] inline bool SameFile(StringView a, StringView b)
        {
            u64 sizeA = 0;
            u64 sizeB = 0;
            i64 modified = 0;
            if (!FileStat(a, sizeA, modified) || !FileStat(b, sizeB, modified) || sizeA != sizeB)
                return false;
            Result<Array<byte>> bytesA = ReadFile(a);
            Result<Array<byte>> bytesB = ReadFile(b);
            if (!bytesA.HasValue() || !bytesB.HasValue())
                return false;
            for (usize i = 0; i < bytesA.Value().Size(); ++i)
                if (bytesA.Value()[i] != bytesB.Value()[i])
                    return false;
            return true;
        }
    }

    /// A collection's path from the top-level collection, such as "Moonfall/Act 1".
    [[nodiscard]] inline String CollectionPath(const Library& library, const Guid& id)
    {
        String path;
        const Collection* c = library.FindCollection(id);
        for (u32 depth = 0; c != nullptr && depth < 64; ++depth)
        {
            path = path.IsEmpty() ? c->name : Format(u8"{}/{}", c->name.AsView(), path.AsView());
            c = c->parent.IsNil() ? nullptr : library.FindCollection(c->parent);
        }
        return path;
    }

    /// The collection's CSV, header first, including sub-collections when `recursive`.
    [[nodiscard]] inline String ExportCsv(const Library& library, const Guid& id, bool recursive, u32* rows = nullptr)
    {
        using namespace export_detail;
        String csv(u8"collection,name,file,tags,note\n");
        Array<Guid> scope;
        if (recursive)
            library.Subtree(id, scope);
        else if (library.FindCollection(id) != nullptr)
            scope.PushBack(id);
        u32 count = 0;
        for (const Guid& g : scope)
        {
            const String path = CollectionPath(library, g);
            for (const Member& m : library.FindCollection(g)->members)
                if (const Entry* e = library.FindEntry(m.entry))
                {
                    String tags;
                    for (const String& tag : e->tags)
                    {
                        if (!tags.IsEmpty())
                            tags += u8";";
                        tags += tag.AsView();
                    }
                    csv += Format(u8"{},{},{},{},{}\n", Field(path.AsView()).AsView(), Field(m.name.AsView()).AsView(), Field(e->path.AsView()).AsView(),
                                  Field(tags.AsView()).AsView(), Field(e->note.AsView()).AsView())
                               .AsView();
                    ++count;
                }
        }
        if (rows != nullptr)
            *rows = count;
        return csv;
    }

    /// A file a model needs beside it: an image its materials name (after the loose texture rule,
    /// when on) or a glTF buffer, with its path relative to the model's folder (just its file name
    /// when it lives elsewhere). CompanionFiles returns false when the model cannot be read.
    struct Companion
    {
        String source;
        String relative;
    };
    [[nodiscard]] inline bool CompanionFiles(StringView modelPath, bool looseTextures, Array<Companion>& out)
    {
        using namespace export_detail;
        const StringView folder = Folder(modelPath);
        auto add = [&](StringView source)
        {
            for (const Companion& c : out)
                if (c.source.AsView() == source)
                    return;
            Companion c;
            c.source = String(source);
            const bool under = !folder.IsEmpty() && source.Size() > folder.Size() + 1 && source.StartsWith(folder) &&
                               (source[folder.Size()] == u8'/' || source[folder.Size()] == u8'\\');
            c.relative = under ? String(source.SubStr(folder.Size() + 1, source.Size() - folder.Size() - 1)) : FileTitle(source);
            out.PushBack(foundation::core::Move(c));
        };
        foundation::model::Model model;
        ModelStats stats;
        if (OpenModel::LoadFile(modelPath, looseTextures, model, stats) != OpenModel::Outcome::Opened)
            return false;
        // A texture names its file the way the loader recorded it: the file it read (FBX, a DDS),
        // else its uri beside the model (glTF). A data uri is inside the file and is skipped.
        for (const foundation::model::ModelTexture* texture : model.textures())
            if (texture != nullptr)
            {
                if (!texture->sourceFile().IsEmpty())
                    add(texture->sourceFile());
                else if (!texture->uri().IsEmpty() && !texture->uri().StartsWith(u8"data:"))
                    add(PathJoin(folder, texture->uri()).AsView());
            }
        if (Extension(modelPath) == StringView(u8".gltf"))
            if (Result<Array<byte>> bytes = ReadFile(modelPath); bytes.HasValue())
            {
                const JsonValue gltf = JsonValue::Parse(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
                const JsonValue buffers = gltf.Get(String(u8"buffers"));
                for (i64 i = 0; i < buffers.Count(); ++i)
                {
                    const String uri = buffers.At(i).Get(String(u8"uri")).AsString();
                    if (!uri.IsEmpty() && !uri.AsView().StartsWith(u8"data:"))
                        add(PathJoin(folder, uri.AsView()).AsView());
                }
            }
        return true;
    }

    struct CopyReport
    {
        struct Copied
        {
            String from;
            String to;
        };
        struct Missing
        {
            String model; // the model it belongs to (the file itself when the model is missing)
            String file;
        };
        Array<Copied> copied;
        Array<Missing> missing;
        Array<String> clashes; // a different file already there, left untouched
        u32 skipped = 0;       // an identical file already there
    };

    /// Copies the collection's model files and their companions into `destination`, including
    /// sub-collections (a subfolder each) when `recursive`.
    [[nodiscard]] inline CopyReport CopyCollectionFiles(const Library& library, const Guid& id, StringView destination, bool recursive,
                                                       bool looseTextures)
    {
        using namespace export_detail;
        CopyReport report;
        auto copy = [&](StringView model, StringView from, StringView to)
        {
            if (!FileExists(from))
            {
                report.missing.PushBack(CopyReport::Missing{String(model), String(from)});
                return;
            }
            if (FileExists(to))
            {
                if (SameFile(from, to))
                    ++report.skipped;
                else
                    report.clashes.PushBack(String(to));
                return;
            }
            (void)CreateDirectories(Folder(to));
            if (FileCopyPreserving(from, to))
                report.copied.PushBack(CopyReport::Copied{String(from), String(to)});
            else
                report.missing.PushBack(CopyReport::Missing{String(model), String(from)});
        };
        // Each collection's folder: the destination, then the names from the exported collection down.
        auto folderOf = [&](const Guid& g)
        {
            String relative;
            for (const Collection* c = library.FindCollection(g); c != nullptr && c->id != id;
                 c = c->parent.IsNil() ? nullptr : library.FindCollection(c->parent))
                relative = relative.IsEmpty() ? SafeName(c->name.AsView()) : PathJoin(SafeName(c->name.AsView()).AsView(), relative.AsView());
            return relative.IsEmpty() ? String(destination) : PathJoin(destination, relative.AsView());
        };
        Array<Guid> scope;
        if (recursive)
            library.Subtree(id, scope);
        else if (library.FindCollection(id) != nullptr)
            scope.PushBack(id);
        for (const Guid& g : scope)
        {
            const String into = folderOf(g);
            (void)CreateDirectories(into.AsView());
            for (const Member& m : library.FindCollection(g)->members)
            {
                const Entry* e = library.FindEntry(m.entry);
                if (e == nullptr)
                    continue;
                const String fileName = m.name.IsEmpty() ? FileTitle(e->path.AsView())
                                                         : Format(u8"{}{}", SafeName(m.name.AsView()).AsView(), Extension(e->path.AsView()));
                if (!FileExists(e->path.AsView()))
                {
                    report.missing.PushBack(CopyReport::Missing{e->path, e->path});
                    continue;
                }
                copy(e->path.AsView(), e->path.AsView(), PathJoin(into.AsView(), fileName.AsView()).AsView());
                Array<Companion> companions;
                if (!CompanionFiles(e->path.AsView(), looseTextures, companions))
                    continue; // the model is copied alone; the files it names could not be read
                for (const Companion& c : companions)
                    copy(e->path.AsView(), c.source.AsView(), PathJoin(into.AsView(), c.relative.AsView()).AsView());
            }
        }
        return report;
    }

    /// Registers `export.csv` and `export.copy_files` over the session's library. `looseTextures`
    /// reports whether the loose texture rule is on in the settings.
    inline void RegisterExportCommands(LibrarySession& session, Function<bool()> looseTextures)
    {
        LibrarySession* s = &session;
        auto collection = [s](const JsonValue& a, Guid& id, String& error)
        {
            if (!Guid::TryParse(a.Get(String(u8"id")).AsString().AsView(), id) || s->Get().FindCollection(id) == nullptr)
            {
                error = String(u8"id is not a collection's");
                return false;
            }
            return true;
        };
        auto recursive = [](const JsonValue& a) { return !a.Has(String(u8"recursive")) || a.Get(String(u8"recursive")).AsBool(true); };
        {
            static constexpr CommandArg kArgs[] = {{u8"id", u8"string", u8"the collection"},
                                                   {u8"recursive", u8"boolean", u8"with the collections under it (default true)", false},
                                                   {u8"path", u8"string", u8"the .csv to write (default: the CSV is returned)", false}};
            Command c;
            c.name = String(u8"export.csv");
            c.description = String(u8"A collection as CSV, a row per model: collection path, name, file, tags, note.");
            c.schema = ArgsSchema(Span<const CommandArg>(kArgs, 3));
            c.kind = CommandKind::Reads;
            c.run = [s, collection, recursive](const JsonValue& a)
            {
                Guid id;
                String error;
                if (!collection(a, id, error))
                    return CommandResult::Fail(error.AsView());
                u32 rows = 0;
                const String csv = ExportCsv(s->Get(), id, recursive(a), &rows);
                JsonValue v = JsonValue::MakeObject();
                v.Set(String(u8"rows"), JsonValue::MakeNumber(rows));
                const String path = a.Get(String(u8"path")).AsString();
                if (path.IsEmpty())
                    v.Set(String(u8"csv"), JsonValue::MakeString(csv));
                else
                {
                    (void)CreateDirectories(export_detail::Folder(path.AsView()));
                    if (!WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(csv.Data()), csv.Size())).IsOk())
                        return CommandResult::Fail(Format(u8"{} could not be written", path.AsView()).AsView());
                    v.Set(String(u8"file"), JsonValue::MakeString(path));
                }
                return CommandResult::Ok(foundation::core::Move(v));
            };
            (void)session.Commands().Register(foundation::core::Move(c));
        }
        {
            static constexpr CommandArg kArgs[] = {{u8"id", u8"string", u8"the collection"},
                                                   {u8"folder", u8"string", u8"where the files go"},
                                                   {u8"recursive", u8"boolean", u8"with the collections under it, a subfolder each (default true)", false}};
            Command c;
            c.name = String(u8"export.copy_files");
            c.description = String(u8"Copies a collection's model files, renamed to their names there, with the images and buffers they need, "
                                   u8"into a folder; reports what was copied, missing or already there and different.");
            c.schema = ArgsSchema(Span<const CommandArg>(kArgs, 3));
            c.kind = CommandKind::Creates;
            c.run = [s, collection, recursive, loose = foundation::core::Move(looseTextures)](const JsonValue& a)
            {
                Guid id;
                String error;
                if (!collection(a, id, error))
                    return CommandResult::Fail(error.AsView());
                const String folder = a.Get(String(u8"folder")).AsString();
                if (folder.IsEmpty())
                    return CommandResult::Fail(u8"folder is required");
                const CopyReport report = CopyCollectionFiles(s->Get(), id, folder.AsView(), recursive(a), loose ? loose() : true);
                JsonValue v = JsonValue::MakeObject();
                JsonValue copied = JsonValue::MakeArray();
                for (const CopyReport::Copied& f : report.copied)
                {
                    JsonValue row = JsonValue::MakeObject();
                    row.Set(String(u8"from"), JsonValue::MakeString(f.from));
                    row.Set(String(u8"to"), JsonValue::MakeString(f.to));
                    copied.Add(foundation::core::Move(row));
                }
                JsonValue missing = JsonValue::MakeArray();
                for (const CopyReport::Missing& f : report.missing)
                {
                    JsonValue row = JsonValue::MakeObject();
                    row.Set(String(u8"model"), JsonValue::MakeString(f.model));
                    row.Set(String(u8"file"), JsonValue::MakeString(f.file));
                    missing.Add(foundation::core::Move(row));
                }
                JsonValue clashes = JsonValue::MakeArray();
                for (const String& f : report.clashes)
                    clashes.Add(JsonValue::MakeString(f));
                v.Set(String(u8"copied"), foundation::core::Move(copied));
                v.Set(String(u8"missing"), foundation::core::Move(missing));
                v.Set(String(u8"clashes"), foundation::core::Move(clashes));
                v.Set(String(u8"alreadyThere"), JsonValue::MakeNumber(report.skipped));
                return CommandResult::Ok(foundation::core::Move(v));
            };
            (void)session.Commands().Register(foundation::core::Move(c));
        }
    }
}
