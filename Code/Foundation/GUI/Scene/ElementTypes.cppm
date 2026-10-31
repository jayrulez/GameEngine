// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :element_types partition
//
// Which element types exist, and which of them one GUI may create.
//
// ElementTypes is the registry of element types. It holds type knowledge, so it is process-wide,
// like the reflection registry. Any library (the GUI itself, a toolkit, a game kit, a viewport)
// adds its widgets by registering their reflected types once at start-up. An element type is a
// reflected Node type with a creator (TypeBuilder::Creator), registered under a tag (its `element`
// attribute, else its type name) that markup uses and a script's `add` accepts. Registering a tag
// that another type already holds fails with an error.
//
// ElementSet is the subset one GUI may create: by default every registered type (including types
// registered later), or only the tags it allows, plus its own aliases. Each GUI holds one
// (GUIContext::SetElementSet), so two GUIs in one process can offer different elements, for
// example to limit a mod's markup to a few.

module;
#include "Core/Prelude.h"

export module foundation.gui:element_types;

import foundation.core;
import :node;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    struct ElementTypes
    {
        /// Registers `type` as an element under its tag, and with the reflection registry so that
        /// scripts can reach it. Returns false and logs an error when it is not a Node type, has no
        /// creator, or its tag belongs to another type.
        static bool Register(const TypeInfo& type);
        /// Unregisters `type` (when a library unloads), so markup and scripts can no longer create
        /// it.
        static void Unregister(const TypeInfo& type);
        /// The element type under `tag`, or null.
        [[nodiscard]] static const TypeInfo* Find(core::StringView tag);
        /// The tag `type` registers under: its `element` attribute, else its type name.
        [[nodiscard]] static core::StringView TagOf(const TypeInfo& type);
        /// Calls `visit` for every registered element type, in registration order.
        static void ForEach(const core::Function<void(core::StringView tag, const TypeInfo& type)>& visit);
    };

    /// The element types one GUI may create: every registered type, or only the tags it allows,
    /// plus its own aliases.
    class ElementSet
    {
    public:
        /// A set that allows every registered element type, including types registered later.
        [[nodiscard]] static ElementSet All() { return ElementSet{}; }
        /// A set that allows only the tags later passed to Allow (none at first).
        [[nodiscard]] static ElementSet None()
        {
            ElementSet set;
            set.m_all = false;
            return set;
        }

        /// Allows `tag` (a registered element's tag) in a set made with None().
        void Allow(core::StringView tag) { m_allowed.PushBack(core::String(tag)); }
        /// Makes `alias` create what `tag` creates (in markup and `add`). The alias is allowed
        /// wherever `tag` is.
        void Alias(core::StringView alias, core::StringView tag)
        {
            m_aliases.PushBack(AliasEntry{core::String(alias), core::String(tag)});
        }

        /// Returns the element type `tag` names in this set (possibly through an alias), or null
        /// when the set does not allow it or no such element is registered.
        [[nodiscard]] const TypeInfo* Resolve(core::StringView tag) const
        {
            for (const AliasEntry& alias : m_aliases)
                if (alias.alias.AsView() == tag)
                    tag = alias.tag.AsView();
            if (!m_all)
            {
                bool allowed = false;
                for (const core::String& entry : m_allowed)
                    allowed = allowed || entry.AsView() == tag;
                if (!allowed)
                    return nullptr;
            }
            return ElementTypes::Find(tag);
        }

        /// Creates a new element of the type `tag` names, from `allocator`. Returns null when the
        /// set does not allow it.
        [[nodiscard]] RefPtr<Node> Create(core::StringView tag, IAllocator& allocator) const
        {
            const TypeInfo* type = Resolve(tag);
            if (type == nullptr)
                return RefPtr<Node>{};
            RefPtr<Object> made = CreateObject(*type, allocator);
            return RefPtr<Node>(Cast<Node>(made.Get()));
        }

    private:
        struct AliasEntry
        {
            core::String alias;
            core::String tag;
        };
        bool m_all = true;
        Array<core::String> m_allowed;
        Array<AliasEntry> m_aliases;
    };

    /// A native weak reference to an element of type T. It resolves through the element table
    /// (which script handles use too) to the element while it lives in its GUI, and to null once
    /// the element or its GUI is destroyed. Use it in callbacks, tools and bindings that must
    /// neither keep a widget alive nor outlive it. It is empty for an element that is in no GUI.
    template <typename T>
    class Element
    {
    public:
        Element() = default;
        explicit Element(T& element)
        {
            if (const Node::ElementRef ref = element.TakeElementRef(); ref.table != nullptr)
            {
                m_table = RefPtr<ElementTable>(ref.table);
                m_slot = ref.slot;
                m_generation = ref.generation;
            }
        }

        [[nodiscard]] T* Get() const noexcept
        {
            return m_table ? static_cast<T*>(m_table->Resolve(m_slot, m_generation)) : nullptr;
        }
        [[nodiscard]] T* operator->() const noexcept { return Get(); }
        [[nodiscard]] bool IsValid() const noexcept { return Get() != nullptr; }
        explicit operator bool() const noexcept { return IsValid(); }

    private:
        RefPtr<ElementTable> m_table;
        u32 m_slot = 0;
        u32 m_generation = 0;
    };
}
