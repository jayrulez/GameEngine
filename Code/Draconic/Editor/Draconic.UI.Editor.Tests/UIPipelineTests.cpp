// Game-UI asset pipeline: author document/theme -> VALIDATING cook -> load the products
// through the factories. Bad payloads must FAIL the cook (validation is the point).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include <initializer_list>

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.ui;
import draconic.ui.resource;
import draconic.ui.editor;

using namespace draconic::foundation;
using namespace draconic::resource;
using namespace draconic::ui;
namespace content = draconic::content;

namespace
{
    void RemoveTree(StringView dir)
    {
        for (const utf8char* f : {u8"menu.rasset", u8"theme.rasset"})
        {
            String path(dir);
            path.Append(u8"/");
            path.Append(f);
            FileDelete(path.AsView());
        }
        RemoveDirectory(dir);
    }
}

TEST_CASE("ui.pipeline: document + theme cook (validated) and load as products")
{
    RegisterUIResource();
    RegisterUIAssets();
    RemoveTree(u8"draconic_uipipe_db");
    draconic::vfs::NativeFileSystem outMount(u8"draconic_uipipe_db");
    content::ContentDatabase outDb(outMount, BinarySerializerFactory(), u8".rasset");

    // Document round-trip.
    auto* docInstance = outDb.RootGroup()->CreateInstance(u8"menu", UIDocumentSource::StaticType());
    {
        UIDocumentAsset asset;
        asset.markup = String(kUIDocumentStarter);
        UIDocumentAssetBuilder builder;
        draconic::editor::AssetBuildContext ctx;
        ctx.output = docInstance;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }
    // Theme round-trip.
    auto* themeInstance = outDb.RootGroup()->CreateInstance(u8"theme", UIThemeSource::StaticType());
    {
        UIThemeAsset asset;
        asset.stylesheet = String(kUIThemeStarter);
        UIThemeAssetBuilder builder;
        draconic::editor::AssetBuildContext ctx;
        ctx.output = themeInstance;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    UIDocumentFactory documentFactory;
    UIThemeFactory themeFactory;
    ResourceManager manager(outDb);
    manager.AddFactory(&documentFactory);
    manager.AddFactory(&themeFactory);
    Proxy<UIDocument> document = manager.Bind<UIDocument>(docInstance->Id());
    REQUIRE(document);
    CHECK(document->markup.AsView() == kUIDocumentStarter);
    Proxy<UITheme> theme = manager.Bind<UITheme>(themeInstance->Id());
    REQUIRE(theme);
    CHECK(!theme->stylesheet.IsEmpty());

    // The cooked markup actually instantiates a view tree with addressable ids.
    MarkupLoader::Initialize();
    RefPtr<View> tree = MarkupLoader::LoadFromString(document->markup.AsView());
    REQUIRE(tree.Get() != nullptr);
    auto* group = Cast<ViewGroup>(tree.Get());
    REQUIRE(group != nullptr);
    CHECK(group->FindByName(u8"ok-btn") != nullptr);

    RemoveTree(u8"draconic_uipipe_db");
}

TEST_CASE("ui.pipeline: malformed payloads FAIL the cook")
{
    RegisterUIResource();
    RegisterUIAssets();
    RemoveTree(u8"draconic_uipipe_bad_db");
    draconic::vfs::NativeFileSystem outMount(u8"draconic_uipipe_bad_db");
    content::ContentDatabase outDb(outMount, BinarySerializerFactory(), u8".rasset");
    auto* instance = outDb.RootGroup()->CreateInstance(u8"menu", UIDocumentSource::StaticType());

    draconic::editor::AssetBuildContext ctx;
    ctx.output = instance;

    UIDocumentAssetBuilder documents;
    UIDocumentAsset badXml;
    badXml.markup = String(u8"<FlexLayout><Label text=\"unclosed\"</FlexLayout>");
    CHECK_FALSE(documents.Build(badXml, ctx).IsOk());
    UIDocumentAsset unknownControl;
    unknownControl.markup = String(u8"<NotARealControl />");
    CHECK_FALSE(documents.Build(unknownControl, ctx).IsOk());
    UIDocumentAsset empty;
    CHECK_FALSE(documents.Build(empty, ctx).IsOk());

    UIThemeAssetBuilder themes;
    UIThemeAsset emptyTheme;
    CHECK_FALSE(themes.Build(emptyTheme, ctx).IsOk());

    RemoveTree(u8"draconic_uipipe_bad_db");
}

TEST_CASE("ui.pipeline: silent markup drops surface as cook warnings")
{
    MarkupLoader::Initialize();
    Array<String> warnings;
    RefPtr<View> tree = MarkupLoader::LoadFromString(
        u8"<Flex direction=\"vertical\">"
        u8"  <Label fontSize=\"20\" text=\"typo\"/>" // camelCase typo -> warning
        u8"  <NotARealControl/>"                     // unknown child -> warning (dropped)
        u8"  <Button id=\"ok\" text=\"fine\" height=\"40\"/>"
        u8"</Flex>",
        nullptr, &warnings);
    REQUIRE(tree.Get() != nullptr); // the tree still builds
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].AsView().StartsWith(u8"unknown attribute 'fontSize'"));
    CHECK(warnings[1].AsView().StartsWith(u8"unknown element <NotARealControl>"));

    // A clean document warns about nothing.
    warnings.Clear();
    RefPtr<View> clean = MarkupLoader::LoadFromString(kUIDocumentStarter, nullptr, &warnings);
    REQUIRE(clean.Get() != nullptr);
    CHECK(warnings.IsEmpty());
}
