// The editor's USER-LEVEL settings store (<user-data>/editor.settings.xml): the
// cross-project preference sections (fonts, UI scale), the file plumbing, and the
// one registration entry point for every section type.
//
// Section types may live where their DOMAIN lives (EditorExportSettings stays with
// the export code, RecentProjectsSettings with the project registry) - but they are
// all REGISTERED here, in one place, because a section registered without its
// serializable factory makes Settings::Load abort the whole store at that section
// (phase-1 semantics) and silently drop everything after it. One list, both calls,
// every type - see RegisterEditorSettingsTypes.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.core:editor_settings;

import draconic.foundation;
import draconic.vfs;
import draconic.xml.serialization;
import draconic.settings;
import :export_preset; // EditorExportSettings (registered below)

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace settings = draconic::settings;
    namespace vfs = draconic::vfs;

    // Editor-level FONT preferences (a Settings section): explicit .ttf paths for the UI
    // and mono families. Empty (the default) = the built-in resolution chain (the dev-tree
    // compile define, then the exe-embedded fallback face).
    class EditorFontSettings final : public ISerializable
    {
        DRACONIC_OBJECT(EditorFontSettings, ISerializable)
    public:
        String fontPath;     // UI family override ("" = built-in chain)
        String monoFontPath; // mono family override ("" = built-in chain)

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "fontPath", fontPath);
            draconic::foundation::Serialize(ar, "monoFontPath", monoFontPath);
        }
    };

    // Editor-level UI preferences (a Settings section). uiScale multiplies the window's
    // OS content scale for the whole editor UI (layout + fonts + baked icons) - both an
    // accessibility knob and the way to exercise the DPI path without a scaled monitor.
    class EditorUiSettings final : public ISerializable
    {
        DRACONIC_OBJECT(EditorUiSettings, ISerializable)
    public:
        f32 uiScale = 1.0f; // clamped to [1, 2] on use

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "uiScale", uiScale);
        }
    };

    // The editor's settings file, in the user-data dir (hand-editable XML, like the project files).
    inline constexpr StringView kEditorSettingsFile = u8"editor.settings.xml";

    // Register the editor's Settings section types so a Settings store can instantiate them on Load.
    // Call once at editor startup, before LoadEditorSettings.
    inline void RegisterEditorSettingsTypes()
    {
        // EVERY section type needs BOTH registrations: the type (so Load can match the
        // stored name) AND the serializable factory (so Load can instantiate it). A type
        // registered without its factory is a time bomb: the first file SAVED with that
        // section makes every later Load abort mid-file (Settings phase-1 semantics),
        // silently dropping the sections after it - the empty-project-list incident.
        GlobalTypeRegistry().Register(EditorExportSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorExportSettings>();
        GlobalTypeRegistry().Register(EditorFontSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorFontSettings>();
        GlobalTypeRegistry().Register(EditorUiSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<EditorUiSettings>();
    }

    // Load the editor settings store from `root` (XML). NotFound when the file is absent (first run =>
    // the store stays empty and every section reads as its defaults). Types must be registered first.
    [[nodiscard]] inline Status LoadEditorSettings(vfs::IFileSystem& root, settings::Settings& out,
                                                   StringView fileName = kEditorSettingsFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        return out.Load(*stream, draconic::xml::XmlSerializerFactory());
    }

    // Persist the editor settings store to `root` (XML).
    [[nodiscard]] inline Status SaveEditorSettings(vfs::IWritableFileSystem& root,
                                                   const settings::Settings& in,
                                                   StringView fileName = kEditorSettingsFile)
    {
        MemoryStream buffer;
        if (Status s = in.Save(buffer, draconic::xml::XmlSerializerFactory()); !s.IsOk())
        {
            return s;
        }
        return root.Save(fileName, buffer.Bytes());
    }

    // Load/save the editor settings at their canonical location (<user-data>/editor.settings.xml).
    // The editor uses these; tests use the fs-explicit forms above.
    [[nodiscard]] inline Status LoadEditorSettingsFromUserData(settings::Settings& out)
    {
        vfs::NativeFileSystem fs(GetUserDataDirectory(u8"draconic").AsView());
        return LoadEditorSettings(fs, out);
    }
    [[nodiscard]] inline Status SaveEditorSettingsToUserData(const settings::Settings& in)
    {
        const String dir = GetUserDataDirectory(u8"draconic");
        (void)CreateDirectory(dir.AsView()); // ensure the leaf dir exists before writing
        vfs::NativeFileSystem fs(dir.AsView());
        return SaveEditorSettings(*fs.AsWritable(), in);
    }

    DRACONIC_DEFINE_OBJECT_VERSIONED(EditorFontSettings, "draconic::editor", 1)
    DRACONIC_DEFINE_OBJECT_VERSIONED(EditorUiSettings, "draconic::editor", 1)
}
