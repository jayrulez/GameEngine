// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Core - :asset_creator partition.
//
// An AssetCreator is one way to make a new source asset from nothing (File > New in the editor,
// asset_create over MCP): a fresh instance of its type, seeded with its defaults, in a group. It
// needs the source database and the sources folder and nothing of the editor, so every host
// (the editor, the headless MCP server) creates through the same creators. A creator does the
// writing and nothing else; what follows a creation (the default scene, a cook, a page) is the
// host's. Each pipeline domain registers its own into the AssetCreatorRegistry, and
// pipeline.registration composes them.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module pipeline.core:asset_creator;

import foundation.core;
import foundation.content;

using namespace foundation::core;

export namespace pipeline
{
    /// Where a creator makes its asset: the group the user picked, if any, the source database's
    /// root, the project's sources folder (where a file-backed asset writes its starter file),
    /// the name asked for and the creator's default group. No editor.
    struct AssetCreationContext
    {
        /// The group the creation was asked for; null leaves the choice to the default group.
        foundation::content::Group* picked = nullptr;
        foundation::content::Group* root = nullptr;
        /// Absolute; empty when the host has no sources folder, which a file-backed creator
        /// refuses.
        StringView sourcesRoot;
        /// The name asked for; empty lets the creator use its own ("Material"). A creator makes
        /// it unique in the group, so a caller that wants it exact refuses a taken name first.
        StringView name;
        /// The creator's folder under the root when nothing was picked; empty is the root.
        StringView defaultGroup;

        /// The name asked for, else the creator's `fallback`.
        [[nodiscard]] StringView NameOr(StringView fallback) const noexcept
        {
            return name.IsEmpty() ? fallback : name;
        }

        /// The picked group, else the default group under the root (made when missing), else
        /// the root.
        [[nodiscard]] foundation::content::Group* Target() const
        {
            if (picked != nullptr)
            {
                return picked;
            }
            if (root == nullptr || defaultGroup.IsEmpty())
            {
                return root;
            }
            foundation::content::Group* group = root->GetGroup(defaultGroup);
            return group != nullptr ? group : root->CreateGroup(defaultGroup);
        }
    };

    struct AssetCreator
    {
        String label;
        /// Creators sharing a category land in a submenu of that name ("Primitives"); empty is
        /// a top-level item.
        String category;
        /// The created asset's type: how a tool asks for "an input map", and how a host knows
        /// whether a builder cooks it.
        const TypeInfo* type = nullptr;
        /// Only a document-like creation, a scene, becomes the project's default scene when none
        /// is set; the host applies it.
        bool setsDefaultScene = false;
        /// Where a creation lands when no group was picked: this folder under the root, made
        /// when missing ("Materials"); empty is the root. Data rather than a choice inside run,
        /// so a tool knows the group before creating (and can refuse a taken name there).
        String defaultGroup;
        /// Answers the new instance (owned by its group), or null on a failure.
        Function<foundation::content::Instance*(const AssetCreationContext&)> run;

        /// The created type's name, as the source database names it.
        [[nodiscard]] StringView TypeName() const noexcept
        {
            return type != nullptr ? StringView(reinterpret_cast<const utf8char*>(type->name))
                                   : StringView{};
        }

        /// Runs the creator with its default group in the context: what every host calls.
        [[nodiscard]] foundation::content::Instance* Create(foundation::content::Group* picked,
                                                            foundation::content::Group* root,
                                                            StringView sourcesRoot,
                                                            StringView name = {}) const
        {
            if (!run)
            {
                return nullptr;
            }
            AssetCreationContext context;
            context.picked = picked;
            context.root = root;
            context.sourcesRoot = sourcesRoot;
            context.name = name;
            context.defaultGroup = defaultGroup.AsView();
            return run(context);
        }

        /// The group a creation lands in, without making it: the picked group, else the default
        /// group if it exists, else the root; null when the default group does not exist yet
        /// (nothing in it can clash).
        [[nodiscard]] foundation::content::Group* TargetFor(foundation::content::Group* picked,
                                                            foundation::content::Group* root) const
        {
            if (picked != nullptr)
            {
                return picked;
            }
            if (defaultGroup.IsEmpty())
            {
                return root;
            }
            return root != nullptr ? root->GetGroup(defaultGroup.AsView()) : nullptr;
        }
    };

    /// A uniquely named instance of `type` in `group`, holding `asset` as written: the shape
    /// most creators are. Null when there is no group or the write is refused.
    [[nodiscard]] inline foundation::content::Instance* CreateWrittenInstance(
        foundation::content::Group* group, StringView baseName, const TypeInfo& type,
        ISerializable& asset)
    {
        if (group == nullptr)
        {
            return nullptr;
        }
        foundation::content::Instance* instance =
            group->CreateInstance(group->UniqueInstanceName(baseName).AsView(), type);
        if (instance == nullptr || !instance->WriteObject(asset).IsOk())
        {
            return nullptr;
        }
        return instance;
    }

    /// Every asset creator a host offers, in registration order: each pipeline domain
    /// contributes its own, and the editor's New menus and the MCP creation tools read the one
    /// list.
    class AssetCreatorRegistry
    {
    public:
        explicit AssetCreatorRegistry(IAllocator& allocator)
            : m_allocator(&allocator), m_creators(allocator)
        {
        }

        /// What a creator that builds in memory first allocates from (a primitive's mesh).
        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        /// A creator with no run is dropped.
        void Register(AssetCreator creator)
        {
            if (creator.run)
            {
                m_creators.PushBack(Move(creator));
            }
        }

        [[nodiscard]] usize Count() const noexcept { return m_creators.Size(); }
        [[nodiscard]] const AssetCreator& At(usize index) const { return m_creators[index]; }
        [[nodiscard]] Span<const AssetCreator> All() const noexcept
        {
            return Span<const AssetCreator>{m_creators.Data(), m_creators.Size()};
        }

        /// The creator labelled `label` (ignoring case), or null.
        [[nodiscard]] const AssetCreator* FindByLabel(StringView label) const noexcept
        {
            for (const AssetCreator& creator : m_creators)
            {
                const StringView mine = creator.label.AsView();
                if (mine.Size() == label.Size() && mine.ContainsIgnoreCase(label))
                {
                    return &creator;
                }
            }
            return nullptr;
        }

        /// The one creator of `typeName`, or null when there is none or several (materials come
        /// as PBR and Unlit, so the type alone does not say which).
        [[nodiscard]] const AssetCreator* FindByType(StringView typeName) const noexcept
        {
            const AssetCreator* found = nullptr;
            for (const AssetCreator& creator : m_creators)
            {
                if (creator.TypeName() != typeName)
                {
                    continue;
                }
                if (found != nullptr)
                {
                    return nullptr;
                }
                found = &creator;
            }
            return found;
        }

    private:
        IAllocator* m_allocator;
        Array<AssetCreator> m_creators;
    };
}
