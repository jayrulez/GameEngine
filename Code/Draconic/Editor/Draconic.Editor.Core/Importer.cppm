// Draconic::EditorCore - :importer partition.
//
// The file-import seam (asset-pipeline design §7): an OS file (drag-dropped onto the editor)
// becomes a SOURCE - the raw bytes copied into the project's Sources/ tree - plus a typed Asset
// instance in the content DB whose import settings point at it. Cooking then owns the
// source -> product path like any other asset (the imported file's content is part of the
// recipe hash).
//
// IFileImporter implementations live with their asset modules (texture/image/...) and are
// registered by the executable; routing is by lowercase extension. Several importers may claim
// one extension - v1 takes the FIRST match (the Sedulous-style chooser dialog is a later
// nicety; the registry API already exposes all matches).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.core:importer;

import draconic.foundation;
import draconic.content;
import :project;

using namespace draconic::foundation;

export namespace draconic::editor
{
    /// Importer-specific options, shown by the import dialog before the import runs. The
    /// dialog renders one checkbox per Toggle (each points into the options object) - a
    /// declarative description, no reflection required. Subclasses add their fields and
    /// return the toggle list; the base is intentionally empty (no options = no dialog).
    class ImportOptions : public ISerializable
    {
        DRACONIC_OBJECT(ImportOptions, ISerializable)
    public:
        struct Toggle
        {
            StringView label;       // checkbox text ("Generate prefab")
            StringView description; // tooltip (empty = none)
            bool* value = nullptr;  // points into the options object
        };

        [[nodiscard]] virtual Array<Toggle> Toggles() { return {}; }
        void Serialize(ISerializer&) override {}
    };

    /// A BULK write an importer defers to the worker flush. Three shapes, one struct:
    ///  - data-stream write: `instance` + `streamName` (+ view/owned bytes)
    ///  - envelope write:    `instance` + `object` (SERIALIZATION runs on the worker too -
    ///    a big mesh source rendered to XML is the single most expensive part of an import)
    ///  - raw file copy:     `copyFrom` -> `copyTo` (source + sidecar provenance copies;
    ///    identical bytes skip so mtimes don't churn recooks)
    /// All three are pure file/mount IO with NO in-memory DB mutation, so they are safe off
    /// the UI thread while the job lock excludes cooks and queues deletes. `view` borrows
    /// from the importer's prepared payload (kept alive through the flush).
    struct DeferredImportWrite
    {
        draconic::content::Instance* instance = nullptr; // borrowed; the DB owns it
        RefPtr<ISerializable> object;                    // envelope write when set
        String streamName;                               // data-stream write when set
        Span<const byte> view{};
        Array<byte> owned;
        String copyFrom; // raw copy when both paths set
        String copyTo;

        [[nodiscard]] Span<const byte> Bytes() const noexcept
        {
            return owned.IsEmpty() ? view : Span<const byte>{owned.Data(), owned.Size()};
        }

        /// Execute on the worker. Returns the write's status.
        [[nodiscard]] Status Execute() const
        {
            if (!copyFrom.IsEmpty() && !copyTo.IsEmpty())
            {
                Result<Array<byte>> bytes = ReadFile(copyFrom.AsView());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                if (FileExists(copyTo.AsView()))
                {
                    Result<Array<byte>> existing = ReadFile(copyTo.AsView());
                    if (existing.HasValue() && existing.Value().Size() == bytes.Value().Size())
                    {
                        bool same = true;
                        for (usize i = 0; i < bytes.Value().Size(); ++i)
                        {
                            if (existing.Value()[i] != bytes.Value()[i])
                            {
                                same = false;
                                break;
                            }
                        }
                        if (same)
                        {
                            return Status{};
                        }
                    }
                }
                return WriteFile(copyTo.AsView(),
                                 Span<const byte>(bytes.Value().Data(), bytes.Value().Size()));
            }
            if (instance == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (object.Get() != nullptr)
            {
                return instance->WriteObject(*object);
            }
            return instance->WriteData(streamName.AsView(), Bytes());
        }

        [[nodiscard]] StringView Label() const noexcept
        {
            if (!copyTo.IsEmpty())
            {
                return copyTo.AsView();
            }
            return (instance != nullptr) ? instance->Name() : StringView(u8"?");
        }
    };

    class IFileImporter
    {
    public:
        virtual ~IFileImporter() = default;

        /// Shown in menus/choosers ("Texture", "Model", ...).
        [[nodiscard]] virtual StringView Label() const = 0;

        /// Does this importer claim the extension (lowercase, no dot: "png")?
        [[nodiscard]] virtual bool Accepts(StringView extension) const = 0;

        /// Fresh options for one import (defaults set). Null = this importer has no options
        /// and the import runs immediately on drop, no dialog.
        [[nodiscard]] virtual RefPtr<ImportOptions> CreateOptions() const { return {}; }

        /// Slow importers split in two: PrepareOnWorker runs OFF the UI thread (pure
        /// parse/decode of the source file - NO project or DB access) and its payload is
        /// then handed to Import on the MAIN thread for the fast DB fan-out. Default: no
        /// worker phase (Import does everything inline).
        [[nodiscard]] virtual bool WantsWorkerPrepare() const { return false; }
        [[nodiscard]] virtual RefPtr<Object> PrepareOnWorker(StringView /*sourcePath*/);

        /// Import `sourcePath` (absolute OS path): copy the source under Sources/ and create
        /// the typed Asset instance(s) in `group`. Returns the primary created instance.
        /// `options` is the object CreateOptions() returned after the user edited it in the
        /// dialog (null when the importer has none or the import runs headless). `prepared`
        /// is PrepareOnWorker's payload when the two-phase path ran (null = load inline).
        /// `deferredWrites`: when non-null, the importer MAY park its bulk writes there
        /// instead of writing inline - the caller flushes them on a worker (null =
        /// headless/tests: everything writes inline).
        [[nodiscard]] virtual Result<draconic::content::Instance*>
        Import(StringView sourcePath, EditorProject& project, draconic::content::Group& group,
               const ImportOptions* options = nullptr, Object* prepared = nullptr,
               Array<DeferredImportWrite>* deferredWrites = nullptr) = 0;
    };

    DRACONIC_DEFINE_OBJECT(ImportOptions, "draconic::editor")

    class ImporterRegistry
    {
    public:
        void Register(UniquePtr<IFileImporter> importer);

        /// First importer claiming the extension (v1 routing), or null.
        [[nodiscard]] IFileImporter* FindFor(StringView extension) const;

        [[nodiscard]] usize Count() const noexcept { return m_importers.Size(); }

    private:
        Array<UniquePtr<IFileImporter>> m_importers;
    };

    // === shared import helpers ===

    /// Lowercased extension of a path, without the dot ("/a/b/Foo.PNG" -> "png").
    [[nodiscard]] inline String FileExtensionLower(StringView path)
    {
        usize dot = path.Size();
        for (usize i = path.Size(); i > 0; --i)
        {
            const utf8char c = path[i - 1];
            if (c == utf8char('.'))
            {
                dot = i;
                break;
            }
            if (c == utf8char('/') || c == utf8char('\\'))
            {
                break;
            }
        }
        String ext;
        for (usize i = dot; i < path.Size(); ++i)
        {
            const utf8char c = path[i];
            ext.PushBack((c >= utf8char('A') && c <= utf8char('Z')) ? static_cast<utf8char>(c + 32)
                                                                    : c);
        }
        return ext;
    }

    /// Final path component ("/a/b/foo.png" -> "foo.png").
    [[nodiscard]] inline StringView FileNameOf(StringView path)
    {
        for (usize i = path.Size(); i > 0; --i)
        {
            const utf8char c = path[i - 1];
            if (c == utf8char('/') || c == utf8char('\\'))
            {
                return path.SubStr(i, path.Size() - i);
            }
        }
        return path;
    }

    /// The stem, for instance naming ("foo.png" -> "foo").
    [[nodiscard]] inline StringView FileStemOf(StringView fileName)
    {
        for (usize i = fileName.Size(); i > 0; --i)
        {
            if (fileName[i - 1] == utf8char('.'))
            {
                return fileName.SubStr(0, i - 1);
            }
        }
        return fileName;
    }

    /// Copy an OS file into the project's Sources/ tree. Returns the sources-relative name the
    /// Asset should reference. An existing SAME-CONTENT file is reused untouched; changed
    /// bytes OVERWRITE it (a re-import must see the edited file - the old skip-if-exists
    /// behavior silently kept stale sources). The copy goes through the core file API and the
    /// project path only - the pipeline reads it back through the sources MOUNT.
    [[nodiscard]] inline Result<String> CopyIntoSources(EditorProject& project,
                                                        StringView sourcePath)
    {
        const StringView fileName = FileNameOf(sourcePath);
        if (fileName.IsEmpty())
        {
            return Err(ErrorCode::InvalidArgument);
        }
        const String target = PathJoin(project.SourcesRoot().AsView(), fileName);

        Result<Array<byte>> bytes = ReadFile(sourcePath);
        if (!bytes.HasValue())
        {
            return Err(bytes.Error());
        }
        if (FileExists(target.AsView()))
        {
            Result<Array<byte>> existing = ReadFile(target.AsView());
            if (existing.HasValue() && existing.Value().Size() == bytes.Value().Size())
            {
                bool same = true;
                for (usize i = 0; i < bytes.Value().Size(); ++i)
                {
                    if (existing.Value()[i] != bytes.Value()[i])
                    {
                        same = false;
                        break;
                    }
                }
                if (same)
                {
                    return String(fileName);
                } // identical: no touch, no recook churn
            }
        }
        const Status written = WriteFile(
            target.AsView(), Span<const byte>(bytes.Value().Data(), bytes.Value().Size()));
        if (!written.IsOk())
        {
            return Err(written.Code());
        }
        return String(fileName);
    }
}
