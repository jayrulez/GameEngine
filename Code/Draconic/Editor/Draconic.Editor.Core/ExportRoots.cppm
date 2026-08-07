// Draconic::EditorCore - :export_roots partition.
//
// The project's explicit "Always Export" set (docs/design/export-reachability.md, Phase 2): the
// entry points a user declares as export roots ON TOP of the automatic default-scene + startup-
// script seeds. Two kinds:
//   - instance GUIDs - a specific asset the game loads by code (rename/move-proof, like
//                       ProjectSettings::defaultSceneId).
//   - group PATHS    - "this subtree is a root": every asset under the group ships, and assets
//                       dropped in later are auto-included (the `Resources/`-folder pattern).
//                       Content-DB groups carry no stable id, so a group root is keyed by its
//                       mount-relative path (the documented rename caveat - move the folder and
//                       re-flag it, same as any path reference).
//
// Committable project data (export_roots.xml beside the project, keyed by GUID/path), so the set
// is team-shared and auditable in ONE place - the #1 failure mode of hidden per-item flags is
// that people forget them. The export driver (CollectExportRoots in :export_pipeline) appends
// these as Flag/Group seed roots; their dependency closure then ships. Mirrors the export_presets
// persistence shape (:export_preset) - versioned XML payload, project-local, CLI-loadable.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.core:export_roots;

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.xml.serialization;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace vfs = draconic::vfs;

    inline constexpr StringView kExportRootsFile = u8"export_roots.xml";

    // A project's explicit export roots - the root, versioned payload of export_roots.xml. Value
    // membership only (guids + group paths); the closure expansion lives in the export driver.
    class ExportRootsSet final : public ISerializable
    {
        DRACONIC_OBJECT(ExportRootsSet, ISerializable)
    public:
        Array<Guid> instances; // flagged asset instances, by guid (rename/move-proof)
        Array<String> groups;  // flagged group subtrees, by mount-relative path

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "instances", instances);
            draconic::foundation::Serialize(ar, "groups", groups);
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return instances.IsEmpty() && groups.IsEmpty();
        }

        [[nodiscard]] bool HasInstance(const Guid& id) const
        {
            for (const Guid& g : instances)
            {
                if (g == id)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] bool HasGroup(StringView path) const
        {
            for (const String& p : groups)
            {
                if (p.AsView() == path)
                {
                    return true;
                }
            }
            return false;
        }

        // Set instance membership; returns `member` (the new state). Idempotent - adding a present
        // guid or removing an absent one is a no-op.
        bool SetInstance(const Guid& id, bool member)
        {
            const bool has = HasInstance(id);
            if (member && !has)
            {
                instances.PushBack(id);
            }
            else if (!member && has)
            {
                RemoveInstance(id);
            }
            return member;
        }
        // Flip instance membership; returns the NEW state (true = now a root).
        bool ToggleInstance(const Guid& id) { return SetInstance(id, !HasInstance(id)); }

        // Set group membership; returns `member`. Idempotent.
        bool SetGroup(StringView path, bool member)
        {
            const bool has = HasGroup(path);
            if (member && !has)
            {
                groups.PushBack(String(path));
            }
            else if (!member && has)
            {
                RemoveGroup(path);
            }
            return member;
        }
        // Flip group membership; returns the NEW state.
        bool ToggleGroup(StringView path) { return SetGroup(path, !HasGroup(path)); }

    private:
        void RemoveInstance(const Guid& id)
        {
            for (usize i = 0; i < instances.Size(); ++i)
            {
                if (instances[i] == id)
                {
                    instances.RemoveAt(i);
                    return;
                }
            }
        }
        void RemoveGroup(StringView path)
        {
            for (usize i = 0; i < groups.Size(); ++i)
            {
                if (groups[i].AsView() == path)
                {
                    groups.RemoveAt(i);
                    return;
                }
            }
        }
    };

    // Resolve a group by its mount-relative path ("" = the root group; "a/b" walks child groups).
    // Null when a segment is missing. Mirrors ContentDatabase::GetInstance(path)'s segment walk.
    [[nodiscard]] inline draconic::content::Group*
    FindGroupByPath(draconic::content::ContentDatabase& db, StringView path)
    {
        draconic::content::Group* group = db.RootGroup();
        usize start = 0;
        for (usize i = 0; i <= path.Size(); ++i)
        {
            const bool atEnd = (i == path.Size());
            if (!atEnd && path[i] != utf8char('/'))
            {
                continue;
            }
            const StringView part = path.SubStr(start, i - start);
            start = i + 1;
            if (part.IsEmpty())
            {
                continue;
            } // leading/trailing/empty segment
            group = group->GetGroup(part);
            if (group == nullptr)
            {
                return nullptr;
            }
        }
        return group;
    }

    // Append every instance guid under a group subtree (the group at `groupPath` AND all its
    // descendant groups) to `out`. This is the group-root's "subtree-as-root" expansion - dynamic
    // membership by construction (whatever is under the folder NOW). A missing group is a no-op.
    inline void CollectGroupInstances(draconic::content::ContentDatabase& db, StringView groupPath,
                                      Array<Guid>& out)
    {
        draconic::content::Group* start = FindGroupByPath(db, groupPath);
        if (start == nullptr)
        {
            return;
        }

        Array<draconic::content::Group*> stack;
        stack.PushBack(start);
        while (!stack.IsEmpty())
        {
            draconic::content::Group* g = stack[stack.Size() - 1];
            stack.RemoveAt(stack.Size() - 1);
            for (draconic::content::Instance* inst : g->Instances())
            {
                if (inst != nullptr)
                {
                    out.PushBack(inst->Id());
                }
            }
            for (draconic::content::Group* child : g->Groups())
            {
                if (child != nullptr)
                {
                    stack.PushBack(child);
                }
            }
        }
    }

    // Read export_roots.xml from `root`. NotFound when absent (a project with no explicit roots -
    // caller treats it as an empty set). Mirrors LoadExportPresets.
    [[nodiscard]] inline Status LoadExportRoots(vfs::IFileSystem& root, ExportRootsSet& out,
                                                StringView fileName = kExportRootsFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(*stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportRootsSet::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    // Write export_roots.xml to `root`. Mirrors SaveExportPresets.
    [[nodiscard]] inline Status SaveExportRoots(vfs::IWritableFileSystem& writable,
                                                ExportRootsSet& roots,
                                                StringView fileName = kExportRootsFile)
    {
        MemoryStream buffer;
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportRootsSet::StaticType());
        roots.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(buffer);
        return writable.Save(fileName, buffer.Bytes());
    }

    DRACONIC_DEFINE_OBJECT_VERSIONED(ExportRootsSet, "draconic::editor", 1)
}
