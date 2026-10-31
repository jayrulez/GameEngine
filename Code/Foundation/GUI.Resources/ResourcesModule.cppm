// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Resources - `foundation.gui.resources`: cooked GUI documents and style sheets.
//
// Each payload is validated text. The cook (GUI.Pipeline) parses a `.xml` document with the GUI's
// markup loader and a `.css` sheet with its CSS parser, fails when either cannot be read, and ships
// the source text. The host of the GUI parses the text again: a document is a template, and each
// use builds its own tree from it. This library does not depend on the GUI framework, so a
// headless tool can read these resources. Nothing is embedded: a sheet or a document names an
// image, a vector image, a font or another document by its asset guid, which the host's resource
// provider resolves, so an icon is cooked once however many sheets draw it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.resources;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::gui
{
    /// Cooked GUI document: validated `.xml` markup.
    class GuiDocumentSource : public ISerializable
    {
        RTTI_OBJECT(GuiDocumentSource, ISerializable)
    public:
        String markup;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "markup", markup); }
    };

    /// The runtime product: the document's markup, a template from which each use builds a tree.
    class GuiDocument : public Object
    {
        RTTI_OBJECT(GuiDocument, Object)
    public:
        String markup;
    };

    /// Cooked GUI style sheet: validated `.css`.
    class GuiStyleSheetSource : public ISerializable
    {
        RTTI_OBJECT(GuiStyleSheetSource, ISerializable)
    public:
        String css;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "css", css); }
    };

    /// The runtime product: the sheet's CSS, for a host to style a GUI with.
    class GuiStyleSheet : public Object
    {
        RTTI_OBJECT(GuiStyleSheet, Object)
    public:
        String css;
    };

    /// Makes a product from its cooked text by copying `Cooked::*CookedText` into
    /// `Product::*ProductText`.
    template <typename Product, typename Cooked, String Product::* ProductText, String Cooked::* CookedText>
    class GuiTextFactory final : public IResourceFactory
    {
    public:
        explicit GuiTextFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override { return &Product::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &Cooked::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            Cooked* source = Cast<Cooked>(object.Get());
            if (source == nullptr)
                return RefPtr<Object>{};
            RefPtr<Product> product = MakeRef<Product>(*m_allocator);
            product.Get()->*ProductText = Move(source->*CookedText);
            return product;
        }

    private:
        IAllocator* m_allocator;
    };
    using GuiDocumentFactory = GuiTextFactory<GuiDocument, GuiDocumentSource, &GuiDocument::markup, &GuiDocumentSource::markup>;
    using GuiStyleSheetFactory = GuiTextFactory<GuiStyleSheet, GuiStyleSheetSource, &GuiStyleSheet::css, &GuiStyleSheetSource::css>;

    inline void RegisterGuiResource()
    {
        GlobalTypeRegistry().Register(GuiDocumentSource::StaticType());
        RegisterSerializable<GuiDocumentSource>();
        GlobalTypeRegistry().Register(GuiDocument::StaticType());
        GlobalTypeRegistry().Register(GuiStyleSheetSource::StaticType());
        RegisterSerializable<GuiStyleSheetSource>();
        GlobalTypeRegistry().Register(GuiStyleSheet::StaticType());
    }

    RTTI_DEFINE_OBJECT(GuiDocumentSource, "rtti::gui")
    RTTI_DEFINE_OBJECT(GuiDocument, "rtti::gui")
    RTTI_DEFINE_OBJECT(GuiStyleSheetSource, "rtti::gui")
    RTTI_DEFINE_OBJECT(GuiStyleSheet, "rtti::gui")
}

export namespace foundation::gui
{
    /// This library's resource factories, which the engine composition registers.
    inline constexpr foundation::resource::ResourceFactoryDesc kGuiResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<GuiDocument, GuiDocumentSource, GuiDocumentFactory>(),
        foundation::resource::FactoryWithAllocator<GuiStyleSheet, GuiStyleSheetSource, GuiStyleSheetFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kGuiResourceModule{
        u8"gui", &RegisterGuiResource, kGuiResourceFactories, sizeof(kGuiResourceFactories) / sizeof(kGuiResourceFactories[0])};
}
