// Draconic::UIEditor - the `draconic.ui.editor` module (tooling).
//
// Source-side game-UI authoring + cook (docs/design/game-ui.md §5):
//   * UIDocumentAsset / UIThemeAsset: text payloads (.sml view-tree / .sss stylesheet),
//     embedded in the asset (New Asset seeds a starter template; dropping a .sml/.sss
//     file imports its text).
//   * Builders VALIDATE at cook - the payload must parse (markup against the registered
//     control set; SSS through the stylesheet loader) or the cook FAILS - then write the
//     text through to the cooked record (v1 payload; a pre-parsed binary tree can slot in
//     behind the same records later). The framework parsers return null without
//     diagnostics, so failures point at the asset, not a line number (v1 honesty).
//
// Never linked by the runtime.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Log/Log.h"
#include "Draconic.Core/Reflection/Reflect.h"

export module draconic.ui.editor;

import draconic.core;
import draconic.editor;
import draconic.editor.core; // IFileImporter/EditorProject/import plumbing
import draconic.content;
import draconic.ui;
import draconic.ui.resource;

using namespace draconic::core;

export namespace draconic::ui
{
    // The UISandbox pause-menu vocabulary (kebab-case attributes, explicit sizes -
    // unsized children in a root Flex stretch into bars).
    inline constexpr StringView kUIDocumentStarter =
        u8"<Flex direction=\"vertical\" justify=\"center\" align=\"center\" padding=\"32\">\n"
        u8"  <Panel padding=\"24\"\n"
        u8"         style=\"background: rounded-rect(rgb(35, 38, 48), radius=12);\">\n"
        u8"    <Flex direction=\"vertical\" align=\"center\" spacing=\"8\">\n"
        u8"      <Label id=\"title\" text=\"New Document\" font-size=\"24\"/>\n"
        u8"      <Spacer spacer-height=\"12\"/>\n"
        u8"      <Button id=\"ok-btn\" text=\"OK\" width=\"200\" height=\"40\"/>\n"
        u8"    </Flex>\n"
        u8"  </Panel>\n"
        u8"</Flex>\n";

    inline constexpr StringView kUIThemeStarter =
        u8"/* Game theme overrides - selectors match control types and .classes. */\n"
        u8"Label { text-color: #E8E8E8; }\n";

    class UIDocumentAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(UIDocumentAsset, draconic::editor::Asset)
    public:
        String markup;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            draconic::core::Serialize(ar, "markup", markup);
        }
    };

    class UIThemeAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(UIThemeAsset, draconic::editor::Asset)
    public:
        String stylesheet;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            draconic::core::Serialize(ar, "stylesheet", stylesheet);
        }
    };

    class UIDocumentAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &UIDocumentAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UIDocumentSource::StaticType();
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const UIDocumentAsset& da = static_cast<const UIDocumentAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (da.markup.IsEmpty())
            {
                DRACONIC_LOG_ERROR(u8"UI", u8"UI document is empty - nothing to cook");
                return Status{ErrorCode::InvalidArgument};
            }
            // Validation IS the cook: parse against the registered control set. Silent
            // drops (unknown attributes / child elements) surface as cook WARNINGS.
            MarkupLoader::Initialize();
            Array<String> warnings;
            RefPtr<View> tree =
                MarkupLoader::LoadFromString(da.markup.AsView(), nullptr, &warnings);
            if (tree.Get() == nullptr)
            {
                DRACONIC_LOG_ERROR(
                    u8"UI", u8"UI document failed to parse (malformed XML or unknown control)");
                return Status{ErrorCode::InvalidArgument};
            }
            for (const String& warning : warnings)
            {
                DRACONIC_LOG_WARNING(u8"UI", u8"UI document: {}", warning);
            }
            UIDocumentSource cooked;
            cooked.markup = String(da.markup.AsView());
            return ctx.output->WriteObject(cooked);
        }
    };

    class UIThemeAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &UIThemeAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &UIThemeSource::StaticType();
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const UIThemeAsset& ta = static_cast<const UIThemeAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (ta.stylesheet.IsEmpty())
            {
                DRACONIC_LOG_ERROR(u8"UI", u8"UI theme is empty - nothing to cook");
                return Status{ErrorCode::InvalidArgument};
            }
            StyleSheetLoader loader;
            loader.SetPalette(ThemePalette::Dark()); // palette variables resolvable at cook
            RefPtr<StyleSheet> sheet = loader.Load(ta.stylesheet.AsView());
            if (sheet.Get() == nullptr)
            {
                DRACONIC_LOG_ERROR(u8"UI", u8"UI theme failed to parse (malformed SSS)");
                return Status{ErrorCode::InvalidArgument};
            }
            UIThemeSource cooked;
            cooked.stylesheet = String(ta.stylesheet.AsView());
            return ctx.output->WriteObject(cooked);
        }
    };

    /// Drag-drop importer for `.sml` / `.sss` files (text embeds into the asset).
    class UIFileImporter final : public draconic::editor::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"UI"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"sml" || extension == u8"sss";
        }

        [[nodiscard]] Result<draconic::content::Instance*>
        Import(StringView sourcePath, draconic::editor::EditorProject& project,
               draconic::content::Group& group, const draconic::editor::ImportOptions*, Object*,
               Array<draconic::editor::DeferredImportWrite>*) override
        {
            (void)project;
            FileStream stream(sourcePath, FileMode::Read);
            if (!stream.IsValid())
            {
                return Err(ErrorCode::NotFound);
            }
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(stream.Size()));
            if (stream.Read(bytes.Data(), bytes.Size()) != bytes.Size())
            {
                return Err(ErrorCode::Unknown);
            }
            String text(StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()));
            const StringView stem =
                draconic::editor::FileStemOf(draconic::editor::FileNameOf(sourcePath));
            const bool isTheme = draconic::editor::FileExtensionLower(sourcePath) == u8"sss";
            draconic::content::Instance* instance = group.CreateInstance(
                stem, isTheme ? UIThemeAsset::StaticType() : UIDocumentAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            Status written;
            if (isTheme)
            {
                UIThemeAsset asset;
                asset.stylesheet = Move(text);
                written = instance->WriteObject(asset);
            }
            else
            {
                UIDocumentAsset asset;
                asset.markup = Move(text);
                written = instance->WriteObject(asset);
            }
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    inline void RegisterUIAssets()
    {
        GlobalTypeRegistry().Register(UIDocumentAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<UIDocumentAsset>();
        GlobalTypeRegistry().Register(UIThemeAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<UIThemeAsset>();
    }

    // UIDocumentAsset/UIThemeAsset StaticType() are defined WITH reflected properties in
    // UIAssetImpl.cpp (reflection track P1).
}
