// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The editor's user-level settings store (<user-data>/editor.settings.xml): the cross-project
// preference sections (fonts, UI scale), the file handling, and the single registration entry
// point for every section type.
//
// Section types may live with their domain (EditorExportSettings with the export code,
// RecentProjectsSettings with the project registry), but they are all registered here, in one
// place, because a section registered without its serializable factory makes Settings::Load abort
// the whole store at that section and silently drop everything after it. RegisterEditorSettingsTypes
// makes both calls for every type in one list.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.core:editor_settings;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.settings;
import editor.project;  // EditorExportSettings (registered below)
import :actions;       // EditorShortcutSettings captures from and applies to the registry

using namespace foundation::core;

export namespace editor
{
    namespace settings = foundation::settings;
    namespace vfs = foundation::vfs;

    // The UI scale preference's range: 50% (a dense layout on a large monitor) to 200%.
    inline constexpr f32 kUiScaleMin = 0.5f;
    inline constexpr f32 kUiScaleMax = 2.0f;

    // Editor-level UI preferences (a Settings section). uiScale multiplies the window's OS content
    // scale for the whole editor UI (layout, fonts and baked icons); it is both an accessibility
    // setting and the way to exercise the DPI path without a scaled monitor. The look's inputs sit
    // beside it: the scheme, the accent, the density and reduced motion.
    class EditorUiSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorUiSettings, ISerializable)
    public:
        f32 uiScale = 1.0f; // clamped to [kUiScaleMin, kUiScaleMax] on use
        // Snapping a scene view to an axis (the orientation gizmo) turns it orthographic, and
        // orbiting away turns it back.
        bool autoOrthographic = true;
        String scheme{u8"dark"};      // "dark" or "light"
        String accent{u8"#2f6fdf"};   // a fill that white text is legible on, as #rrggbb (azure)
        String density{u8"compact"};  // "compact", "default" or "comfortable"
        bool reduceMotion = false;    // keep moving parts of the UI still

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "uiScale", uiScale);
            foundation::core::SerializeAppended(ar, "autoOrthographic", autoOrthographic);
            foundation::core::SerializeAppended(ar, "scheme", scheme);
            foundation::core::SerializeAppended(ar, "accent", accent);
            foundation::core::SerializeAppended(ar, "density", density);
            foundation::core::SerializeAppended(ar, "reduceMotion", reduceMotion);
        }
    };

    /// The user's auto-orthographic preference (EditorUiSettings), on when there is no store
    /// or no section yet.
    [[nodiscard]] inline bool AutoOrthographicViews(const foundation::settings::Settings* user)
    {
        const EditorUiSettings* ui = user != nullptr ? user->Find<EditorUiSettings>() : nullptr;
        return ui == nullptr || ui->autoOrthographic;
    }

    // The editor's settings file, in the user-data dir (hand-editable XML, like the project files).
    inline constexpr StringView kEditorSettingsFile = u8"editor.settings.xml";

    // The editor's MCP host: agent access to the open project over localhost HTTP, off by
    // default. `port` is where it listens (one editor per port, so a second editor takes
    // another); `token` is the bearer secret the editor generates on first enable (see
    // GenerateMcpToken) and also writes to <user-data>/mcp-token so a local agent can configure
    // itself. Edited in Preferences; `--mcp` and `--mcp-port` override it for one run.
    inline constexpr u32 kEditorMcpDefaultPort = 7405;
    class EditorMcpSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorMcpSettings, ISerializable)
    public:
        bool enabled = false;
        u32 port = kEditorMcpDefaultPort;
        String token;

        /// Applies the Preferences fields as typed. `portText` must name a port in 1024..65535
        /// (lower ports need elevation, and 0 cannot go in a client's URL); anything else leaves the
        /// port as it was and returns false. `token` is taken verbatim; empty means a new one is
        /// generated on the next enable.
        bool ApplyFromPreferences(bool enable, StringView portText, StringView newToken)
        {
            enabled = enable;
            token = String(newToken);
            const Optional<i64> parsed = ParseInt(portText);
            if (!parsed.HasValue() || parsed.Value() < 1024 || parsed.Value() > 65535)
            {
                return false;
            }
            port = static_cast<u32>(parsed.Value());
            return true;
        }

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "enabled", enabled);
            foundation::core::Serialize(ar, "port", port);
            foundation::core::Serialize(ar, "token", token);
        }
    };

    // A fresh bearer token for the MCP host: a Guid from OS entropy, in its canonical text (36
    // characters). It is a secret, so it is not seeded by the clock, which a neighbour could guess.
    // Only when the OS refuses entropy does a clock and process seed stand in, with a warning.
    [[nodiscard]] inline String GenerateMcpToken()
    {
        Guid secret;
        if (!Guid::TryGenerateFromSystemEntropy(secret))
        {
            LOG_WARNING(u8"Editor", u8"the OS gave no entropy for the MCP token; minted from the "
                                    u8"clock and the process id instead - treat it as guessable");
            Random rng(GetTicks() ^ (static_cast<u64>(ProcessId()) << 32));
            secret = Guid::Generate(rng);
        }
        utf8char text[37];
        secret.ToChars(text);
        return String(StringView(text, 36));
    }

    namespace detail
    {
        // Converts a chord stored in the older numeric form (foundation.ui's key code, the USB HID
        // usage, and modifier bits) to the binding text the keymap reads ("ctrl-t"); empty for a
        // key it does not name.
        inline String LegacyChordKeys(u32 key, u32 modifiers)
        {
            static constexpr const char8_t* kNamed[][2] = {
                {u8"40", u8"return"},  {u8"41", u8"escape"},    {u8"42", u8"backspace"},  {u8"43", u8"tab"},
                {u8"44", u8"space"},   {u8"45", u8"minus"},     {u8"46", u8"equals"},     {u8"47", u8"leftbracket"},
                {u8"48", u8"rightbracket"}, {u8"49", u8"backslash"}, {u8"51", u8"semicolon"}, {u8"52", u8"apostrophe"},
                {u8"53", u8"grave"},   {u8"54", u8"comma"},     {u8"55", u8"period"},     {u8"56", u8"slash"},
                {u8"73", u8"insert"},  {u8"74", u8"home"},      {u8"75", u8"pageup"},     {u8"76", u8"delete"},
                {u8"77", u8"end"},     {u8"78", u8"pagedown"},  {u8"79", u8"right"},      {u8"80", u8"left"},
                {u8"81", u8"down"},    {u8"82", u8"up"},
            };
            String name;
            if (key >= 4 && key <= 29)
                name.PushBack(static_cast<utf8char>(u8'a' + (key - 4)));
            else if (key >= 30 && key <= 38)
                name.PushBack(static_cast<utf8char>(u8'1' + (key - 30)));
            else if (key == 39)
                name.PushBack(u8'0');
            else if (key >= 58 && key <= 69)
                name = Format(u8"f{}", key - 57);
            else
                for (const auto& named : kNamed)
                    if (Format(u8"{}", key).AsView() == StringView(named[0]))
                        name = String(named[1]);
            if (name.IsEmpty())
                return {};
            String keys;
            if ((modifiers & 0x00C0u) != 0)
                keys += u8"ctrl-";
            if ((modifiers & 0x0300u) != 0)
                keys += u8"alt-";
            if ((modifiers & 0x0003u) != 0)
                keys += u8"shift-";
            keys += name.AsView();
            return keys;
        }
    }

    // One shortcut the user rebound: the action's id and the chord as a binding writes it
    // ("ctrl-shift-z"; empty means the user chose no shortcut, which differs from the
    // declaration's default). A section in the older format stores a key code and modifier bits,
    // which are converted to the text on read.
    struct ShortcutOverrideEntry
    {
        String id;
        String keys;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "id", id);
            if (ar.Mode() == SerializeMode::Read && ar.HasKey("key"))
            {
                u32 key = 0;
                u32 modifiers = 0;
                foundation::core::Serialize(ar, "key", key);
                foundation::core::Serialize(ar, "modifiers", modifiers);
                keys = detail::LegacyChordKeys(key, modifiers);
                return;
            }
            foundation::core::Serialize(ar, "keys", keys);
        }
        [[nodiscard]] EditorShortcut Chord() const { return EditorShortcut::Of(keys.AsView()); }
    };
    inline void Serialize(ISerializer& ar, ShortcutOverrideEntry& e)
    {
        ar.BeginObject();
        e.Serialize(ar);
        ar.EndObject();
    }

    // The user's shortcut overrides (Preferences > Shortcuts), keyed by action id. Captured from
    // the registry on Save and applied to it after every domain has registered its actions. An id
    // nobody registered (a domain not loaded in this build) stays in the section untouched, so a
    // later run still has it.
    class EditorShortcutSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorShortcutSettings, ISerializable)
    public:
        Array<ShortcutOverrideEntry> overrides;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "overrides", overrides);
        }
        [[nodiscard]] const ShortcutOverrideEntry* Find(StringView id) const
        {
            for (const ShortcutOverrideEntry& e : overrides)
            {
                if (e.id.AsView() == id)
                {
                    return &e;
                }
            }
            return nullptr;
        }
    };

    /// One resolution the Game tab offers to preview at.
    struct GamePreviewResolution
    {
        String name; // "Steam Deck"
        u32 width = 1920;
        u32 height = 1080;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
        }
    };
    inline void Serialize(ISerializer& ar, GamePreviewResolution& r)
    {
        ar.BeginObject();
        r.Serialize(ar);
        ar.EndObject();
    }

    /// The user's preview resolutions, a section of the per-user store: sizes to test a game at on
    /// this machine, offered by the Game tab after the project's own resolution and those of its
    /// export presets. They say nothing about the game, which is why they are not stored with the
    /// project.
    class GamePreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(GamePreviewSettings, ISerializable)
    public:
        Array<GamePreviewResolution> presets;
        bool seeded = false; // the defaults are added once, so defaults the user removes stay removed

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "presets", presets);
            foundation::core::Serialize(ar, "seeded", seeded);
        }

        /// Seeds the common sizes the first time. True when it did, so the caller saves.
        bool SeedDefaults()
        {
            if (seeded)
            {
                return false;
            }
            seeded = true;
            const auto add = [this](StringView name, u32 width, u32 height)
            { presets.PushBack(GamePreviewResolution{String(name), width, height}); };
            add(u8"HD", 1280, 720);
            add(u8"Full HD", 1920, 1080);
            add(u8"QHD", 2560, 1440);
            add(u8"Steam Deck", 1280, 800);
            add(u8"Phone portrait", 1080, 2400);
            add(u8"Phone landscape", 2400, 1080);
            return true;
        }

        /// The store's section, seeded on first use (and marked changed then, so it saves). Null
        /// with no store, as when headless and in tests.
        [[nodiscard]] static GamePreviewSettings* From(settings::Settings* store)
        {
            if (store == nullptr)
            {
                return nullptr;
            }
            GamePreviewSettings& section = store->Section<GamePreviewSettings>();
            if (section.SeedDefaults())
            {
                store->MarkChanged<GamePreviewSettings>();
            }
            return &section;
        }
    };

    /// The Game tab's state per project, a section of the project's editor store.
    class GamePageSettings final : public ISerializable
    {
        RTTI_OBJECT(GamePageSettings, ISerializable)
    public:
        /// The chosen resolution's key: "project", "export:<preset>", "preset:<name>" or "panel".
        String resolutionKey;
        /// The tab previews a phone held upright: its screen has a portrait phone's shape, and a
        /// game made for the other orientation is presented rotated.
        bool phonePreview = false;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "resolutionKey", resolutionKey);
            foundation::core::SerializeAppended(ar, "phonePreview", phonePreview);
        }
    };

    /// Copies the registry's overrides into the section: every registered action with an override
    /// is written (added or updated), and an entry for an id the registry does not know is kept.
    /// Returns how many entries the section holds afterwards.
    inline usize CaptureShortcutOverrides(const EditorActionRegistry& actions, EditorShortcutSettings& out)
    {
        for (const EditorActionDeclaration& action : actions.Actions())
        {
            const StringView id = action.id.AsView();
            const bool has = actions.HasOverride(id);
            ShortcutOverrideEntry* entry = nullptr;
            for (ShortcutOverrideEntry& e : out.overrides)
            {
                if (e.id.AsView() == id)
                {
                    entry = &e;
                    break;
                }
            }
            if (!has)
            {
                if (entry != nullptr)
                {
                    const usize index = static_cast<usize>(entry - out.overrides.Data());
                    out.overrides.RemoveAt(index); // reset to the default: the entry goes
                }
                continue;
            }
            const EditorShortcut chord = actions.Shortcut(id);
            if (entry == nullptr)
            {
                ShortcutOverrideEntry fresh;
                fresh.id = String(id);
                out.overrides.PushBack(Move(fresh));
                entry = &out.overrides[out.overrides.Size() - 1];
            }
            entry->keys = chord.Keys();
        }
        return out.overrides.Size();
    }

    /// Applies the section's overrides to the registry, entry by entry through Rebind. An unknown
    /// id (a domain not loaded) is skipped silently, and a collision is logged and skipped (the
    /// other action keeps the chord). Returns how many were bound.
    inline usize ApplyShortcutOverrides(const EditorShortcutSettings& in, EditorActionRegistry& actions)
    {
        usize applied = 0;
        for (const ShortcutOverrideEntry& e : in.overrides)
        {
            const EditorActionDeclaration* holder = nullptr;
            const Status bound = actions.Rebind(e.id.AsView(), e.Chord(), &holder);
            if (bound.IsOk())
            {
                ++applied;
            }
            else if (bound.Code() == ErrorCode::AlreadyExists && holder != nullptr)
            {
                LOG_WARNING(u8"Editor", u8"shortcut override for '{}' skipped: '{}' holds {}",
                            e.id.AsView(), holder->id.AsView(), FormatShortcut(e.Chord()).AsView());
            }
        }
        return applied;
    }

    /// The Preferences page's staged shortcut edits (a chord chosen per action, or a reset to the
    /// default), applied together on Save. Applying first frees every staged action's chord,
    /// so two actions can swap chords in one save; a chord another (unstaged) action holds is a
    /// collision: that action keeps it, the staged one goes back to what it had, and the
    /// collision is reported ("Ctrl+S: 'Save' holds it"). The registry's overrides are then
    /// captured into the section for the store.
    class ShortcutEdits
    {
    public:
        struct Entry
        {
            String id;
            bool reset = false;
            EditorShortcut chord;
        };

        void Set(StringView id, EditorShortcut chord)
        {
            Entry& e = Stage(id);
            e.reset = false;
            e.chord = chord;
        }
        void Reset(StringView id)
        {
            Entry& e = Stage(id);
            e.reset = true;
            e.chord = EditorShortcut{};
        }
        void Discard(StringView id)
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (m_entries[i].id.AsView() == id)
                {
                    m_entries.RemoveAt(i);
                    return;
                }
            }
        }
        [[nodiscard]] const Entry* Pending(StringView id) const
        {
            for (const Entry& e : m_entries)
            {
                if (e.id.AsView() == id)
                {
                    return &e;
                }
            }
            return nullptr;
        }
        [[nodiscard]] usize Count() const noexcept { return m_entries.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_entries.IsEmpty(); }

        /// Returns how many edits took effect; the collisions, one line each, in `collisions`.
        /// The edits are consumed.
        usize Apply(EditorActionRegistry& actions, EditorShortcutSettings& section, Array<String>* collisions)
        {
            // What each staged action had, to give back on a collision.
            struct Previous
            {
                bool hadOverride = false;
                EditorShortcut chord;
            };
            Array<Previous> previous;
            for (const Entry& e : m_entries)
            {
                Previous p;
                p.hadOverride = actions.HasOverride(e.id.AsView());
                p.chord = actions.Shortcut(e.id.AsView());
                previous.PushBack(p);
                (void)actions.Rebind(e.id.AsView(), EditorShortcut{}); // freed, so swaps work
            }
            usize applied = 0;
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                const Entry& e = m_entries[i];
                const StringView id = e.id.AsView();
                if (actions.Find(id) == nullptr)
                {
                    continue;
                }
                if (e.reset)
                {
                    actions.ResetShortcut(id);
                    ++applied;
                    continue;
                }
                const EditorActionDeclaration* holder = nullptr;
                const Status bound = actions.Rebind(id, e.chord, &holder);
                if (bound.IsOk())
                {
                    ++applied;
                    continue;
                }
                // Back to what it had.
                if (previous[i].hadOverride)
                {
                    (void)actions.Rebind(id, previous[i].chord);
                }
                else
                {
                    actions.ResetShortcut(id);
                }
                if (collisions != nullptr && holder != nullptr)
                {
                    collisions->PushBack(Format(u8"{}: '{}' holds it", FormatShortcut(e.chord).AsView(),
                                                holder->label.AsView()));
                }
            }
            m_entries.Clear();
            (void)CaptureShortcutOverrides(actions, section);
            return applied;
        }

    private:
        Entry& Stage(StringView id)
        {
            for (Entry& e : m_entries)
            {
                if (e.id.AsView() == id)
                {
                    return e;
                }
            }
            Entry fresh;
            fresh.id = String(id);
            m_entries.PushBack(Move(fresh));
            return m_entries[m_entries.Size() - 1];
        }

        Array<Entry> m_entries;
    };

    // Register the editor's Settings section types so a Settings store can instantiate them on Load.
    // Call once at editor startup, before LoadEditorSettings.
    inline void RegisterEditorSettingsTypes()
    {
        // Every section type needs both registrations: the type (so Load can match the stored
        // name) and the serializable factory (so Load can instantiate it). Without the factory,
        // the first file saved with that section makes every later Load abort mid-file, silently
        // dropping the sections after it (an empty recent projects list, for one).
        GlobalTypeRegistry().Register(EditorExportSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorExportSettings>();
        GlobalTypeRegistry().Register(EditorUiSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorUiSettings>();
        GlobalTypeRegistry().Register(EditorMcpSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorMcpSettings>();
        GlobalTypeRegistry().Register(EditorShortcutSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorShortcutSettings>();
        GlobalTypeRegistry().Register(GamePreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<GamePreviewSettings>();
        GlobalTypeRegistry().Register(GamePageSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<GamePageSettings>();
    }

    // Load the editor settings store from `root` (XML). NotFound when the file is absent (on a first
    // run the store stays empty and every section reads as its defaults). Types must be registered
    // first.
    [[nodiscard]] inline Status LoadEditorSettings(vfs::IFileSystem& root, settings::Settings& out,
                                                   StringView fileName = kEditorSettingsFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        return out.Load(*stream, foundation::xml::XmlSerializerFactory());
    }

    // Persist the editor settings store to `root` (XML).
    [[nodiscard]] inline Status SaveEditorSettings(vfs::IWritableFileSystem& root,
                                                   const settings::Settings& in,
                                                   StringView fileName = kEditorSettingsFile)
    {
        MemoryStream buffer;
        if (Status s = in.Save(buffer, foundation::xml::XmlSerializerFactory()); !s.IsOk())
        {
            return s;
        }
        return root.Save(fileName, buffer.Bytes());
    }

    // Load/save the editor settings at their canonical location (<user-data>/editor.settings.xml).
    // The editor uses these; tests use the forms above that take a file system.
    [[nodiscard]] inline Status LoadEditorSettingsFromUserData(settings::Settings& out)
    {
        vfs::NativeFileSystem fs(GetUserDataDirectory().AsView(), foundation::core::DefaultAllocator());
        return LoadEditorSettings(fs, out);
    }
    [[nodiscard]] inline Status SaveEditorSettingsToUserData(const settings::Settings& in)
    {
        const String dir = GetUserDataDirectory();
        (void)CreateDirectory(dir.AsView()); // ensure the leaf dir exists before writing
        vfs::NativeFileSystem fs(dir.AsView(), foundation::core::DefaultAllocator());
        return SaveEditorSettings(*fs.AsWritable(), in);
    }

    RTTI_DEFINE_OBJECT_VERSIONED(EditorUiSettings, "rtti::editor::editor", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorMcpSettings, "rtti::editor::editor", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorShortcutSettings, "rtti::editor::editor", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(GamePreviewSettings, "rtti::editor::editor", 1)
    RTTI_DEFINE_OBJECT_VERSIONED(GamePageSettings, "rtti::editor::editor", 1)
}
