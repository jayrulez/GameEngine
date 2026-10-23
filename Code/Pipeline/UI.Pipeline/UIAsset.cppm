// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::UI - the `foundation.ui.editor` module (tooling).
//
// Source-side game-UI authoring + cook:
//   * UIDocumentAsset / UIThemeAsset: a LINKED source file (.sml view-tree / .sss
//     stylesheet). The authored text lives in the project's Sources/ tree and the asset
//     references it through Asset::fileName - exactly like a script asset - so by-hand
//     edits touch the real .sml/.sss file, not a payload embedded in the asset. Dropping a
//     .sml/.sss file stages it into Sources/ and links it (New Asset seeds a starter file
//     the same way). An asset with no linked file (empty fileName) fails the cook.
//   * Builders VALIDATE at cook - the text (read from the linked source file) must parse
//     (markup against the registered control set; SSS
//     through the stylesheet loader) or the cook FAILS - then write the text through to the
//     cooked record (v1 payload; a pre-parsed binary tree can slot in behind the same
//     records later). The cooked PRODUCT still embeds the resolved text; only the SOURCE
//     asset links. The framework parsers return null without diagnostics, so failures point
//     at the asset, not a line number (v1 honesty).
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module ui.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.content;
import foundation.ui;
import foundation.ui.gamekit; // RegisterGamekitMarkup - so <screen> roots validate at cook
import foundation.ui.resource;
import vg.pipeline;       // ReadVectorImage: a theme's @icon embeds the vector image it names
import foundation.image;  // IResourceProvider::LoadImage (a theme cook resolves no images)

using namespace foundation::core;
using namespace foundation::ui;

export namespace pipeline{
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

    // The markup lives ONLY in the linked Sources/ file (Asset::fileName) - nothing inline.
    class UIDocumentAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(UIDocumentAsset, pipeline::Asset)
    public:
        void Serialize(ISerializer& ar) override { pipeline::Asset::Serialize(ar); }
    };

    class UIThemeAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(UIThemeAsset, pipeline::Asset)
    public:
        // The stylesheet lives ONLY in the linked Sources/ file (Asset::fileName).
        // EDITOR-ONLY preview scaffolding (no-editor-data-in-runtime): the markup the theme editor
        // previews this stylesheet against, persisted so a theme's preview context survives across
        // sessions (incl. inline edits). NEVER read by UIThemeAssetBuilder -> it stays out of the
        // cooked UIThemeSource / runtime UITheme.
        String previewMarkup;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            // Editor-only preview markup (never read by the builder - see the field).
            foundation::core::Serialize(ar, "previewMarkup", previewMarkup);
        }
    };

    class UIDocumentAssetBuilder final : public pipeline::DefaultAssetBuilder
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

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const UIDocumentAsset& da = static_cast<const UIDocumentAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            // The markup lives in the LINKED source file (fileName) - read it back through the
            // sources mount. No link = nothing to cook.
            if (da.fileName.IsEmpty())
            {
                LOG_ERROR(u8"UI", u8"UI document has no linked source file - cook failed");
                return Status{ErrorCode::InvalidArgument};
            }
            String markup;
            {
                const Status read = ReadSourceText(ctx, da.fileName.View(), markup);
                if (!read.IsOk())
                {
                    LOG_ERROR(u8"UI", u8"UI document '{}': source file missing - cook failed",
                              da.fileName.View());
                    return read;
                }
            }
            if (markup.IsEmpty())
            {
                LOG_ERROR(u8"UI", u8"UI document is empty - nothing to cook");
                return Status{ErrorCode::InvalidArgument};
            }
            // Validation IS the cook: parse against the registered control set. Silent
            // drops (unknown attributes / child elements) surface as cook WARNINGS.
            MarkupLoader::Initialize();
            foundation::ui::gamekit::RegisterGamekitMarkup(); // so <screen> HUD/menu roots validate

            Array<String> warnings;
            RefPtr<View> tree =
                MarkupLoader::LoadFromString(*ctx.allocator, markup.AsView(), nullptr, &warnings);
            if (tree.Get() == nullptr)
            {
                LOG_ERROR(
                    u8"UI", u8"UI document failed to parse (malformed XML or unknown control)");
                return Status{ErrorCode::InvalidArgument};
            }
            for (const String& warning : warnings)
            {
                LOG_WARNING(u8"UI", u8"UI document: {}", warning);
            }
            UIDocumentSource cooked;
            cooked.markup = Move(markup);
            return ctx.output->WriteObject(cooked);
        }
    };

    // A stylesheet's view of the cook: `@icon name "{guid}"` reads the vector image the guid names
    // and records it, so the cooked theme carries every icon it draws; a reference that names
    // none is recorded as missing. Images resolve at runtime (textures stream), so none here.
    class ThemeIconCollector final : public IResourceProvider
    {
    public:
        explicit ThemeIconCollector(const pipeline::AssetBuildContext* ctx) : m_ctx(ctx) {}

        bool LoadText(StringView path, String& outText) override
        {
            Guid id;
            if (!ParseIconReference(path, id))
            {
                m_missing.PushBack(String(path));
                return false;
            }
            for (const UIThemeIcon& icon : m_icons)
            {
                if (icon.id.AsView() == path)
                {
                    outText = icon.svg;
                    return true;
                }
            }
            m_references.PushBack(id);
            String svg;
            if (m_ctx == nullptr || !ReadVectorImage(*m_ctx, id, svg).IsOk() || svg.IsEmpty())
            {
                m_missing.PushBack(String(path));
                return false;
            }
            outText = svg;
            m_icons.PushBack(UIThemeIcon{String(path), Move(svg)});
            return true;
        }
        const foundation::image::ImageData* LoadImage(StringView) override { return nullptr; }

        // "{guid}" or the bare guid, as an image source is written.
        static bool ParseIconReference(StringView path, Guid& out)
        {
            if (path.Size() == 38 && path[0] == utf8char('{') && path[37] == utf8char('}'))
            {
                path = path.SubStr(1, 36);
            }
            return Guid::TryParse(path, out) && !out.IsNil();
        }

        [[nodiscard]] const Array<Guid>& References() const noexcept { return m_references; }
        [[nodiscard]] const Array<String>& Missing() const noexcept { return m_missing; }
        [[nodiscard]] Array<UIThemeIcon>& Icons() noexcept { return m_icons; }

    private:
        const pipeline::AssetBuildContext* m_ctx;
        Array<Guid> m_references;
        Array<String> m_missing;
        Array<UIThemeIcon> m_icons;
    };

    class UIThemeAssetBuilder final : public pipeline::DefaultAssetBuilder
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
        // 2: the icons the sheet names are embedded.
        [[nodiscard]] u32 Version() const override { return 2; }

        // The vector images the sheet's @icon directives name: their content is the theme's too.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx,
                              pipeline::AssetDependencies& out) override
        {
            const UIThemeAsset& ta = static_cast<const UIThemeAsset&>(asset);
            String stylesheet;
            if (ta.fileName.IsEmpty() || !ReadSourceText(ctx, ta.fileName.View(), stylesheet).IsOk())
            {
                return;
            }
            // Parsed with no context: every reference is recorded, none read.
            ThemeIconCollector collector(nullptr);
            StyleSheetLoader loader(*ctx.allocator);
            loader.SetPalette(ThemePalette::Dark());
            loader.ResourceProvider = &collector;
            (void)loader.Load(stylesheet.AsView());
            for (const Guid& id : collector.References())
            {
                out.reads.PushBack(id);
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const UIThemeAsset& ta = static_cast<const UIThemeAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            // The stylesheet lives in the LINKED source file (fileName) - read it back through
            // the sources mount. No link = nothing to cook.
            if (ta.fileName.IsEmpty())
            {
                LOG_ERROR(u8"UI", u8"UI theme has no linked source file - cook failed");
                return Status{ErrorCode::InvalidArgument};
            }
            String stylesheet;
            {
                const Status read = ReadSourceText(ctx, ta.fileName.View(), stylesheet);
                if (!read.IsOk())
                {
                    LOG_ERROR(u8"UI", u8"UI theme '{}': source file missing - cook failed",
                              ta.fileName.View());
                    return read;
                }
            }
            if (stylesheet.IsEmpty())
            {
                LOG_ERROR(u8"UI", u8"UI theme is empty - nothing to cook");
                return Status{ErrorCode::InvalidArgument};
            }
            ThemeIconCollector icons(&ctx);
            StyleSheetLoader loader(*ctx.allocator);
            loader.SetPalette(ThemePalette::Dark()); // palette variables resolvable at cook
            loader.ResourceProvider = &icons;         // @icon: the vector images, embedded below
            RefPtr<StyleSheet> sheet = loader.Load(stylesheet.AsView());
            if (sheet.Get() == nullptr)
            {
                LOG_ERROR(u8"UI", u8"UI theme failed to parse (malformed SSS)");
                return Status{ErrorCode::InvalidArgument};
            }
            if (!icons.Missing().IsEmpty())
            {
                for (const String& missing : icons.Missing())
                {
                    LOG_ERROR(u8"UI", u8"UI theme: @icon \"{}\" names no vector image asset", missing);
                }
                return Status{ErrorCode::NotFound};
            }
            UIThemeSource cooked;
            cooked.stylesheet = Move(stylesheet);
            cooked.icons = Move(icons.Icons());
            return ctx.output->WriteObject(cooked);
        }
    };

    /// Drag-drop importer for `.sml` / `.sss` files (an `.svg` is VG.Pipeline's vector image). The
    /// dropped file is STAGED into the project's Sources/ tree and the asset LINKS it through
    /// fileName - the authored text is never embedded in the asset (mirrors ScriptFileImporter).
    /// The cook reads the source file back through the sources mount.
    class UIFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"UI"; }

        // The asset a UI source file becomes, by its extension.
        [[nodiscard]] static StringView AssetTypeNameFor(StringView sourcePath)
        {
            const String extension = pipeline::FileExtensionLower(sourcePath);
            return extension == u8"sss" ? StringView(u8"UIThemeAsset") : StringView(u8"UIDocumentAsset");
        }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"sml" || extension == u8"sss";
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(foundation::content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, AssetTypeNameFor(sourcePath));
        }

        [[nodiscard]] Result<foundation::content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               foundation::content::Group& group,
               const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            const String extension = pipeline::FileExtensionLower(sourcePath);

            Result<String> fileName = pipeline::CopyIntoSources(context, sourcePath);
            if (!fileName.HasValue())
            {
                return Err(fileName.Error());
            }

            const StringView stem = pipeline::FileStemOf(fileName.Value().AsView());
            const TypeInfo& type = extension == u8"sss" ? UIThemeAsset::StaticType() : UIDocumentAsset::StaticType();
            foundation::content::Instance* instance =
                group.CreateInstance(pipeline::SingleAssetName(options, stem), type);
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            const foundation::vfs::SourcePath linked(fileName.Value().AsView());
            Status written;
            if (extension == u8"sss")
            {
                UIThemeAsset asset;
                asset.fileName = linked;
                written = instance->WriteObject(asset);
            }
            else
            {
                UIDocumentAsset asset;
                asset.fileName = linked;
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
        GlobalTypeRegistry().Register(UIDocumentAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<UIDocumentAsset>();
        GlobalTypeRegistry().Register(UIThemeAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<UIThemeAsset>();
    }

    // UIDocumentAsset/UIThemeAsset StaticType() are defined WITH reflected properties in
    // UIAssetImpl.cpp.

    /// File > New's UI creators (pipeline.registration composes every domain's).
    void RegisterUICreators(AssetCreatorRegistry& registry);
}
