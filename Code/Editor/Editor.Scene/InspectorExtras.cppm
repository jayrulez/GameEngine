// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :inspector_extras partition.
//
// What a section in the inspector shows besides its reflected rows, for a component or, while the
// scene is selected, for a settings block: notices that hold while a condition does (a rigid body
// with no collider), actions (a zone's Bake) and sections of their own (a script component's
// behaviours, the physics block's collision groups). The inspector asks the section's type for
// them, so it names no type. A domain registers its type's extras (RegisterSceneEditor registers
// the built-in ones), and the inspector adds what a provider gives under the section's header,
// showing a notice only while its message is not empty.

module;
#include "Core/Prelude.h"

export module editor.scene:inspector_extras;

import foundation.core;
import foundation.gui.toolkit; // IPropertySource (a section of its own)
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    /// What a provider fills, for the entities the inspector shows (each has the component; there
    /// are no entities for a settings block).
    class InspectorExtras
    {
    public:
        virtual ~InspectorExtras() = default;
        [[nodiscard]] virtual SceneEditContext& Edit() = 0;
        [[nodiscard]] virtual EditorContext* Context() = 0;
        [[nodiscard]] virtual Span<const Guid> Entities() const = 0;
        /// A notice under the header. `message` is asked again whenever the inspector shows its
        /// values, so the notice stays current, and the notice shows while the message is not
        /// empty.
        virtual void AddNotice(StringView name, Function<String()> message) = 0;
        /// An action under the header: a button `label` that runs `run`.
        virtual void AddAction(StringView name, StringView label, StringView description, Function<void()> run) = 0;
        /// Rows of a source of its own under the header (a script component's behaviours, which
        /// reflection does not show); `refresh` runs as the inspector shows its values.
        virtual void AddSection(UniquePtr<foundation::gui::IPropertySource> source, Function<void()> refresh) = 0;
    };

    using InspectorExtrasProvider = Function<void(InspectorExtras& extras)>;

    /// The extras each component type has, by its type.
    class InspectorExtrasRegistry
    {
    public:
        explicit InspectorExtrasRegistry(IAllocator& allocator) : m_entries(allocator) {}
        /// The editor's one registry (RegisterSceneEditor fills it).
        static InspectorExtrasRegistry& Get()
        {
            static InspectorExtrasRegistry registry(EditorRootAllocator());
            return registry;
        }
        void Register(const TypeInfo& type, InspectorExtrasProvider provider)
        {
            m_entries.PushBack(Entry{&type, Move(provider)});
        }
        /// Every provider of `type`'s extras, in their registration's order.
        template <typename Fn>
        void ForEach(const TypeInfo& type, Fn&& fn) const
        {
            for (const Entry& entry : m_entries)
                if (entry.type == &type)
                    fn(entry.provider);
        }

    private:
        struct Entry
        {
            const TypeInfo* type = nullptr;
            InspectorExtrasProvider provider;
        };
        Array<Entry> m_entries;
    };
}
