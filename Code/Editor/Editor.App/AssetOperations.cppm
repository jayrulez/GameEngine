// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_operations partition
//
// What can be done to the project's assets and groups: rename, duplicate, delete, create a group,
// set the export flag, cook. Each has one implementation, so the assets browser's menus and keys
// and an agent's delete over MCP do the same thing: an asset's pages close before it is deleted,
// the default scene is cleared when it is deleted, the cooked mirror follows a rename, and nothing
// changes the databases while a cook reads them (the operation waits for the cook and says so).
// Names are checked by AssetNames, the rule Create Asset and Save As use.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:asset_operations;

import foundation.core;
import foundation.content;
import foundation.vfs;
import editor.core;
import :asset_create_dialog; // AssetNames
import :group_picker_dialog; // GroupPicker::ShownName

using namespace foundation::core;

export namespace editor::app
{
    namespace content = foundation::content;

    class AssetOperations
    {
    public:
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr; // null: nothing waits (a test)
            /// Closes every page editing an asset, now (EditorPages::CloseAssetNow).
            Function<void(const Guid&)> closePages;
            /// Runs work after the event that asked for it (a confirmation's answer).
            Function<void(Function<void()>)> later;
        };

        explicit AssetOperations(Services services) : m_services(Move(services)) {}

        // === Renaming ===
        /// Renames an asset; returns empty when it was renamed (or will be, after a cook), else
        /// the reason it was not.
        String Rename(const Guid& id, StringView name)
        {
            content::Instance* instance = Find(id);
            if (instance == nullptr)
                return String(u8"That asset is gone.");
            if (instance->Name() == name)
                return String{};
            if (String refused = AssetNames::Refusal(instance->OwningGroup(), name); !refused.IsEmpty())
                return refused;
            if (Waits([this, id, renamed = String(name)]() { (void)Rename(id, renamed.AsView()); }, u8"Rename"))
                return String{};
            editor::EditorProject& project = *Project();
            const String oldPath = instance->Path();
            if (const Status renamed = project.SourceDb().RenameInstance(id, name); !renamed.IsOk())
                return String(u8"The rename failed (see the Console).");
            (void)project.CookedDb().RenameInstance(id, name); // keep its product's name in step (everything binds by guid)
            if (project.Settings().defaultSceneId == id || project.Settings().defaultScene == oldPath)
            {
                project.Settings().defaultSceneId = id;
                project.Settings().defaultScene = instance->Path(); // keep the path mirror in step
                (void)project.SaveSettings();
            }
            LOG_INFO(u8"Assets", u8"renamed '{}' -> '{}'", oldPath, instance->Path());
            return String{};
        }
        /// Renames a group (not the top one); returns empty on success, else the reason.
        String RenameGroup(content::Group& group, StringView name)
        {
            if (group.Parent() == nullptr)
                return String(u8"Content cannot be renamed.");
            if (group.Name() == name)
                return String{};
            if (String refused = GroupNameRefusal(*group.Parent(), name); !refused.IsEmpty())
                return refused;
            content::Group* target = &group;
            if (Waits([this, target, renamed = String(name)]() { (void)RenameGroup(*target, renamed.AsView()); }, u8"Rename"))
                return String{};
            editor::EditorProject& project = *Project();
            const String oldPath = group.Path();
            if (!project.SourceDb().RenameGroup(group, name).IsOk())
                return String(u8"The rename failed (see the Console).");
            if (content::Group* cooked = GroupAt(project.CookedDb(), oldPath.AsView()))
                (void)project.CookedDb().RenameGroup(*cooked, name);
            RefreshDefaultScenePath(project);
            LOG_INFO(u8"Assets", u8"renamed group '{}' -> '{}'", oldPath, group.Path());
            return String{};
        }

        // === Making ===
        /// Makes a copy of an asset beside it ("Crate.2"); null (and reported) when it could not
        /// be made.
        content::Instance* Duplicate(const Guid& id)
        {
            content::Instance* source = Find(id);
            if (source == nullptr || Waits([this, id]() { (void)Duplicate(id); }, u8"Duplicate"))
                return nullptr;
            const String name = source->OwningGroup().UniqueInstanceName(source->Name());
            content::Instance* copy = Project()->SourceDb().CloneInstance(id, name.AsView());
            if (copy == nullptr)
            {
                Notify(editor::NoticeKind::Error, u8"The duplicate failed (see the Console).");
                return nullptr;
            }
            LOG_INFO(u8"Assets", u8"duplicated '{}' -> '{}'", source->Path(), copy->Path());
            Context().SetStatus(Format(u8"Duplicated as {}.", copy->Name()).AsView());
            if (m_services.cook != nullptr)
                m_services.cook->RequestCook(false); // makes a builder's copy pickable at once
            return copy;
        }
        /// Makes a new group in `parent` ("Group", "Group.2") and its folder on disk at once, so
        /// the group persists while empty; null when it could not be made.
        content::Group* CreateGroup(content::Group& parent)
        {
            content::Group* in = &parent;
            if (Project() == nullptr || Waits([this, in]() { (void)CreateGroup(*in); }, u8"New group"))
                return nullptr;
            const String name = parent.UniqueGroupName(u8"Group");
            content::Group* created = parent.CreateGroup(name.AsView());
            if (created == nullptr)
                return nullptr;
            // The database is scanned from its folders: an empty group with no folder would vanish.
            if (auto* writable = Project()->SourceDb().Mount().AsWritable())
                (void)writable->CreateDirectory(created->Path().AsView());
            LOG_INFO(u8"Assets", u8"created group '{}'", created->Path());
            return created;
        }

        // === Moving ===
        /// Whether `ids` can move into `to`, and when not, why (all are there already, a name is
        /// taken).
        [[nodiscard]] bool CanMove(Span<const Guid> ids, const content::Group& to, String& why) const
        {
            usize moving = 0;
            for (const Guid& id : ids)
            {
                const content::Instance* instance = Find(id);
                if (instance == nullptr || &instance->OwningGroup() == &to)
                    continue;
                if (to.GetInstance(instance->Name()) != nullptr)
                {
                    why = Format(u8"{} is already in {}", instance->Name(), GroupPicker::ShownName(to));
                    return false;
                }
                ++moving;
            }
            if (moving == 0)
                why = Format(u8"Already in {}", GroupPicker::ShownName(to));
            return moving > 0;
        }
        /// Moves assets into `to`, with their files, and the cooked mirror follows. An asset whose
        /// name is taken there stays where it is, and this is reported.
        void MoveTo(Array<Guid> ids, content::Group& to)
        {
            content::Group* target = &to;
            AssetOperations* self = this;
            if (Project() == nullptr || Waits([self, ids, target]() mutable { self->MoveTo(foundation::core::Move(ids), *target); }, u8"Move"))
                return;
            editor::EditorProject& project = *Project();
            usize moved = 0;
            for (const Guid& id : ids)
            {
                content::Instance* instance = Find(id);
                if (instance == nullptr || &instance->OwningGroup() == &to)
                    continue;
                if (to.GetInstance(instance->Name()) != nullptr)
                {
                    Notify(editor::NoticeKind::Warning, Format(u8"{} stays: {} has one of that name.", instance->Name(), GroupPicker::ShownName(to)).AsView());
                    continue;
                }
                const String from = instance->Path();
                if (!project.SourceDb().MoveInstance(id, to).IsOk())
                {
                    Notify(editor::NoticeKind::Error, Format(u8"{} could not be moved (see the Console).", instance->Name()).AsView());
                    continue;
                }
                ++moved;
                LOG_INFO(u8"Assets", u8"moved '{}' -> '{}'", from, instance->Path());
                // Move the cooked product too. Everything binds by guid, so this only keeps the
                // mirror accurate.
                if (project.CookedDb().GetInstance(id) != nullptr)
                    if (content::Group* cooked = MakeGroupAt(project.CookedDb(), to.Path().AsView()))
                        (void)project.CookedDb().MoveInstance(id, *cooked);
            }
            RefreshDefaultScenePath(project);
            if (moved > 0)
                Context().SetStatus(moved == 1 ? Format(u8"Moved 1 asset to {}.", GroupPicker::ShownName(to)).AsView()
                                               : Format(u8"Moved {} assets to {}.", moved, GroupPicker::ShownName(to)).AsView());
        }

        // === Deleting ===
        /// The names a confirmation shows for `ids`: "Crate", "Crate and Barrel", "Crate, Barrel and 3 more".
        [[nodiscard]] String Names(Span<const Guid> ids) const
        {
            Array<StringView> names;
            for (const Guid& id : ids)
                if (const content::Instance* instance = Find(id))
                    names.PushBack(instance->Name());
            if (names.IsEmpty())
                return String{};
            if (names.Size() == 1)
                return String(names[0]);
            if (names.Size() == 2)
                return Format(u8"{} and {}", names[0], names[1]);
            if (names.Size() == 3)
                return Format(u8"{}, {} and {}", names[0], names[1], names[2]);
            return Format(u8"{}, {} and {} more", names[0], names[1], names.Size() - 2);
        }
        /// Deletes assets after the event that asked (closing their pages first), waiting for a
        /// running cook; reports how many were deleted.
        void Delete(Array<Guid> ids)
        {
            AssetOperations* self = this;
            Later([self, pendingIds = Move(ids)]() mutable { self->DeleteNow(Move(pendingIds)); });
        }
        /// Deletes one asset now (an agent's delete, which does its own waiting); false when it is
        /// not there or was not deleted.
        bool DeleteOne(const Guid& id)
        {
            const bool gone = Remove(id);
            if (gone)
                ClearDefaultSceneIfGone();
            return gone;
        }
        /// Deletes a group and all it holds after the event that asked, closing its assets' pages
        /// first; `whereNext` gets the group to show if the one shown was inside it.
        void DeleteGroup(content::Group& group, Function<void(content::Group&)> whereNext = {})
        {
            if (group.Parent() == nullptr)
                return;
            AssetOperations* self = this;
            content::Group* target = &group;
            Later([self, target, pendingWhereNext = Move(whereNext)]() mutable { self->DeleteGroupNow(*target, Move(pendingWhereNext)); });
        }
        /// Counts the assets a group holds, all the way down.
        [[nodiscard]] static usize CountAssets(const content::Group& group)
        {
            usize count = group.Instances().Size();
            for (const content::Group* child : group.Groups())
                count += CountAssets(*child);
            return count;
        }
        static void CollectAssets(const content::Group& group, Array<Guid>& out)
        {
            for (const content::Instance* instance : group.Instances())
                out.PushBack(instance->Id());
            for (const content::Group* child : group.Groups())
                CollectAssets(*child, out);
        }

        // === Exporting and cooking ===
        /// Whether an export always includes the asset, whether or not anything references it
        /// ("Always Export").
        [[nodiscard]] bool IsAlwaysExported(const Guid& id) const { return Project() != nullptr && Project()->ExportRoots().HasInstance(id); }
        [[nodiscard]] bool IsAlwaysExported(const content::Group& group) const { return Project() != nullptr && Project()->ExportRoots().HasGroup(group.Path().AsView()); }
        void ToggleAlwaysExport(const Guid& id)
        {
            const content::Instance* instance = Find(id);
            if (instance == nullptr)
                return;
            const bool now = Project()->ExportRoots().ToggleInstance(id);
            SaveExportRoots(now ? Format(u8"{} is always exported.", instance->Name()) : Format(u8"{} is exported when reached.", instance->Name()));
        }
        void ToggleAlwaysExport(const content::Group& group)
        {
            if (Project() == nullptr)
                return;
            const bool now = Project()->ExportRoots().ToggleGroup(group.Path().AsView());
            SaveExportRoots(now ? Format(u8"Everything in {} is always exported.", group.Name()) : Format(u8"{} is exported when reached.", group.Name()));
        }
        /// Cooks the assets (and what they need); `force` rebuilds them.
        void Cook(Array<Guid> ids, bool force)
        {
            if (m_services.cook != nullptr && !ids.IsEmpty())
                m_services.cook->RequestCookFor(Move(ids), force);
        }
        void CookGroup(const content::Group& group, bool force)
        {
            Array<Guid> ids;
            CollectAssets(group, ids);
            Cook(Move(ids), force);
        }
        void CookAll(bool force)
        {
            if (m_services.cook != nullptr)
                m_services.cook->RequestCook(force);
        }

    private:
        [[nodiscard]] editor::EditorContext& Context() const { return *m_services.context; }
        [[nodiscard]] editor::EditorProject* Project() const { return m_services.context != nullptr ? m_services.context->Project() : nullptr; }
        [[nodiscard]] content::Instance* Find(const Guid& id) const { return Project() != nullptr ? Project()->SourceDb().GetInstance(id) : nullptr; }
        void Notify(editor::NoticeKind kind, StringView text) const { Context().Notify(kind, text); }
        void Later(Function<void()> work)
        {
            if (m_services.later)
                m_services.later(Move(work));
            else
                work();
        }
        // While a cook reads the databases, schedules `work` for when the cook is idle, reports
        // the wait and returns true.
        bool Waits(Function<void()> work, StringView what)
        {
            if (m_services.cook == nullptr || !m_services.cook->MutationLocked())
                return false;
            m_services.cook->RunWhenIdle(Move(work));
            Notify(editor::NoticeKind::Info, Format(u8"{} waits for the cook to finish.", what).AsView());
            return true;
        }
        [[nodiscard]] static String GroupNameRefusal(const content::Group& parent, StringView name)
        {
            if (name.IsEmpty())
                return String(u8"Enter a name.");
            for (const utf8char c : name)
                for (const utf8char refused : StringView(u8"/\\:*?\"<>|"))
                    if (c == refused)
                        return Format(u8"A name cannot hold {}.", StringView(&c, 1));
            if (parent.GetGroup(name) != nullptr)
                return Format(u8"{} is already there.", name);
            return String{};
        }
        [[nodiscard]] static content::Group* MakeGroupAt(content::ContentDatabase& db, StringView path)
        {
            content::Group* group = db.RootGroup();
            usize start = 0;
            for (usize i = 0; i <= path.Size() && group != nullptr; ++i)
                if (i == path.Size() || path[i] == u8'/')
                {
                    if (i > start)
                        group = group->CreateGroup(path.SubStr(start, i - start));
                    start = i + 1;
                }
            return group;
        }
        [[nodiscard]] static content::Group* GroupAt(content::ContentDatabase& db, StringView path)
        {
            content::Group* group = db.RootGroup();
            usize start = 0;
            for (usize i = 0; i <= path.Size() && group != nullptr; ++i)
                if (i == path.Size() || path[i] == u8'/')
                {
                    if (i > start)
                        group = group->GetGroup(path.SubStr(start, i - start));
                    start = i + 1;
                }
            return group;
        }

        void DeleteNow(Array<Guid> ids)
        {
            if (Project() == nullptr)
                return;
            AssetOperations* self = this;
            if (Waits([self, ids]() mutable { self->DeleteNow(Move(ids)); }, u8"Delete"))
                return;
            usize gone = 0;
            for (const Guid& id : ids)
                gone += Remove(id) ? 1u : 0u;
            ClearDefaultSceneIfGone();
            Context().SetStatus(gone == 1 ? String(u8"Deleted 1 asset.").AsView() : Format(u8"Deleted {} assets.", gone).AsView());
        }
        void DeleteGroupNow(content::Group& group, Function<void(content::Group&)> whereNext)
        {
            if (Project() == nullptr)
                return;
            if (m_services.cook != nullptr && m_services.cook->MutationLocked())
            {
                AssetOperations* self = this;
                content::Group* target = &group;
                (void)Waits([self, target, pendingWhereNext = Move(whereNext)]() mutable { self->DeleteGroupNow(*target, Move(pendingWhereNext)); }, u8"Delete");
                return;
            }
            Array<Guid> ids;
            CollectAssets(group, ids);
            for (const Guid& id : ids)
                if (m_services.closePages)
                    m_services.closePages(id);
            if (whereNext)
                whereNext(*group.Parent()); // move away before the group's pointers are freed
            const String path = group.Path();
            if (Project()->SourceDb().DeleteGroup(group).IsOk())
            {
                LOG_INFO(u8"Assets", u8"deleted group '{}' ({} asset(s))", path, ids.Size());
                Context().SetStatus(Format(u8"Deleted {}.", path).AsView());
            }
            else
                Notify(editor::NoticeKind::Error, u8"The group's delete failed (see the Console).");
            ClearDefaultSceneIfGone();
        }
        bool Remove(const Guid& id)
        {
            content::Instance* instance = Find(id);
            if (instance == nullptr)
                return false;
            const String path = instance->Path();
            if (m_services.closePages)
                m_services.closePages(id);
            if (!Project()->SourceDb().DeleteInstance(id).IsOk())
            {
                LOG_WARNING(u8"Assets", u8"delete FAILED for '{}'", path);
                return false;
            }
            LOG_INFO(u8"Assets", u8"deleted '{}'", path); // the next cook sweeps its product
            return true;
        }
        void ClearDefaultSceneIfGone()
        {
            editor::EditorProject* project = Project();
            if (project == nullptr || (project->Settings().defaultSceneId.IsNil() && project->Settings().defaultScene.IsEmpty()))
                return;
            const bool there = !project->Settings().defaultSceneId.IsNil() ? project->SourceDb().GetInstance(project->Settings().defaultSceneId) != nullptr
                                                                           : project->SourceDb().GetInstance(project->Settings().defaultScene.AsView()) != nullptr;
            if (there)
                return;
            project->Settings().defaultSceneId = Guid{};
            project->Settings().defaultScene = String{};
            (void)project->SaveSettings();
            LOG_INFO(u8"Assets", u8"the default scene was deleted: cleared in the manifest");
        }
        static void RefreshDefaultScenePath(editor::EditorProject& project)
        {
            if (project.Settings().defaultSceneId.IsNil())
                return;
            if (content::Instance* scene = project.SourceDb().GetInstance(project.Settings().defaultSceneId);
                scene != nullptr && project.Settings().defaultScene != scene->Path())
            {
                project.Settings().defaultScene = scene->Path();
                (void)project.SaveSettings();
            }
        }
        void SaveExportRoots(const String& said)
        {
            if (!Project()->SaveExportRoots().IsOk())
            {
                Notify(editor::NoticeKind::Error, u8"The export roots could not be saved (export_roots.xml).");
                return;
            }
            Notify(editor::NoticeKind::Info, said.AsView());
        }

        Services m_services;
    };
}
