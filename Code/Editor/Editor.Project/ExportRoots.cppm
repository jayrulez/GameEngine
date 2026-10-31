// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_roots partition.
//
// The project's explicit "Always Export" set: the entry points a user declares as export roots on
// top of the automatic default-scene and startup-script seeds. There are two kinds:
//   - instance GUIDs: a specific asset the game loads by code (stable across renames and moves,
//     like ProjectSettings::defaultSceneId).
//   - group paths: "this subtree is a root". Every asset under the group ships, and assets
//     dropped in later are included automatically (the `Resources/` folder pattern). Content-DB
//     groups have no stable id, so a group root is keyed by its mount-relative path; a moved
//     folder must be flagged again, as with any path reference.
//
// This is committable project data (export_roots.xml beside the project, keyed by GUID or path),
// so the set is shared by the team and auditable in one place; hidden per-item flags fail most
// often because people forget them. The export driver (CollectExportRoots in :export_pipeline)
// appends these as Flag and Group seed roots, and their dependency closure then ships. It follows
// the export_presets persistence shape (:export_preset): a versioned XML payload, project-local
// and loadable by the CLI.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:export_roots;

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;

using namespace foundation::core;

export namespace editor
{
    namespace vfs = foundation::vfs;

    inline constexpr StringView kExportRootsFile = u8"export_roots.xml";

    // A project's explicit export roots: the root, versioned payload of export_roots.xml. It holds
    // membership only (guids and group paths); the closure expansion lives in the export driver.
    class ExportRootsSet final : public ISerializable
    {
        RTTI_OBJECT(ExportRootsSet, ISerializable)
    public:
        Array<Guid> instances; // flagged asset instances, by guid (stable across renames and moves)
        Array<String> groups;  // flagged group subtrees, by mount-relative path

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "instances", instances);
            foundation::core::Serialize(ar, "groups", groups);
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

        // Sets instance membership and returns `member` (the new state). Idempotent: adding a
        // present guid or removing an absent one changes nothing.
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
        // Flips instance membership and returns the new state (true when it is now a root).
        bool ToggleInstance(const Guid& id) { return SetInstance(id, !HasInstance(id)); }

        // Sets group membership and returns `member`. Idempotent.
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
        // Flips group membership and returns the new state.
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

    // Resolves a group by its mount-relative path ("" is the root group; "a/b" walks child groups).
    // Null when a segment is missing. Segments are walked as ContentDatabase::GetInstance(path) does.
    [[nodiscard]] inline foundation::content::Group*
    FindGroupByPath(foundation::content::ContentDatabase& db, StringView path)
    {
        foundation::content::Group* group = db.RootGroup();
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
            } // a leading, trailing or empty segment
            group = group->GetGroup(part);
            if (group == nullptr)
            {
                return nullptr;
            }
        }
        return group;
    }

    // Appends every instance guid under a group subtree (the group at `groupPath` and all its
    // descendant groups) to `out`. This expands a group root, so membership is dynamic: whatever is
    // under the folder now. A missing group adds nothing.
    inline void CollectGroupInstances(foundation::content::ContentDatabase& db, StringView groupPath,
                                      Array<Guid>& out)
    {
        foundation::content::Group* start = FindGroupByPath(db, groupPath);
        if (start == nullptr)
        {
            return;
        }

        Array<foundation::content::Group*> stack;
        stack.PushBack(start);
        while (!stack.IsEmpty())
        {
            foundation::content::Group* g = stack[stack.Size() - 1];
            stack.RemoveAt(stack.Size() - 1);
            for (foundation::content::Instance* inst : g->Instances())
            {
                if (inst != nullptr)
                {
                    out.PushBack(inst->Id());
                }
            }
            for (foundation::content::Group* child : g->Groups())
            {
                if (child != nullptr)
                {
                    stack.PushBack(child);
                }
            }
        }
    }

    // Reads export_roots.xml from `root`. NotFound when absent (a project with no explicit roots;
    // the caller treats it as an empty set). Works like LoadExportPresets.
    [[nodiscard]] inline Status LoadExportRoots(vfs::IFileSystem& root, ExportRootsSet& out,
                                                StringView fileName = kExportRootsFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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

    // Writes export_roots.xml to `root`, like SaveExportPresets.
    [[nodiscard]] inline Status SaveExportRoots(vfs::IWritableFileSystem& writable,
                                                ExportRootsSet& roots,
                                                StringView fileName = kExportRootsFile)
    {
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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

    RTTI_DEFINE_OBJECT_VERSIONED(ExportRootsSet, "rtti::editor::editor", 1)
}
