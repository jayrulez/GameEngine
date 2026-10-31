// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Importer - the `pipeline.importer` module.
//
// File import: an OS file (drag-dropped onto the editor) becomes a source, its raw bytes copied
// into the project's Sources/ tree, and a typed Asset instance in the content DB whose import
// settings point at it. Cooking then takes the source to its product like any other asset (the
// imported file's content is part of the recipe hash).
//
// IFileImporter implementations live with their asset modules (texture, image, ...) and are
// registered by the executable; routing is by lowercase extension. Several importers may claim
// one extension: FindFor returns the first match, and FindAllFor returns them all so the editor
// can offer a chooser.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module pipeline.importer;

import foundation.core;
import foundation.content;
import pipeline.core; // Asset (the typed fileName back-reference re-import memory matches on)

using namespace foundation::core;

export namespace pipeline
{
    /// What an import does when the name it would take is held by an asset of its own type that
    /// another file made (an asset of another type is always refused; a re-import of the same file
    /// replaces its asset without asking). The editor asks the user and passes the answer; a
    /// headless caller (MCP, a script) refuses unless it says.
    enum class ImportConflictChoice : u8
    {
        Refuse,   // fail, naming the asset there (the default)
        Replace,  // the asset there takes the new file: its guid, and so what refers to it, kept
        KeepBoth, // the new asset under the next free name ("Menu.2")
    };

    struct ImportContext
    {
        // Everything an import needs to know about where it lands, rather than the editor's
        // project object: the pipeline is headless-drivable (CLI, MCP, tests), so the caller
        // supplies the facts imports consume and keeps its project model to itself. The
        // allocator is required; the editor page or cook tool decides.
        ImportContext(IAllocator& alloc, StringView root) : allocator(&alloc), sourcesRoot(root) {}

        IAllocator* allocator;
        String sourcesRoot; // absolute OS path to the project's Sources/ tree
        ImportConflictChoice onConflict = ImportConflictChoice::Refuse;
        // The import ledger (ImportLedgerPath: under the project's ignored .cache/): which OS
        // file each file under Sources/ came from, so a re-import of the same file is told from
        // another file of the same name. Empty: none kept (the file name alone decides).
        String ledgerPath;
    };

    /// What kind of asset an import-plan entry would create (the review dialog's grouping).
    enum class ImportResourceKind : u8
    {
        Texture,
        Material,
        Mesh,
        Skeleton,
        AnimationClip,
        Collision,
        Asset, // a single-asset importer's one product (a texture, image, audio, font, ... file)
    };

    [[nodiscard]] inline StringView ImportResourceKindLabel(ImportResourceKind kind) noexcept
    {
        switch (kind)
        {
        case ImportResourceKind::Texture: return u8"Textures";
        case ImportResourceKind::Material: return u8"Materials";
        case ImportResourceKind::Mesh: return u8"Meshes";
        case ImportResourceKind::Skeleton: return u8"Skeleton";
        case ImportResourceKind::AnimationClip: return u8"Animation Clips";
        case ImportResourceKind::Collision: return u8"Collision";
        case ImportResourceKind::Asset: return u8"Asset";
        }
        return u8"Resources";
    }

    // (SingleAssetPlan, the shared single-asset DescribeImport, is defined below the path
    // helpers it uses.)

    /// One resource an import would create. `sourceName` is the importer's deterministic base
    /// name for the resource (the stable key the commit matches on); `targetName` is what the
    /// user wants it called (defaults to sourceName); unchecked entries are skipped.
    struct ImportPlanEntry
    {
        ImportResourceKind kind = ImportResourceKind::Texture;
        String sourceName;
        String targetName;
        bool enabled = true;
    };

    /// DescribeImport's result: everything the import would create, in fan-out order.
    /// Empty means the importer does not support per-resource review (toggles only).
    struct ImportPlan
    {
        Array<ImportPlanEntry> entries;
        [[nodiscard]] bool IsEmpty() const noexcept { return entries.IsEmpty(); }
        [[nodiscard]] const ImportPlanEntry* Find(ImportResourceKind kind,
                                                 StringView sourceName) const
        {
            for (const ImportPlanEntry& e : entries)
            {
                if (e.kind == kind && e.sourceName.AsView() == sourceName)
                {
                    return &e;
                }
            }
            return nullptr;
        }
    };

    inline void Serialize(ISerializer& ar, ImportPlanEntry& e)
    {
        u8 kind = static_cast<u8>(e.kind);
        foundation::core::Serialize(ar, "kind", kind);
        e.kind = static_cast<ImportResourceKind>(kind);
        foundation::core::Serialize(ar, "sourceName", e.sourceName);
        foundation::core::Serialize(ar, "targetName", e.targetName);
        foundation::core::Serialize(ar, "enabled", e.enabled);
    }

    inline void Serialize(ISerializer& ar, ImportPlan& p)
    {
        foundation::core::Serialize(ar, "entries", p.entries);
    }

    /// Re-import memory: overlay the stored decisions of a previous import onto a freshly
    /// described plan. Matched entries (kind and sourceName) take the stored enabled state and
    /// rename; resources new in the source stay at their described defaults.
    inline void MergeStoredSelection(ImportPlan& plan, const ImportPlan& stored)
    {
        for (ImportPlanEntry& e : plan.entries)
        {
            if (const ImportPlanEntry* s = stored.Find(e.kind, e.sourceName.AsView()))
            {
                e.enabled = s->enabled;
                if (!s->targetName.IsEmpty())
                {
                    e.targetName = s->targetName;
                }
            }
        }
    }

    /// Importer-specific options, shown by the import dialog before the import runs. The
    /// dialog renders one checkbox per Toggle (each points into the options object), a
    /// declarative description with no reflection required. Subclasses add their fields and
    /// return the toggle list; the base is empty (no options, no dialog).
    class ImportOptions : public ISerializable
    {
        RTTI_OBJECT(ImportOptions, ISerializable)
    public:
        struct Toggle
        {
            StringView label;       // checkbox text ("Generate prefab")
            StringView description; // tooltip (empty = none)
            bool* value = nullptr;  // points into the options object
        };

        [[nodiscard]] virtual Array<Toggle> Toggles() { return {}; }
        void Serialize(ISerializer&) override {}

        /// The review dialog's per-resource decisions (edited copy of DescribeImport's plan).
        /// Empty imports everything under default names. Importers that support review
        /// consult it through SelectionEnabled and SelectionName at each creation site; others
        /// ignore it.
        ImportPlan selection;

        [[nodiscard]] bool SelectionEnabled(ImportResourceKind kind, StringView sourceName) const
        {
            const ImportPlanEntry* e = selection.Find(kind, sourceName);
            return e == nullptr || e->enabled;
        }
        /// The user's target name for the resource (falls back to the importer's base name).
        [[nodiscard]] StringView SelectionName(ImportResourceKind kind, StringView sourceName) const
        {
            const ImportPlanEntry* e = selection.Find(kind, sourceName);
            return (e != nullptr && !e->targetName.IsEmpty()) ? e->targetName.AsView()
                                                              : sourceName;
        }
    };

    /// A bulk write an importer defers to the worker flush. Three shapes, one struct:
    ///  - data-stream write: `instance` and `streamName` (and view or owned bytes)
    ///  - envelope write:    `instance` and `object` (serialization runs on the worker too;
    ///    a big mesh source rendered to XML is the single most expensive part of an import)
    ///  - raw file copy:     `copyFrom` to `copyTo` (source and sidecar provenance copies;
    ///    identical bytes are skipped so mtimes don't churn recooks)
    /// All three are pure file and mount IO with no in-memory DB mutation, so they are safe off
    /// the UI thread while the job lock excludes cooks and queues deletes. `view` borrows
    /// from the importer's prepared payload (kept alive through the flush).
    struct DeferredImportWrite
    {
        foundation::content::Instance* instance = nullptr; // borrowed; the DB owns it
        RefPtr<ISerializable> object;                    // envelope write when set
        String streamName;                               // data-stream write when set
        Span<const byte> view{};
        Array<byte> owned;
        String copyFrom; // raw copy when both paths set
        String copyTo;
        // A lazy payload: runs on the worker right before the write and fills `owned` (a
        // mesh's conversion, LOD chain and geometry serialization, the import's CPU bulk).
        // Import queues the write in microseconds; the work happens off the main thread.
        // Whatever it captures must outlive the flush (the editor keeps the prepared model
        // alive for this reason).
        Function<Status(Array<byte>&)> produce;

        [[nodiscard]] Span<const byte> Bytes() const noexcept
        {
            return owned.IsEmpty() ? view : Span<const byte>{owned.Data(), owned.Size()};
        }

        /// Execute on the worker. Returns the write's status.
        [[nodiscard]] Status Execute()
        {
            if (produce)
            {
                const Status produced = produce(owned);
                if (!produced.IsOk())
                {
                    return produced;
                }
                produce = {};
            }
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
                // Nested destinations (a gltf referencing "textures/x.jpg" copies to
                // Sources/textures/): create the parents first, since raw WriteFile does not,
                // unlike the inline path's NativeFileSystem::Save (without them every nested
                // copy fails and the import never finishes).
                for (usize i = copyTo.Size(); i > 0; --i)
                {
                    if (copyTo[i - 1] == utf8char('/'))
                    {
                        (void)CreateDirectories(copyTo.AsView().SubStr(0, i - 1));
                        break;
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

    struct ImportClash; // below, with the clash rule

    class IFileImporter
    {
    public:
        virtual ~IFileImporter() = default;

        /// Shown in menus and choosers ("Texture", "Model", ...).
        [[nodiscard]] virtual StringView Label() const = 0;

        /// Does this importer claim the extension (lowercase, no dot: "png")?
        [[nodiscard]] virtual bool Accepts(StringView extension) const = 0;

        /// Fresh options for one import (defaults set). Null means this importer has no options
        /// and the import runs immediately on drop, no dialog.
        [[nodiscard]] virtual RefPtr<ImportOptions> CreateOptions(IAllocator&) const
        {
            return {};
        }

        /// Slow importers split in two: PrepareOnWorker runs off the UI thread (pure
        /// parsing and decoding of the source file, with no project or DB access) and its payload
        /// is then handed to Import on the main thread for the fast DB fan-out. Default: no
        /// worker phase (Import does everything inline).
        [[nodiscard]] virtual bool WantsWorkerPrepare() const { return false; }
        [[nodiscard]] virtual RefPtr<Object> PrepareOnWorker(StringView /*sourcePath*/,
                                                             IAllocator& allocator);

        /// Enumerate everything Import would create (the review dialog's data), without
        /// touching the project or DB. `prepared` is PrepareOnWorker's payload when the
        /// two-phase path ran (a supporting importer should reuse it, not re-parse).
        /// Default: an empty plan, so no per-resource review; the dialog shows toggles only.
        [[nodiscard]] virtual ImportPlan DescribeImport(StringView /*sourcePath*/,
                                                        const ImportOptions* /*options*/,
                                                        Object* /*prepared*/)
        {
            return {};
        }

        /// The asset type a single-asset importer makes of `sourcePath` ("ImageAsset"), for the
        /// name-clash check before an import; empty for an importer that makes several (a
        /// model's), which keeps its own names apart.
        [[nodiscard]] virtual StringView AssetTypeName(StringView /*sourcePath*/) const { return {}; }

        /// The clash an import of `sourcePath` would meet in `group` under the names `plan` gives
        /// (ImportClashKind): what the editor's window shows on the file's row, and what an agent's
        /// import is refused with. Default: a single asset (AssetTypeName) under its Asset entry's
        /// target, else the file's stem; none when AssetTypeName is empty. An importer that makes
        /// several says what it claims (a model, its folder).
        [[nodiscard]] virtual ImportClash FindClash(foundation::content::Group& group, StringView sourcePath, const ImportPlan& plan,
                                                           StringView ledgerPath) const;

        /// Re-import memory: the selection a previous import of this source stored in the
        /// target group (empty when there is none or it is unsupported). The editor merges it onto
        /// the fresh plan (MergeStoredSelection) so re-importing does not re-ask settled decisions.
        [[nodiscard]] virtual ImportPlan StoredSelection(foundation::content::Group& /*group*/,
                                                        StringView /*sourcePath*/)
        {
            return {};
        }

        /// Import `sourcePath` (absolute OS path): copy the source under Sources/ and create
        /// the typed Asset instance(s) in `group`. Returns the primary created instance.
        /// `options` is the object CreateOptions() returned after the user edited it in the
        /// dialog (null when the importer has none or the import runs headless). `prepared`
        /// is PrepareOnWorker's payload when the two-phase path ran (null loads inline).
        /// `deferredWrites`: when non-null, the importer may park its bulk writes there
        /// instead of writing inline, and the caller flushes them on a worker (null in
        /// headless runs and tests: everything writes inline).
        [[nodiscard]] virtual Result<foundation::content::Instance*>
        Import(StringView sourcePath, const ImportContext& context,
               foundation::content::Group& group, const ImportOptions* options = nullptr,
               Object* prepared = nullptr,
               Array<DeferredImportWrite>* deferredWrites = nullptr) = 0;
    };

    RTTI_DEFINE_OBJECT(ImportOptions, "rtti::pipeline::importer")

    class ImporterRegistry
    {
    public:
        // The allocator (required; the owner decides) backs registered importers.
        explicit ImporterRegistry(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        void Register(UniquePtr<IFileImporter> importer);

        /// First importer claiming the extension, or null.
        [[nodiscard]] IFileImporter* FindFor(StringView extension) const;

        /// Every importer claiming the extension, in registration order (empty when none). The editor
        /// offers a chooser when more than one matches (image or texture on the same extension).
        [[nodiscard]] Array<IFileImporter*> FindAllFor(StringView extension) const;

        [[nodiscard]] usize Count() const noexcept { return m_importers.Size(); }

    private:
        IAllocator* m_allocator;
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

    /// The plan of a single-asset importer: one Asset entry named after the file stem, the
    /// shared DescribeImport for texture, image, audio, font, heightfield and splatmap.
    [[nodiscard]] inline ImportPlan SingleAssetPlan(StringView sourcePath)
    {
        ImportPlan plan;
        ImportPlanEntry e;
        e.kind = ImportResourceKind::Asset;
        e.sourceName = String(FileStemOf(FileNameOf(sourcePath)));
        e.targetName = e.sourceName;
        plan.entries.PushBack(Move(e));
        return plan;
    }

    /// The instance name a single-asset importer should claim: the user's rename when a
    /// selection carries one, else the stem.
    [[nodiscard]] inline StringView SingleAssetName(const ImportOptions* options, StringView stem)
    {
        return (options != nullptr) ? options->SelectionName(ImportResourceKind::Asset, stem)
                                    : stem;
    }

    /// Re-import memory for single-asset importers: find the asset a previous import of this
    /// source created in `group` (matched by the typed fileName back-reference, so a renamed
    /// asset stays findable) and return its current name as the stored rename. `typeName`
    /// filters candidates before the envelope read (the read is the expensive part).
    [[nodiscard]] inline ImportPlan SingleAssetStoredSelection(foundation::content::Group& group,
                                                              StringView sourcePath,
                                                              StringView typeName)
    {
        const StringView fileName = FileNameOf(sourcePath);
        if (fileName.IsEmpty())
        {
            return {};
        }
        for (foundation::content::Instance* instance : group.Instances())
        {
            if (instance->TypeName() != typeName)
            {
                continue;
            }
            RefPtr<ISerializable> object = instance->ReadObject();
            const auto* asset = Cast<Asset>(object.Get());
            if (asset == nullptr || asset->fileName != fileName)
            {
                continue;
            }
            ImportPlan plan;
            ImportPlanEntry e;
            e.kind = ImportResourceKind::Asset;
            e.sourceName = String(FileStemOf(fileName));
            e.targetName = String(instance->Name());
            plan.entries.PushBack(Move(e));
            return plan;
        }
        return {};
    }

    // === the import ledger ===

    /// Where a project keeps its import ledger: `<cacheRoot>/imports.txt` (the project's .cache/,
    /// ignored by version control: an OS path is the importing machine's, never the project's).
    [[nodiscard]] inline String ImportLedgerPath(StringView cacheRoot) { return PathJoin(cacheRoot, u8"imports.txt"); }

    /// The OS file `sourcesName` (a file under Sources/) was last imported from; empty when the
    /// ledger does not say (no ledger, a file from before it, or one copied in by hand).
    [[nodiscard]] inline String ImportOriginOf(StringView ledgerPath, StringView sourcesName)
    {
        if (ledgerPath.IsEmpty())
        {
            return {};
        }
        Result<Array<byte>> bytes = ReadFile(ledgerPath);
        if (!bytes.HasValue())
        {
            return {};
        }
        const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        String found;
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != utf8char('\n'))
            {
                ++end;
            }
            const StringView line = text.SubStr(start, end - start);
            start = end + 1;
            usize tab = 0;
            while (tab < line.Size() && line[tab] != utf8char('\t'))
            {
                ++tab;
            }
            if (tab < line.Size() && line.SubStr(0, tab) == sourcesName)
            {
                found = String(line.SubStr(tab + 1, line.Size() - tab - 1)); // the last line for it wins
            }
        }
        return found;
    }

    /// Notes that `sourcesName` came from `origin` (its line replaced).
    inline void RecordImportOrigin(StringView ledgerPath, StringView sourcesName, StringView origin)
    {
        if (ledgerPath.IsEmpty())
        {
            return;
        }
        String text;
        if (Result<Array<byte>> bytes = ReadFile(ledgerPath); bytes.HasValue())
        {
            const StringView old(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
            usize start = 0;
            while (start < old.Size())
            {
                usize end = start;
                while (end < old.Size() && old[end] != utf8char('\n'))
                {
                    ++end;
                }
                const StringView line = old.SubStr(start, end - start);
                start = end + 1;
                usize tab = 0;
                while (tab < line.Size() && line[tab] != utf8char('\t'))
                {
                    ++tab;
                }
                if (line.IsEmpty() || (tab < line.Size() && line.SubStr(0, tab) == sourcesName))
                {
                    continue;
                }
                text += line;
                text += u8"\n";
            }
        }
        text += sourcesName;
        text += u8"\t";
        text += origin;
        text += u8"\n";
        for (usize i = ledgerPath.Size(); i > 0; --i)
        {
            if (ledgerPath[i - 1] == utf8char('/') || ledgerPath[i - 1] == utf8char('\\'))
            {
                (void)CreateDirectories(ledgerPath.SubStr(0, i - 1));
                break;
            }
        }
        (void)WriteFile(ledgerPath, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()));
    }

    namespace import_detail
    {
        [[nodiscard]] inline bool SameBytes(const Array<byte>& a, const Array<byte>& b)
        {
            if (a.Size() != b.Size())
            {
                return false;
            }
            for (usize i = 0; i < a.Size(); ++i)
            {
                if (a[i] != b[i])
                {
                    return false;
                }
            }
            return true;
        }
    }

    /// The name under Sources/ a copy of `sourcePath` takes, from what is there and the ledger
    /// alone (no file is read): its own file name when that is free or this file's own copy
    /// (`mayOverwrite`, or the ledger saying it came from `sourcePath`); else, when that name is
    /// another file's (the ledger names another origin, or the caller says it may not overwrite),
    /// the next free one ("a.2.png"). Noted in the ledger as `sourcePath`'s. For a copy made later
    /// (a deferred write); CopyIntoSources copies at once.
    [[nodiscard]] inline String SourcesNameFor(const ImportContext& context, StringView sourcePath, bool mayOverwrite = true,
                                               const Array<byte>* bytes = nullptr)
    {
        const StringView fileName = FileNameOf(sourcePath);
        const StringView stem = FileStemOf(fileName);
        const StringView extension = fileName.SubStr(stem.Size(), fileName.Size() - stem.Size()); // with its dot
        String name(fileName);
        for (u32 n = 2;; ++n)
        {
            const String target = PathJoin(context.sourcesRoot.AsView(), name.AsView());
            if (!FileExists(target.AsView()))
            {
                break;
            }
            const String origin = ImportOriginOf(context.ledgerPath.AsView(), name.AsView());
            if (origin.IsEmpty() ? mayOverwrite : origin.AsView() == sourcePath)
            {
                break; // this file's own copy
            }
            if (bytes != nullptr)
            {
                Result<Array<byte>> existing = ReadFile(target.AsView());
                if (existing.HasValue() && import_detail::SameBytes(existing.Value(), *bytes))
                {
                    break; // the very same bytes: shared, nothing lost
                }
            }
            name = Format(u8"{}.{}{}", stem, n, extension); // another file's: the next name
        }
        RecordImportOrigin(context.ledgerPath.AsView(), name.AsView(), sourcePath);
        return name;
    }

    /// Copy an OS file into the project's Sources/ tree under SourcesNameFor's name. Returns the
    /// sources-relative name the Asset should reference. An existing same-content file is reused
    /// untouched (no recook churn); changed bytes overwrite this file's own copy (a re-import must
    /// see the edited file) and never another file's. The copy goes through the core file API and
    /// the context's sources root only; the pipeline reads it back through the sources mount.
    [[nodiscard]] inline Result<String> CopyIntoSources(const ImportContext& context,
                                                        StringView sourcePath, bool mayOverwrite = true)
    {
        if (FileNameOf(sourcePath).IsEmpty())
        {
            return Err(ErrorCode::InvalidArgument);
        }
        Result<Array<byte>> bytes = ReadFile(sourcePath);
        if (!bytes.HasValue())
        {
            return Err(bytes.Error());
        }
        String name = SourcesNameFor(context, sourcePath, mayOverwrite, &bytes.Value());
        const String target = PathJoin(context.sourcesRoot.AsView(), name.AsView());
        if (FileExists(target.AsView()))
        {
            Result<Array<byte>> existing = ReadFile(target.AsView());
            if (existing.HasValue() && import_detail::SameBytes(existing.Value(), bytes.Value()))
            {
                return name; // identical: no touch, no recook churn
            }
        }
        const Status written = WriteFile(
            target.AsView(), Span<const byte>(bytes.Value().Data(), bytes.Value().Size()));
        if (!written.IsOk())
        {
            return Err(written.Code());
        }
        return name;
    }

    // === name clashes ===

    enum class ImportClashKind : u8
    {
        None,      // the name is free
        Reimport,  // held by this file's own earlier import: it is replaced, as a re-import is
        OtherType, // held by an asset of another type: always refused
        SameType,  // held by an asset of its type from another file: ImportConflictChoice decides
    };

    struct ImportClash
    {
        ImportClashKind kind = ImportClashKind::None;
        String name;         // the name asked for
        String existingType; // what holds it
        foundation::content::Instance* existing = nullptr;
    };

    /// What taking `name` in `group` for a `typeName` asset made from `sourcePath` meets. A
    /// same-type asset whose source is this file's copy under Sources/ (same file name, and the
    /// ledger saying it came from this path, or saying nothing) is this file's earlier import.
    [[nodiscard]] inline ImportClash FindImportClash(foundation::content::Group& group, StringView name, StringView typeName,
                                                     StringView sourcePath, StringView ledgerPath)
    {
        ImportClash clash;
        clash.name = String(name);
        foundation::content::Instance* existing = group.GetInstance(name);
        if (existing == nullptr)
        {
            return clash;
        }
        clash.existing = existing;
        clash.existingType = String(existing->TypeName());
        if (existing->TypeName() != typeName)
        {
            clash.kind = ImportClashKind::OtherType;
            return clash;
        }
        clash.kind = ImportClashKind::SameType;
        RefPtr<ISerializable> object = existing->ReadObject();
        const auto* asset = Cast<Asset>(object.Get());
        if (asset == nullptr)
        {
            return clash;
        }
        // Its file under Sources/: this file's name, or a name the copy took beside another's ("a.2.png").
        const StringView ownName = FileNameOf(sourcePath);
        const StringView held = asset->fileName.View();
        const String origin = ImportOriginOf(ledgerPath, held);
        if (!origin.IsEmpty())
        {
            if (origin.AsView() == sourcePath)
            {
                clash.kind = ImportClashKind::Reimport;
            }
            return clash;
        }
        if (held == ownName)
        {
            clash.kind = ImportClashKind::Reimport; // no ledger entry: the file name decides
        }
        return clash;
    }

    /// The clash said for a person: what holds the name and what to do.
    [[nodiscard]] inline String DescribeImportClash(const ImportClash& clash)
    {
        switch (clash.kind)
        {
        case ImportClashKind::OtherType:
            return Format(u8"'{}' is already a {} here: import it under another name.", clash.name.AsView(), clash.existingType.AsView());
        case ImportClashKind::SameType:
            return Format(u8"'{}' is already here, made from another file: replace it, or keep both.", clash.name.AsView());
        case ImportClashKind::None:
        case ImportClashKind::Reimport:
            break;
        }
        return {};
    }

    /// Whether the import may go on over `clash` with `choice`.
    [[nodiscard]] inline bool ImportClashAllows(const ImportClash& clash, ImportConflictChoice choice)
    {
        return clash.kind == ImportClashKind::None || clash.kind == ImportClashKind::Reimport ||
               (clash.kind == ImportClashKind::SameType && choice != ImportConflictChoice::Refuse);
    }

    /// The asset a single-asset importer writes: its name (the user's, else the stem) claimed in
    /// `group` by the clash rule and the context's choice, and the file copied into Sources/
    /// (never over another file's copy). `sourcesName` gets the copy's name for the asset's
    /// fileName. Refused (AlreadyExists) for an asset of another type, or of its type from another
    /// file when the choice is Refuse; nothing is copied or made then.
    [[nodiscard]] inline Result<foundation::content::Instance*> ClaimSingleAsset(const ImportContext& context, foundation::content::Group& group,
                                                                                 const ImportOptions* options, StringView sourcePath,
                                                                                 const TypeInfo& type, String& sourcesName)
    {
        const StringView stem = FileStemOf(FileNameOf(sourcePath));
        String name(SingleAssetName(options, stem));
        const StringView typeName(reinterpret_cast<const utf8char*>(type.name));
        const ImportClash clash = FindImportClash(group, name.AsView(), typeName, sourcePath, context.ledgerPath.AsView());
        if (!ImportClashAllows(clash, context.onConflict))
        {
            return Err(ErrorCode::AlreadyExists);
        }
        foundation::content::Instance* reuse = nullptr;
        bool overwriteOwn = false; // its copy under Sources/ is the asset's own, to be overwritten
        if (clash.kind == ImportClashKind::Reimport ||
            (clash.kind == ImportClashKind::SameType && context.onConflict == ImportConflictChoice::Replace))
        {
            reuse = clash.existing;
            RefPtr<ISerializable> object = reuse->ReadObject();
            const auto* asset = Cast<Asset>(object.Get());
            overwriteOwn = asset != nullptr && asset->fileName.View() == FileNameOf(sourcePath);
        }
        else if (clash.kind == ImportClashKind::SameType)
        {
            name = group.UniqueInstanceName(name.AsView()); // KeepBoth
        }
        Result<String> copied = CopyIntoSources(context, sourcePath, overwriteOwn);
        if (!copied.HasValue())
        {
            return Err(copied.Error());
        }
        foundation::content::Instance* instance = reuse != nullptr ? reuse : group.CreateInstance(name.AsView(), type);
        if (instance == nullptr)
        {
            return Err(ErrorCode::Unknown);
        }
        sourcesName = Move(copied.Value());
        return instance;
    }

    inline ImportClash IFileImporter::FindClash(foundation::content::Group& group, StringView sourcePath, const ImportPlan& plan,
                                                StringView ledgerPath) const
    {
        const StringView typeName = AssetTypeName(sourcePath);
        if (typeName.IsEmpty())
        {
            return {};
        }
        const StringView stem = FileStemOf(FileNameOf(sourcePath));
        const ImportPlanEntry* entry = plan.Find(ImportResourceKind::Asset, stem);
        const StringView name = entry != nullptr && !entry->targetName.IsEmpty() ? entry->targetName.AsView() : stem;
        return FindImportClash(group, name, typeName, sourcePath, ledgerPath);
    }
}
