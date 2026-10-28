// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :settings_extra_sources partition, its body. See SettingsExtraSources.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.physics;
import foundation.script.resource;
import foundation.gui;
import foundation.gui.toolkit;
import engine.physics;
import engine.script;
import editor.core;
import :edit;
import :script_behaviors_source;
import :settings_extra_sources;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace script = foundation::script;
using engine::physics::PhysicsSceneSettings;
using engine::script::SceneScriptSettings;

namespace editor
{
    namespace
    {
        constexpr StringView kGroups = u8"groups";
        constexpr u32 kEveryGroup = 0xFFFFFFFFu;

        u64 Mix(u64 hash, u64 value) { return (hash ^ value) * 1099511628211ull; }
    }

    // === CollisionGroupsSource ===

    CollisionGroupsSource::CollisionGroupsSource(SceneEditContext& edit) : m_edit(&edit) { m_signature = Signature(); }

    const PhysicsSceneSettings* CollisionGroupsSource::Settings() const
    {
        foundation::scene::SceneSystem* system = m_edit->FindSystemBySettingsType(&TypeOf<PhysicsSceneSettings>());
        return system != nullptr ? static_cast<const PhysicsSceneSettings*>(system->SettingsInstance()) : nullptr;
    }

    // At least one: a scene without groups has Default, colliding with everything.
    usize CollisionGroupsSource::Count() const
    {
        const PhysicsSceneSettings* settings = Settings();
        return settings != nullptr ? Max<usize>(1u, settings->groupNames.Size()) : 0u;
    }

    String CollisionGroupsSource::NameOf(usize group) const
    {
        const PhysicsSceneSettings* settings = Settings();
        if (settings != nullptr && group < settings->groupNames.Size() && !settings->groupNames[group].IsEmpty())
            return settings->groupNames[group];
        return group == 0 ? String(u8"Default") : Format(u8"Group {}", group);
    }

    // A group without a mask of its own collides with everything.
    u32 CollisionGroupsSource::MaskOf(usize group) const
    {
        const PhysicsSceneSettings* settings = Settings();
        return settings != nullptr && group < settings->groupCollides.Size() ? settings->groupCollides[group] : kEveryGroup;
    }

    u64 CollisionGroupsSource::Signature() const
    {
        u64 hash = Mix(1469598103934665603ull, Count());
        for (usize i = 0; i < Count(); ++i)
            hash = Mix(hash, Hash<String>{}(NameOf(i)));
        return hash;
    }

    void CollisionGroupsSource::Refresh()
    {
        const u64 signature = Signature();
        if (signature != m_signature)
        {
            m_signature = signature;
            NotifyPropertiesChanged(); // the flags' names are the groups'
        }
        else
            NotifyValuesChanged();
    }

    void CollisionGroupsSource::Mutate(const Function<void(PhysicsSceneSettings&)>& mutate)
    {
        // Every group named and masked first, so an edit never leaves a row without its own.
        const usize count = Count();
        Array<String> names;
        Array<u32> masks;
        for (usize i = 0; i < count; ++i)
        {
            names.PushBack(NameOf(i));
            masks.PushBack(MaskOf(i));
        }
        (void)m_edit->MutateSceneSettings(&TypeOf<PhysicsSceneSettings>(), [&](foundation::scene::SceneSystem& system)
                                          {
                                              auto* settings = static_cast<PhysicsSceneSettings*>(system.SettingsInstance());
                                              if (settings == nullptr)
                                                  return;
                                              settings->groupNames = names;
                                              settings->groupCollides = masks;
                                              mutate(*settings);
                                          });
        Refresh();
    }

    CollisionGroupsSource::Where CollisionGroupsSource::Parse(StringView path)
    {
        Where where;
        if (!path.StartsWith(kGroups))
            return where;
        StringView rest = path.SubStr(kGroups.Size(), path.Size() - kGroups.Size());
        if (rest.IsEmpty())
        {
            where.list = true;
            return where;
        }
        if (rest[0] != u8'.')
            return where;
        rest = rest.SubStr(1, rest.Size() - 1);
        usize at = 0;
        i64 index = 0;
        while (at < rest.Size() && rest[at] >= u8'0' && rest[at] <= u8'9')
            index = index * 10 + (rest[at++] - u8'0');
        if (at == 0)
            return where;
        where.group = index;
        if (at + 1 < rest.Size())
            where.row = String(rest.SubStr(at + 1, rest.Size() - at - 1));
        return where;
    }

    void CollisionGroupsSource::GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const
    {
        const usize count = Count();
        if (count == 0)
            return;
        if (parent.IsEmpty())
        {
            gui::PropertyInfo list;
            list.Name = String(kGroups);
            list.DisplayName = String(u8"Collision Groups");
            list.Description = String(u8"A body's group is its index here; a pair of groups collides both ways or neither.");
            list.Kind = gui::PropertyKind::List;
            list.ListOps = gui::PropertyListAdd | gui::PropertyListRemove;
            list.MaxCount = foundation::physics::kCollisionGroupCount;
            out.PushBack(Move(list));
            // Which pairs collide, a grid of the groups (the old editor's matrix).
            gui::PropertyInfo matrix;
            matrix.Name = String(u8"matrix");
            matrix.DisplayName = String(u8"Collides");
            matrix.Description = String(u8"Where a group's row and another's column meet: whether the two collide (both ways).");
            matrix.Kind = gui::PropertyKind::Group;
            matrix.Hint = String(u8"CollisionMatrix");
            out.PushBack(Move(matrix));
            return;
        }
        const Where where = Parse(parent);
        if (where.list)
        {
            for (usize i = 0; i < count; ++i)
            {
                gui::PropertyInfo group;
                group.Name = Format(u8"{}", i);
                group.DisplayName = Format(u8"{}: {}", i, NameOf(i));
                group.Kind = gui::PropertyKind::Group;
                out.PushBack(Move(group));
            }
            return;
        }
        if (where.group < 0 || static_cast<usize>(where.group) >= count || !where.row.IsEmpty())
            return;
        // A group's row: its name (which pairs collide is the matrix's grid, read and written
        // through each group's `collides`, a hidden row).
        gui::PropertyInfo name;
        name.Name = String(u8"name");
        name.DisplayName = String(u8"Name");
        name.Kind = gui::PropertyKind::String;
        out.PushBack(Move(name));
    }

    gui::Variant CollisionGroupsSource::GetValue(StringView path, usize object) const
    {
        (void)object;
        const Where where = Parse(path);
        if (where.list || path == StringView(u8"matrix"))
            return gui::Variant(static_cast<i64>(Count()));
        if (where.group < 0 || static_cast<usize>(where.group) >= Count())
            return gui::Variant{};
        const usize group = static_cast<usize>(where.group);
        if (where.row == StringView(u8"name"))
            return gui::Variant(NameOf(group).AsView());
        if (where.row == StringView(u8"collides"))
        {
            const u64 shown = Count() >= 32 ? u64{kEveryGroup} : (u64{1} << Count()) - 1u;
            return gui::Variant(static_cast<i64>(MaskOf(group) & shown));
        }
        return gui::Variant{};
    }

    void CollisionGroupsSource::CommitEdit(StringView path, const gui::PropertyWrite& write)
    {
        const Where where = Parse(path);
        if (where.group < 0 || static_cast<usize>(where.group) >= Count())
            return;
        const usize group = static_cast<usize>(where.group);
        if (where.row == StringView(u8"name"))
        {
            const String name(Trim(write.Value.AsString()));
            Mutate([group, name](PhysicsSceneSettings& settings) { settings.groupNames[group] = name; });
            return;
        }
        if (where.row == StringView(u8"collides"))
        {
            const u64 value = static_cast<u64>(write.Value.AsInt());
            const u64 mask = write.Mask;
            Mutate([group, value, mask](PhysicsSceneSettings& settings)
                   {
                       for (usize other = 0; other < settings.groupCollides.Size(); ++other)
                       {
                           if ((mask & (u64{1} << other)) == 0)
                               continue;
                           const bool collides = (value & (u64{1} << other)) != 0;
                           // Both ways: the pair's two rows.
                           u32& mine = settings.groupCollides[group];
                           u32& theirs = settings.groupCollides[other];
                           mine = collides ? mine | (1u << other) : mine & ~(1u << other);
                           theirs = collides ? theirs | (1u << group) : theirs & ~(1u << group);
                       }
                   });
        }
    }

    bool CollisionGroupsSource::EditList(StringView path, const gui::ListEdit& edit)
    {
        if (!Parse(path).list)
            return false;
        const usize count = Count();
        if (edit.Kind == gui::ListEdit::Op::Insert)
        {
            // At the end whatever the place asked: a body's group is its index.
            if (count >= foundation::physics::kCollisionGroupCount)
                return false;
            Mutate([count](PhysicsSceneSettings& settings)
                   {
                       settings.groupNames.PushBack(Format(u8"Group {}", count));
                       settings.groupCollides.PushBack(kEveryGroup);
                       for (u32& mask : settings.groupCollides)
                           mask |= 1u << count; // every group collides with the new one
                   });
            return true;
        }
        if (edit.Kind == gui::ListEdit::Op::Remove)
        {
            if (count <= 1 || edit.Index + 1 != count)
                return false; // only the last: the others keep their bodies' indices
            const usize last = count - 1;
            Mutate([last](PhysicsSceneSettings& settings)
                   {
                       settings.groupNames.RemoveAt(last);
                       settings.groupCollides.RemoveAt(last);
                       for (u32& mask : settings.groupCollides)
                           mask |= 1u << last; // a group added there later collides with everything
                   });
            return true;
        }
        return false;
    }

    // === LevelScriptSource ===

    LevelScriptSource::LevelScriptSource(SceneEditContext& edit, EditorContext* context) : m_edit(&edit), m_context(context) { m_signature = Signature(); }

    const SceneScriptSettings* LevelScriptSource::Settings() const
    {
        foundation::scene::SceneSystem* system = m_edit->FindSystemBySettingsType(&TypeOf<SceneScriptSettings>());
        return system != nullptr ? static_cast<const SceneScriptSettings*>(system->SettingsInstance()) : nullptr;
    }

    // The Level class bound, through the editor's resources while the reference has not resolved.
    const script::ScriptClass* LevelScriptSource::Class() const
    {
        const SceneScriptSettings* settings = Settings();
        if (settings == nullptr)
            return nullptr;
        if (settings->script.Get() != nullptr)
            return settings->script.Get();
        if (settings->script.id.IsNil() || m_context == nullptr || m_context->Resources() == nullptr)
            return nullptr;
        return m_context->Resources()->Bind<script::ScriptClass>(settings->script.id).Get();
    }

    u64 LevelScriptSource::Signature() const
    {
        const SceneScriptSettings* settings = Settings();
        u64 hash = Mix(1469598103934665603ull, settings != nullptr ? Hash<Guid>{}(settings->script.id) : 0u);
        return Mix(hash, Class() != nullptr ? 1u : 0u);
    }

    void LevelScriptSource::Refresh()
    {
        const u64 signature = Signature();
        if (signature != m_signature)
        {
            m_signature = signature;
            NotifyPropertiesChanged();
        }
        else
            NotifyValuesChanged();
    }

    void LevelScriptSource::Mutate(const Function<void(SceneScriptSettings&)>& mutate)
    {
        (void)m_edit->MutateSceneSettings(&TypeOf<SceneScriptSettings>(), [&](foundation::scene::SceneSystem& system)
                                          {
                                              if (auto* settings = static_cast<SceneScriptSettings*>(system.SettingsInstance()))
                                                  mutate(*settings);
                                          });
        Refresh();
    }

    void LevelScriptSource::GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const
    {
        const script::ScriptClass* type = Class();
        if (type == nullptr)
            return;
        if (parent.IsEmpty())
        {
            ScriptPropertyRows::Describe(*type, out);
            return;
        }
        String row;
        i32 part = -1;
        ScriptPropertyRows::ParseRow(parent, row, part);
        const script::ScriptPropertyDesc* property = ScriptPropertyRows::Find(*type, row.AsView());
        if (property != nullptr && part < 0)
            ScriptPropertyRows::Parts(*property, out);
    }

    gui::Variant LevelScriptSource::GetValue(StringView path, usize object) const
    {
        (void)object;
        const script::ScriptClass* type = Class();
        const SceneScriptSettings* settings = Settings();
        if (type == nullptr || settings == nullptr)
            return gui::Variant{};
        String row;
        i32 part = -1;
        ScriptPropertyRows::ParseRow(path, row, part);
        const script::ScriptPropertyDesc* property = ScriptPropertyRows::Find(*type, row.AsView());
        if (property == nullptr)
            return gui::Variant{};
        const engine::script::ScriptPropertyOverride* over = settings->FindOverride(property->hash);
        return ScriptPropertyRows::ToGrid(over != nullptr ? over->value : property->defaultValue, property->type, part);
    }

    gui::Variant LevelScriptSource::GetDefault(StringView path, usize object) const
    {
        (void)object;
        const script::ScriptClass* type = Class();
        if (type == nullptr)
            return gui::Variant{};
        String row;
        i32 part = -1;
        ScriptPropertyRows::ParseRow(path, row, part);
        const script::ScriptPropertyDesc* property = ScriptPropertyRows::Find(*type, row.AsView());
        return property != nullptr ? ScriptPropertyRows::ToGrid(property->defaultValue, property->type, part) : gui::Variant{};
    }

    void LevelScriptSource::CommitEdit(StringView path, const gui::PropertyWrite& write)
    {
        const script::ScriptClass* type = Class();
        if (type == nullptr)
            return;
        String row;
        i32 part = -1;
        ScriptPropertyRows::ParseRow(path, row, part);
        const script::ScriptPropertyDesc* property = ScriptPropertyRows::Find(*type, row.AsView());
        if (property == nullptr)
            return;
        const script::ScriptPropertyDesc desc = *property;
        const gui::Variant value = write.Value;
        Mutate([desc, part, value](SceneScriptSettings& settings)
               {
                   const engine::script::ScriptPropertyOverride* over = settings.FindOverride(desc.hash);
                   settings.SetOverride(desc.hash, ScriptPropertyRows::FromGrid(over != nullptr ? over->value : desc.defaultValue, desc.type, part, value));
               });
    }

    bool LevelScriptSource::Revert(StringView path)
    {
        const script::ScriptClass* type = Class();
        if (type == nullptr)
            return false;
        String row;
        i32 part = -1;
        ScriptPropertyRows::ParseRow(path, row, part);
        const script::ScriptPropertyDesc* property = ScriptPropertyRows::Find(*type, row.AsView());
        if (property == nullptr)
            return false;
        const u64 hash = property->hash;
        Mutate([hash](SceneScriptSettings& settings) { settings.RemoveOverride(hash); });
        return true;
    }
}
