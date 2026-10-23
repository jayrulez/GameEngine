// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::UI.Resource - the `foundation.ui.resource` module.
//
// Cooked game-UI content. v1 payloads are VALIDATED TEXT:
// the cook parses (markup / SSS) and FAILS on errors, but ships the source text - the
// runtime re-parses at bind. Documents are TEMPLATES: every canvas instantiates its own view
// tree from UIDocument::markup; themes parse once per bind and are shared.
//
// Deliberately FREE of the foundation.ui framework: validation lives in the editor
// builders, instantiation in the subsystem - a headless tool can read these records
// without pulling the whole UI stack.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.resource;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::ui
{
    /// Cooked UI document: a validated `.sml` view-tree payload.
    class UIDocumentSource : public ISerializable
    {
        RTTI_OBJECT(UIDocumentSource, ISerializable)
    public:
        String markup;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "markup", markup);
        }
    };

    /// Runtime product a canvas's Ref binds; the subsystem instantiates a FRESH view
    /// tree per canvas from `markup` (documents are templates, never shared live trees).
    class UIDocument : public Object
    {
        RTTI_OBJECT(UIDocument, Object)
    public:
        String markup;
    };

    class UIDocumentFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required - the
        // application that registers the factory decides).
        explicit UIDocumentFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UIDocument::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &UIDocumentSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            UIDocumentSource* source = Cast<UIDocumentSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<UIDocument> document = MakeRef<UIDocument>(*m_allocator);
            document->markup = String(source->markup.AsView());
            return document;
        }
    
    private:
        IAllocator* m_allocator;
    };

    /// An SVG a theme names with `@icon name "{guid}"` (a vector image asset), embedded in the cooked
    /// theme: the stylesheet then parses on its own, with nothing else to wait for, and `svg(name)`
    /// draws it. `id` is the reference exactly as the sheet writes it.
    struct UIThemeIcon
    {
        String id;
        String svg;
    };

    /// Cooked UI theme: a validated `.sss` stylesheet payload, and the icons it names.
    class UIThemeSource : public ISerializable
    {
        RTTI_OBJECT(UIThemeSource, ISerializable)
    public:
        String stylesheet;
        Array<UIThemeIcon> icons;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "stylesheet", stylesheet);
            u32 count = static_cast<u32>(icons.Size());
            ar.Key("icons");
            ar.BeginArray(count);
            if (ar.Mode() == SerializeMode::Read)
            {
                icons.Clear();
                icons.Resize(count);
            }
            for (UIThemeIcon& icon : icons)
            {
                ar.BeginObject();
                foundation::core::Serialize(ar, "id", icon.id);
                foundation::core::Serialize(ar, "svg", icon.svg);
                ar.EndObject();
            }
            ar.EndArray();
        }
    };

    class UITheme : public Object
    {
        RTTI_OBJECT(UITheme, Object)
    public:
        String stylesheet;
        Array<UIThemeIcon> icons;

        /// The SVG an icon reference (`"{guid}"`, as the sheet writes it) stands for; null if the
        /// theme carries none by that reference.
        [[nodiscard]] const String* FindIcon(StringView id) const noexcept
        {
            for (const UIThemeIcon& icon : icons)
            {
                if (icon.id.AsView() == id)
                {
                    return &icon.svg;
                }
            }
            return nullptr;
        }
    };

    class UIThemeFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required - the
        // application that registers the factory decides).
        explicit UIThemeFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UITheme::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &UIThemeSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            UIThemeSource* source = Cast<UIThemeSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<UITheme> theme = MakeRef<UITheme>(*m_allocator);
            theme->stylesheet = String(source->stylesheet.AsView());
            theme->icons = Move(source->icons);
            return theme;
        }
    
    private:
        IAllocator* m_allocator;
    };

    inline void RegisterUIResource()
    {
        GlobalTypeRegistry().Register(UIDocumentSource::StaticType());
        RegisterSerializable<UIDocumentSource>();
        GlobalTypeRegistry().Register(UIDocument::StaticType());
        GlobalTypeRegistry().Register(UIThemeSource::StaticType());
        RegisterSerializable<UIThemeSource>();
        GlobalTypeRegistry().Register(UITheme::StaticType());
    }

    RTTI_DEFINE_OBJECT(UIDocumentSource, "rtti::ui")
    RTTI_DEFINE_OBJECT(UIDocument, "rtti::ui")
    // v2: the icons the sheet names, embedded.
    RTTI_DEFINE_OBJECT_VERSIONED(UIThemeSource, "rtti::ui", 2)
    RTTI_DEFINE_OBJECT(UITheme, "rtti::ui")
}

export namespace foundation::ui
{
    /// The UI resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kUIResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<UIDocument, UIDocumentSource, UIDocumentFactory>(),
        foundation::resource::FactoryWithAllocator<UITheme, UIThemeSource, UIThemeFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kUIResourceModule{
        u8"ui", &RegisterUIResource, kUIResourceFactories,
        sizeof(kUIResourceFactories) / sizeof(kUIResourceFactories[0])};
}
