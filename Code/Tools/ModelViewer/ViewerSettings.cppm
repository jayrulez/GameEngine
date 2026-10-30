// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:settings`: the viewer's preferences and recents
// (Documentation/Specs/model-viewer.md, P9). Kept in `model-viewer/settings.txt` as lines of
// `key value`; a line this viewer does not know is kept as it was (a newer viewer's, or one
// written by hand), so nothing is lost by opening an older viewer. The recent models and folders
// are lines of their own, newest first, as many as the preference keeps. The `settings.get`,
// `settings.set` and `recents.list` commands read and change them, the window's Preferences
// page and an agent alike, and `cache.clear` empties the cooked models and thumbnails.

module;
#include "Core/Prelude.h"

export module modelviewer:settings;

import foundation.core;
import foundation.json;
import :library;
import :commands;

using namespace foundation::core;

export namespace modelviewer
{
    using foundation::json::JsonValue;

    struct ViewerSettings
    {
        String folder;                 // the folder Browse shows
        bool looseTextures = true;
        String screenshotFolder;       // empty: DefaultScreenshotFolder()
        bool askScreenshot = true;     // the Screenshot button asks where; a command never does
        String screenshotPattern = String(u8"{model}_{time}");
        u32 thumbnailSize = 128;       // 96, 128 or 192; from the next start
        String cacheFolder;            // empty: the user data folder's model-viewer; from the next start
        bool loop = true;              // a model's clips start looping
        f32 turntableSpeed = 30.0f;    // degrees a second
        f32 exposure = 0.0f;
        f32 ambient = 0.3f;
        f32 sky = 1.0f;
        bool grid = true;
        String density = String(u8"default"); // the editor's: compact, default, comfortable
        u32 recentCount = 10;
        u32 tileSize = 1;              // the grids' tiles: 0 small, 1 medium, 2 large
        Array<String> recentModels;    // newest first
        Array<String> recentFolders;
        Array<String> unknown;         // lines kept as they were
    };

    inline constexpr u32 kThumbnailSizes[] = {96, 128, 192};

    namespace settings_detail
    {
        [[nodiscard]] inline bool Flag(StringView v) { return v == StringView(u8"1") || v == StringView(u8"true"); }
        [[nodiscard]] inline f32 Number(StringView v, f32 fallback)
        {
            const Optional<f64> n = ParseFloat(v);
            return n.HasValue() ? static_cast<f32>(n.Value()) : fallback;
        }
        [[nodiscard]] inline u32 Whole(StringView v, u32 fallback)
        {
            const Optional<f64> n = ParseFloat(v);
            return n.HasValue() && n.Value() >= 0.0 ? static_cast<u32>(n.Value()) : fallback;
        }
    }

    /// The user's Pictures folder's ModelViewer: where screenshots go unless the preference says.
    [[nodiscard]] inline String DefaultScreenshotFolder()
    {
        Optional<String> home = GetEnvironmentVariable(u8"HOME");
        if (!home.HasValue())
            home = GetEnvironmentVariable(u8"USERPROFILE");
        return home.HasValue() ? PathJoin(PathJoin(home.Value().AsView(), u8"Pictures").AsView(), u8"ModelViewer") : String(u8"ModelViewer");
    }
    [[nodiscard]] inline String ScreenshotFolderOf(const ViewerSettings& s)
    {
        return s.screenshotFolder.IsEmpty() ? DefaultScreenshotFolder() : s.screenshotFolder;
    }
    /// Where the cooked models and the thumbnails go: the preference, else `fallback` (the user
    /// data folder's model-viewer).
    [[nodiscard]] inline String CacheBaseOf(const ViewerSettings& s, StringView fallback)
    {
        return s.cacheFolder.IsEmpty() ? String(fallback) : s.cacheFolder;
    }

    /// The bytes of the files under `folder`, every level down.
    [[nodiscard]] inline u64 FolderBytes(StringView folder) // NOLINT(misc-no-recursion)
    {
        struct Collect
        {
            Array<String>* files;
            Array<String>* folders;
            StringView parent;
        };
        Array<String> files;
        Array<String> folders;
        Collect collect{&files, &folders, folder};
        (void)ListDirectory(folder,
                            [](void* ctx, StringView name, bool isDirectory)
                            {
                                auto* c = static_cast<Collect*>(ctx);
                                (isDirectory ? c->folders : c->files)->PushBack(PathJoin(c->parent, name));
                            },
                            &collect);
        u64 total = 0;
        for (const String& file : files)
        {
            u64 size = 0;
            i64 modified = 0;
            if (FileStat(file.AsView(), size, modified))
                total += size;
        }
        for (const String& sub : folders)
            total += FolderBytes(sub.AsView());
        return total;
    }

    /// A path to the front of a recents list (once), the list cut to `limit`.
    inline void PushRecent(Array<String>& list, StringView path, u32 limit)
    {
        for (usize i = 0; i < list.Size(); ++i)
            if (list[i].AsView() == path)
            {
                list.RemoveAt(i);
                break;
            }
        list.Insert(0, String(path));
        while (list.Size() > limit)
            list.PopBack();
    }

    [[nodiscard]] inline ViewerSettings ReadSettings(StringView text)
    {
        using namespace settings_detail;
        ViewerSettings s;
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != u8'\n')
                ++end;
            StringView line = text.SubStr(start, end - start);
            start = end + 1;
            if (!line.IsEmpty() && line[line.Size() - 1] == u8'\r')
                line = line.SubStr(0, line.Size() - 1);
            if (line.IsEmpty())
                continue;
            usize space = 0;
            while (space < line.Size() && line[space] != u8' ')
                ++space;
            const StringView key = line.SubStr(0, space);
            const StringView value = space < line.Size() ? line.SubStr(space + 1, line.Size() - space - 1) : StringView{};
            if (key == StringView(u8"folder"))
                s.folder = String(value);
            else if (key == StringView(u8"loose"))
                s.looseTextures = Flag(value);
            else if (key == StringView(u8"screenshot-folder"))
                s.screenshotFolder = String(value);
            else if (key == StringView(u8"screenshot-ask"))
                s.askScreenshot = Flag(value);
            else if (key == StringView(u8"screenshot-pattern"))
                s.screenshotPattern = String(value);
            else if (key == StringView(u8"thumbnail-size"))
                s.thumbnailSize = Whole(value, s.thumbnailSize);
            else if (key == StringView(u8"cache-folder"))
                s.cacheFolder = String(value);
            else if (key == StringView(u8"loop"))
                s.loop = Flag(value);
            else if (key == StringView(u8"turntable-speed"))
                s.turntableSpeed = Number(value, s.turntableSpeed);
            else if (key == StringView(u8"exposure"))
                s.exposure = Number(value, s.exposure);
            else if (key == StringView(u8"ambient"))
                s.ambient = Number(value, s.ambient);
            else if (key == StringView(u8"sky"))
                s.sky = Number(value, s.sky);
            else if (key == StringView(u8"grid"))
                s.grid = Flag(value);
            else if (key == StringView(u8"density"))
                s.density = String(value);
            else if (key == StringView(u8"recent-count"))
                s.recentCount = Whole(value, s.recentCount);
            else if (key == StringView(u8"tile-size"))
                s.tileSize = Min(Whole(value, s.tileSize), 2u);
            else if (key == StringView(u8"recent-model"))
                s.recentModels.PushBack(String(value));
            else if (key == StringView(u8"recent-folder"))
                s.recentFolders.PushBack(String(value));
            else
                s.unknown.PushBack(String(line));
        }
        return s;
    }

    [[nodiscard]] inline String WriteSettings(const ViewerSettings& s)
    {
        String text = Format(u8"folder {}\nloose {}\nscreenshot-folder {}\nscreenshot-ask {}\nscreenshot-pattern {}\nthumbnail-size {}\ncache-folder {}\nloop {}\n"
                             u8"turntable-speed {}\nexposure {}\nambient {}\nsky {}\ngrid {}\ndensity {}\nrecent-count {}\ntile-size {}\n",
                             s.folder.AsView(), s.looseTextures ? 1 : 0, s.screenshotFolder.AsView(), s.askScreenshot ? 1 : 0,
                             s.screenshotPattern.AsView(), s.thumbnailSize, s.cacheFolder.AsView(), s.loop ? 1 : 0, s.turntableSpeed, s.exposure, s.ambient, s.sky,
                             s.grid ? 1 : 0, s.density.AsView(), s.recentCount, s.tileSize);
        for (const String& line : s.unknown)
            text += Format(u8"{}\n", line.AsView()).AsView();
        for (const String& path : s.recentModels)
            text += Format(u8"recent-model {}\n", path.AsView()).AsView();
        for (const String& path : s.recentFolders)
            text += Format(u8"recent-folder {}\n", path.AsView()).AsView();
        return text;
    }

    /// A screenshot's file name from the pattern: `{model}` the file's name without its
    /// extension, `{clip}` the clip playing (empty when none), `{time}` a stamp unique enough
    /// (`stamp`, the caller's clock); characters a file name cannot hold become `_`; `.png` added.
    [[nodiscard]] inline String ScreenshotName(StringView pattern, StringView modelPath, StringView clip, u64 stamp)
    {
        String model = FileTitle(modelPath);
        usize dot = model.Size();
        while (dot > 0 && model[dot - 1] != u8'.')
            --dot;
        if (dot > 1)
            model = String(model.AsView().SubStr(0, dot - 1));
        String name;
        for (usize i = 0; i < pattern.Size(); ++i)
        {
            const StringView rest = pattern.SubStr(i, pattern.Size() - i);
            if (rest.StartsWith(u8"{model}"))
            {
                name += model.AsView();
                i += 6;
            }
            else if (rest.StartsWith(u8"{clip}"))
            {
                name += clip;
                i += 5;
            }
            else if (rest.StartsWith(u8"{time}"))
            {
                name += Format(u8"{}", stamp).AsView();
                i += 5;
            }
            else
                name += pattern.SubStr(i, 1);
        }
        for (usize i = 0; i < name.Size(); ++i)
        {
            const utf8char c = name[i];
            if (c == u8'/' || c == u8'\\' || c == u8':' || c == u8'*' || c == u8'?' || c == u8'"' || c == u8'<' || c == u8'>' || c == u8'|')
                name[i] = u8'_';
        }
        if (name.IsEmpty())
            name = String(u8"screenshot");
        name += u8".png";
        return name;
    }

    /// The settings as JSON (what settings.get answers; settings.set takes any part of it).
    [[nodiscard]] inline JsonValue SettingsJson(const ViewerSettings& s)
    {
        JsonValue v = JsonValue::MakeObject();
        v.Set(String(u8"folder"), JsonValue::MakeString(s.folder));
        v.Set(String(u8"looseTextures"), JsonValue::MakeBool(s.looseTextures));
        v.Set(String(u8"screenshotFolder"), JsonValue::MakeString(s.screenshotFolder));
        v.Set(String(u8"askScreenshot"), JsonValue::MakeBool(s.askScreenshot));
        v.Set(String(u8"screenshotPattern"), JsonValue::MakeString(s.screenshotPattern));
        v.Set(String(u8"thumbnailSize"), JsonValue::MakeNumber(s.thumbnailSize));
        v.Set(String(u8"cacheFolder"), JsonValue::MakeString(s.cacheFolder));
        v.Set(String(u8"loop"), JsonValue::MakeBool(s.loop));
        v.Set(String(u8"turntableSpeed"), JsonValue::MakeNumber(s.turntableSpeed));
        v.Set(String(u8"exposure"), JsonValue::MakeNumber(s.exposure));
        v.Set(String(u8"ambient"), JsonValue::MakeNumber(s.ambient));
        v.Set(String(u8"sky"), JsonValue::MakeNumber(s.sky));
        v.Set(String(u8"grid"), JsonValue::MakeBool(s.grid));
        v.Set(String(u8"density"), JsonValue::MakeString(s.density));
        v.Set(String(u8"recentCount"), JsonValue::MakeNumber(s.recentCount));
        v.Set(String(u8"tileSize"), JsonValue::MakeNumber(s.tileSize));
        return v;
    }

    /// Takes what `changes` names into `s` (the rest as it was); false, `s` untouched, when a value
    /// is out of its range (`error` says which).
    [[nodiscard]] inline bool ApplySettingsJson(ViewerSettings& s, const JsonValue& changes, String& error)
    {
        ViewerSettings next = s;
        auto text = [&](StringView key, String& into)
        {
            if (changes.Has(String(key)))
                into = changes.Get(String(key)).AsString();
        };
        auto flag = [&](StringView key, bool& into)
        {
            if (changes.Has(String(key)))
                into = changes.Get(String(key)).AsBool(into);
        };
        auto number = [&](StringView key, f32& into, f64 lo, f64 hi) -> bool
        {
            if (!changes.Has(String(key)))
                return true;
            const f64 v = changes.Get(String(key)).AsNumber(static_cast<f64>(into));
            if (v < lo || v > hi)
            {
                error = Format(u8"{} must be from {} to {}", key, lo, hi);
                return false;
            }
            into = static_cast<f32>(v);
            return true;
        };
        text(u8"folder", next.folder);
        flag(u8"looseTextures", next.looseTextures);
        text(u8"screenshotFolder", next.screenshotFolder);
        flag(u8"askScreenshot", next.askScreenshot);
        text(u8"screenshotPattern", next.screenshotPattern);
        text(u8"cacheFolder", next.cacheFolder);
        flag(u8"loop", next.loop);
        flag(u8"grid", next.grid);
        if (!number(u8"turntableSpeed", next.turntableSpeed, 1.0, 360.0) || !number(u8"exposure", next.exposure, -4.0, 4.0) ||
            !number(u8"ambient", next.ambient, 0.0, 2.0) || !number(u8"sky", next.sky, 0.0, 3.0))
            return false;
        if (changes.Has(String(u8"thumbnailSize")))
        {
            const u32 size = static_cast<u32>(changes.Get(String(u8"thumbnailSize")).AsInt(0));
            bool known = false;
            for (const u32 k : kThumbnailSizes)
                known = known || k == size;
            if (!known)
            {
                error = String(u8"thumbnailSize is 96, 128 or 192");
                return false;
            }
            next.thumbnailSize = size;
        }
        if (changes.Has(String(u8"density")))
        {
            const String d = changes.Get(String(u8"density")).AsString();
            if (d != u8"compact" && d != u8"default" && d != u8"comfortable")
            {
                error = String(u8"density is compact, default or comfortable");
                return false;
            }
            next.density = d;
        }
        if (changes.Has(String(u8"recentCount")))
        {
            const i64 count = changes.Get(String(u8"recentCount")).AsInt(-1);
            if (count < 0 || count > 50)
            {
                error = String(u8"recentCount must be from 0 to 50");
                return false;
            }
            next.recentCount = static_cast<u32>(count);
            while (next.recentModels.Size() > next.recentCount)
                next.recentModels.PopBack();
            while (next.recentFolders.Size() > next.recentCount)
                next.recentFolders.PopBack();
        }
        if (changes.Has(String(u8"tileSize")))
        {
            const i64 tile = changes.Get(String(u8"tileSize")).AsInt(-1);
            if (tile < 0 || tile > 2)
            {
                error = String(u8"tileSize is 0 (small), 1 (medium) or 2 (large)");
                return false;
            }
            next.tileSize = static_cast<u32>(tile);
        }
        if (next.screenshotPattern.IsEmpty())
        {
            error = String(u8"screenshotPattern needs something in it ({model}, {clip}, {time})");
            return false;
        }
        s = foundation::core::Move(next);
        return true;
    }

    /// The settings file and what is in it; `onChanged` hears each change (the window applies it).
    class SettingsStore
    {
    public:
        ViewerSettings value;
        Function<void()> onChanged;

        void Open(StringView path)
        {
            m_path = String(path);
            if (Result<Array<byte>> bytes = ReadFile(m_path.AsView()); bytes.HasValue())
                value = ReadSettings(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        }
        bool Save() const
        {
            if (m_path.IsEmpty())
                return false;
            (void)CreateDirectories(PathParent(m_path.AsView()));
            const String text = WriteSettings(value);
            return WriteFile(m_path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk();
        }
        /// A recent model or folder (the front of its list), kept.
        void Recent(bool folder, StringView path)
        {
            PushRecent(folder ? value.recentFolders : value.recentModels, path, value.recentCount);
            (void)Save();
        }
        [[nodiscard]] StringView Path() const noexcept { return m_path.AsView(); }

    private:
        String m_path;
    };

    /// `settings.get`, `settings.set` and `recents.list` over `store`, and `cache.clear` over the
    /// `cacheFolders` (the cooked models, the thumbnails), refused while `busy` says why not (a
    /// model open in a tab reads its cooked files).
    inline void RegisterSettingsCommands(CommandSet& set, SettingsStore& store, Span<const String> cacheFolders, Function<String()> busy = {})
    {
        Array<String> folders;
        for (const String& f : cacheFolders)
            folders.PushBack(f);
        SettingsStore* st = &store;
        auto make = [](StringView name, StringView description, CommandKind kind, Span<const CommandArg> args)
        {
            Command c;
            c.name = String(name);
            c.description = String(description);
            c.schema = ArgsSchema(args);
            c.kind = kind;
            return c;
        };
        {
            Command c = make(u8"settings.get", u8"The viewer's preferences: screenshots, thumbnails, the cache and its size, the view's defaults, density, recents kept.",
                             CommandKind::Reads, Span<const CommandArg>{});
            c.run = [st, folders](const JsonValue&)
            {
                JsonValue v = SettingsJson(st->value);
                u64 bytes = 0;
                for (const String& f : folders)
                    bytes += FolderBytes(f.AsView());
                v.Set(String(u8"cacheBytes"), JsonValue::MakeNumber(static_cast<f64>(bytes)));
                v.Set(String(u8"screenshotFolderUsed"), JsonValue::MakeString(ScreenshotFolderOf(st->value)));
                return CommandResult::Ok(foundation::core::Move(v));
            };
            (void)set.Register(foundation::core::Move(c));
        }
        {
            static constexpr CommandArg kArgs[] = {
                {u8"screenshotFolder", u8"string", u8"where screenshots go (empty: Pictures/ModelViewer)", false},
                {u8"askScreenshot", u8"boolean", u8"the Screenshot button asks where to save", false},
                {u8"screenshotPattern", u8"string", u8"a screenshot's name: {model}, {clip}, {time}", false},
                {u8"thumbnailSize", u8"integer", u8"96, 128 or 192 (from the viewer's next start)", false},
                {u8"cacheFolder", u8"string", u8"where cooked models and thumbnails go (empty: the user data folder; from the next start)", false},
                {u8"loop", u8"boolean", u8"clips start looping", false},
                {u8"turntableSpeed", u8"number", u8"degrees a second, 1 to 360", false},
                {u8"exposure", u8"number", u8"the view's exposure to start with, -4 to 4", false},
                {u8"ambient", u8"number", u8"its ambient, 0 to 2", false},
                {u8"sky", u8"number", u8"its sky, 0 to 3", false},
                {u8"grid", u8"boolean", u8"the ground grid", false},
                {u8"looseTextures", u8"boolean", u8"the loose texture rule, for models opened after", false},
                {u8"density", u8"string", u8"compact, default or comfortable", false},
                {u8"recentCount", u8"integer", u8"how many recent models and folders to keep, 0 to 50", false},
                {u8"tileSize", u8"integer", u8"the grids' tiles: 0 small, 1 medium, 2 large", false}};
            Command c = make(u8"settings.set", u8"Changes the preferences named (the rest stay); returns them all.", CommandKind::Changes,
                             Span<const CommandArg>(kArgs, sizeof(kArgs) / sizeof(kArgs[0])));
            c.run = [st](const JsonValue& a)
            {
                String error;
                if (!ApplySettingsJson(st->value, a, error))
                    return CommandResult::Fail(error.AsView());
                (void)st->Save();
                if (st->onChanged)
                    st->onChanged();
                return CommandResult::Ok(SettingsJson(st->value));
            };
            (void)set.Register(foundation::core::Move(c));
        }
        {
            Command c = make(u8"recents.list", u8"The models opened and the folders browsed lately, newest first.", CommandKind::Reads,
                             Span<const CommandArg>{});
            c.run = [st](const JsonValue&)
            {
                JsonValue v = JsonValue::MakeObject();
                JsonValue models = JsonValue::MakeArray();
                for (const String& p : st->value.recentModels)
                    models.Add(JsonValue::MakeString(p));
                JsonValue browsed = JsonValue::MakeArray();
                for (const String& p : st->value.recentFolders)
                    browsed.Add(JsonValue::MakeString(p));
                v.Set(String(u8"models"), foundation::core::Move(models));
                v.Set(String(u8"folders"), foundation::core::Move(browsed));
                return CommandResult::Ok(foundation::core::Move(v));
            };
            (void)set.Register(foundation::core::Move(c));
        }
        {
            Command c = make(u8"cache.clear", u8"Deletes the cooked models and the thumbnails (each is made again when next needed); returns the bytes freed.",
                             CommandKind::Deletes, Span<const CommandArg>{});
            c.run = [folders, busy = foundation::core::Move(busy)](const JsonValue&)
            {
                if (busy)
                    if (const String why = busy(); !why.IsEmpty())
                        return CommandResult::Fail(why.AsView());
                u64 freed = 0;
                for (const String& f : folders)
                {
                    freed += FolderBytes(f.AsView());
                    if (!RemoveDirectoryRecursive(f.AsView()))
                        return CommandResult::Fail(Format(u8"{} could not all be deleted", f.AsView()).AsView());
                    (void)CreateDirectories(f.AsView());
                }
                JsonValue v = JsonValue::MakeObject();
                v.Set(String(u8"freed"), JsonValue::MakeNumber(static_cast<f64>(freed)));
                return CommandResult::Ok(foundation::core::Move(v));
            };
            (void)set.Register(foundation::core::Move(c));
        }
    }
}
