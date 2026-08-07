// Draconic::UIResource - the `draconic.ui.resource` module.
//
// Cooked game-UI content (docs/design/game-ui.md §4). v1 payloads are VALIDATED TEXT:
// the cook parses (markup / SSS) and FAILS on errors, but ships the source text - the
// runtime re-parses at bind (v2 upgrades the payload to a pre-parsed binary tree behind
// the same records). Documents are TEMPLATES: every canvas instantiates its own view
// tree from UIDocument::markup; themes parse once per bind and are shared.
//
// Deliberately FREE of the draconic.ui framework: validation lives in the editor
// builders, instantiation in the subsystem - a headless tool can read these records
// without pulling the whole UI stack.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;

using namespace draconic::foundation;
using namespace draconic::resource;

export namespace draconic::ui
{
    /// Cooked UI document: a validated `.sml` view-tree payload.
    class UIDocumentSource : public ISerializable
    {
        DRACONIC_OBJECT(UIDocumentSource, ISerializable)
    public:
        String markup;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "markup", markup);
        }
    };

    /// Runtime product a canvas's Ref binds; the subsystem instantiates a FRESH view
    /// tree per canvas from `markup` (documents are templates, never shared live trees).
    class UIDocument : public Object
    {
        DRACONIC_OBJECT(UIDocument, Object)
    public:
        String markup;
    };

    class UIDocumentFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UIDocument::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            UIDocumentSource* source = Cast<UIDocumentSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<UIDocument> document = MakeRef<UIDocument>(DefaultAllocator());
            document->markup = String(source->markup.AsView());
            return document;
        }
    };

    /// Cooked UI theme: a validated `.sss` stylesheet payload.
    class UIThemeSource : public ISerializable
    {
        DRACONIC_OBJECT(UIThemeSource, ISerializable)
    public:
        String stylesheet;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "stylesheet", stylesheet);
        }
    };

    class UITheme : public Object
    {
        DRACONIC_OBJECT(UITheme, Object)
    public:
        String stylesheet;
    };

    class UIThemeFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UITheme::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            UIThemeSource* source = Cast<UIThemeSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<UITheme> theme = MakeRef<UITheme>(DefaultAllocator());
            theme->stylesheet = String(source->stylesheet.AsView());
            return theme;
        }
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

    DRACONIC_DEFINE_OBJECT(UIDocumentSource, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(UIDocument, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(UIThemeSource, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(UITheme, "draconic::ui")
}
