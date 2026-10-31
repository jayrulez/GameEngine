// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :resource_ref_editor partition
//
// The inspector's asset reference: the property grid's editor for GUI.Toolkit's Reference kind, an
// asset slot. What may fill it is asked of the builders, never listed here: the source asset types
// whose builder makes the type the reference points at, or a type derived from it. Pick, drop and
// clear are each one write through the grid, so each is one undo step; a pick opens the asset
// picker (AssetPickerWindow). Open and reveal show the asset. What it needs from the editor is a
// table of its own (ResourceRefServices), so a test supplies its own and the application fills it
// from its context.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:resource_ref_editor;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :asset_picker_slot;
import :asset_picker_window; // AssetPickRequest

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// What a referenced asset shows: its name, its type's glyph and the class that tints it, and
    /// its thumbnail in the glyph's place once there is one.
    struct ResourceRefShown
    {
        String Name;
        String Glyph;
        String GlyphClass;
        RefPtr<gui::Drawable> Picture;
    };

    /// The services a reference row needs from the editor. Each is optional; an unset one leaves
    /// its verb out.
    struct ResourceRefServices
    {
        /// The asset types, by name, that may fill a reference to `target`.
        Function<Array<String>(const TypeInfo& target)> AcceptedTypes;
        /// Describes what `id` names; false when the project has no such asset (a dangling reference).
        Function<bool(const Guid& id, ResourceRefShown& out)> Describe;
        /// Opens a picker over `types` (empty: any) with `current` checked; `chosen` assigns what it
        /// picks (nil: None).
        Function<void(Span<const String> types, const Guid& current, Function<void(const Guid&)> chosen)> Pick;
        Function<void(const Guid&)> Open;
        Function<void(const Guid&)> Reveal;

        /// Fills the table from the editor's context: the types the builders make the target from
        /// (SourceAssetTypesOf; when there are none, the type itself, for a reference naming a
        /// source asset type), the project's names, the type icons and thumbnails, the asset picker
        /// over the project's assets (`open` shows the asset picker for a request), and the
        /// context's open and reveal. `context` and `icons` outlive whatever the table is given to.
        [[nodiscard]] static ResourceRefServices From(editor::EditorContext& context, editor::EditorIcons& icons,
                                                      Function<void(AssetPickRequest request)> open)
        {
            ResourceRefServices services;
            editor::EditorContext* ctx = &context;
            editor::EditorIcons* typeIcons = &icons;
            if (open)
                services.Pick = [ctx, openPicker = Move(open)](Span<const String> types, const Guid& current, Function<void(const Guid&)> chosen)
                {
                    if (ctx->Project() == nullptr)
                        return;
                    AssetPickRequest request;
                    for (const String& type : types)
                        request.types.PushBack(type);
                    request.current = current;
                    request.chosen = Move(chosen);
                    openPicker(Move(request));
                };
            // The types the builders make it from; when there are none, the type itself, for a
            // reference naming a source asset type (as Project Settings' references do).
            services.AcceptedTypes = [ctx](const TypeInfo& target)
            {
                Array<String> names;
                if (ctx->SourceAssetTypesOf)
                    for (const TypeInfo* asset : ctx->SourceAssetTypesOf(target))
                        names.PushBack(String(StringView(reinterpret_cast<const utf8char*>(asset->name))));
                if (names.IsEmpty() && target.name != nullptr)
                    names.PushBack(String(StringView(reinterpret_cast<const utf8char*>(target.name))));
                return names;
            };
            services.Describe = [ctx, typeIcons](const Guid& id, ResourceRefShown& out)
            {
                foundation::content::Instance* instance = ctx->Project() != nullptr ? ctx->Project()->SourceDb().GetInstance(id) : nullptr;
                if (instance == nullptr)
                    return false;
                out.Name = String(instance->Name());
                const TypeInfo* type = GlobalTypeRegistry().FindByName(reinterpret_cast<const char*>(String(instance->TypeNamespace()).CStr()),
                                                                        reinterpret_cast<const char*>(String(instance->TypeName()).CStr()));
                if (type != nullptr)
                {
                    const editor::TypeIcon icon = typeIcons->For(*type, u8"Asset");
                    out.Glyph = icon.Source;
                    out.GlyphClass = icon.Class;
                }
                if (ctx->Thumbnails() != nullptr)
                    out.Picture = ctx->Thumbnails()->Get(id); // null until generated; the next bind asks again
                return true;
            };
            services.Open = [ctx](const Guid& id)
            {
                if (ctx->OpenAsset)
                    ctx->OpenAsset(id);
            };
            services.Reveal = [ctx](const Guid& id)
            {
                if (ctx->RevealAsset)
                    ctx->RevealAsset(id);
            };
            return services;
        }
    };

    /// ResourceRefEditor::kMissingText, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr StringView kResourceRefEditorMissingText = u8"(missing)";

    class ResourceRefEditor final : public gui::SingleControlEditor
    {
        RTTI_OBJECT(ResourceRefEditor, gui::SingleControlEditor)
    public:
        static constexpr StringView kMissingText = kResourceRefEditorMissingText;

        /// `services` (the registrar's) outlives the editor.
        explicit ResourceRefEditor(const ResourceRefServices& services) : m_services(&services)
        {
            AddClass(u8"reference");
            m_slot = MakeRef<AssetPickerSlot>(MemoryAllocator());
            Hold(*m_slot);
        }
        ResourceRefEditor() : ResourceRefEditor(m_none) {} // the constructor RTTI uses (with its own empty services); the registry's instance has real services

        /// The editor for every Reference property `registry` shows; `services` outlives it.
        static void Register(gui::PropertyEditorRegistry& registry, const ResourceRefServices& services)
        {
            const ResourceRefServices* table = &services;
            registry.RegisterKind(gui::PropertyKind::Reference, [table](IAllocator& allocator) -> RefPtr<gui::PropertyEditor>
                                  { return MakeRef<ResourceRefEditor>(allocator, *table); });
        }

        [[nodiscard]] AssetPickerSlot* Slot() const noexcept { return m_slot.Get(); }
        /// What it references: nil for none, or when the objects differ.
        [[nodiscard]] const Guid& Current() const noexcept { return m_current; }

    protected:
        void OnBind(const gui::PropertyInfo& info, const gui::PropertyValue& value) override
        {
            m_current = Guid{};
            m_mixed = value.Mixed;
            if (!m_mixed)
                (void)Guid::TryParse(Trim(value.Value.AsString()), m_current);
            m_types = info.ReferenceType != nullptr && m_services->AcceptedTypes ? m_services->AcceptedTypes(*info.ReferenceType) : Array<String>{};
            m_slot->SetAcceptedTypes(m_types);
            Wire();
            Show();
        }

    private:
        // Wires its verbs as far as the services and the property allow; a read-only reference
        // only opens.
        void Wire()
        {
            ResourceRefEditor* self = this;
            const bool writes = !IsReadOnly();
            m_slot->SetOnPick(writes && m_services->Pick ? Function<void()>([self]() { self->Pick(); }) : Function<void()>{});
            m_slot->SetOnClear(writes ? Function<void()>([self]() { self->Assign(Guid{}); }) : Function<void()>{});
            m_slot->SetOnDropped(writes ? Function<void(const Guid&)>([self](const Guid& id) { self->Assign(id); }) : Function<void(const Guid&)>{});
            m_slot->SetOnOpen(m_services->Open && !m_mixed ? Function<void()>([self]() { self->m_services->Open(self->m_current); }) : Function<void()>{});
            m_slot->SetOnReveal(m_services->Reveal && !m_mixed ? Function<void()>([self]() { self->m_services->Reveal(self->m_current); }) : Function<void()>{});
        }
        void Show()
        {
            m_slot->SetPicture({});
            if (m_mixed)
            {
                m_slot->SetGlyph({});
                m_slot->SetValue(gui::MixedValueText());
                return;
            }
            if (m_current.IsNil())
            {
                m_slot->SetGlyph({});
                m_slot->SetValue({});
                return;
            }
            ResourceRefShown shown;
            if (m_services->Describe && m_services->Describe(m_current, shown))
            {
                m_slot->SetGlyph(shown.Glyph.AsView(), shown.GlyphClass.AsView());
                m_slot->SetPicture(foundation::core::Move(shown.Picture));
                m_slot->SetValue(shown.Name.AsView());
            }
            else
            {
                m_slot->SetGlyph({});
                m_slot->SetValue(kMissingText); // it can still be cleared or replaced
            }
        }
        // The picker answers later, when the row may show another property (rows are pooled):
        // its choice lands only on the property it was opened for.
        void Pick()
        {
            RefPtr<ResourceRefEditor> self(this);
            const String path(GetPath());
            m_services->Pick(m_types.AsSpan(), m_current, [self, path](const Guid& id)
                             {
                                 if (self->GetPath() == path.AsView())
                                     self->Assign(id);
                             });
        }
        // One write through the grid: a pick, a drop and a clear alike.
        void Assign(const Guid& id)
        {
            RefPtr<ResourceRefEditor> keep(this); // keep alive: the write may rebind the row
            String text;
            if (!id.IsNil())
            {
                utf8char chars[37];
                id.ToChars(chars);
                text = String(StringView(chars, 36));
            }
            ApplyEdit(gui::PropertyWrite{gui::Variant(text.AsView())});
        }
        ResourceRefServices m_none; // an editor RTTI makes has no services, so it owns an empty table (a process-wide one would put state in an interface)
        const ResourceRefServices* m_services;
        RefPtr<AssetPickerSlot> m_slot;
        Array<String> m_types;
        Guid m_current;
        bool m_mixed = false;
    };

    RTTI_DEFINE_OBJECT(ResourceRefEditor, "rtti::editor::editor::app")
}
