// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.project - the starter content a new project is seeded with, the one function both the
// editor's New Project and the MCP project_create run (a project made over MCP had none, so its
// exported game showed no text: the engine's built-in font is not in a dist).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import fonts.pipeline;
import texture.pipeline;

using namespace foundation::core;

namespace editor
{
    namespace
    {
        // The group `name` under the project's root group, made if absent.
        foundation::content::Group* GroupNamed(foundation::content::Group* root, StringView name)
        {
            foundation::content::Group* group = root->GetGroup(name);
            return (group != nullptr) ? group : root->CreateGroup(name);
        }
    }

    void SeedStarterContent(IAllocator& allocator, EditorProject& project,
                            const pipeline::AssetCreatorRegistry& creators, StringView dataRoot,
                            bool allPrimitives)
    {
        foundation::content::Group* root = project.SourceDb().RootGroup();
        const pipeline::ImportContext import{allocator, project.SourcesRoot()};

        // 1) The UI font: Roboto as a font asset, set as the manifest's default (the font a
        //    shipped game binds; the built-in one is not in a dist). A distance-field font, so a
        //    72 px title and a 14 px caption both draw clean from its one bake.
        {
            const String source =
                foundation::vfs::DataPath(dataRoot, u8"Engine/Fonts/roboto/Roboto-Regular.ttf");
            Result<String> copied = pipeline::CopyIntoSources(import, source.AsView());
            if (copied.HasValue())
            {
                if (foundation::content::Instance* instance = GroupNamed(root, u8"Fonts")->CreateInstance(
                        u8"Roboto", pipeline::FontAsset::StaticType()))
                {
                    pipeline::FontAsset asset;
                    asset.fileName = foundation::vfs::SourcePath(copied.Value().AsView());
                    asset.family = String(u8"Roboto");
                    asset.mode = pipeline::FontBakeMode::DistanceField;
                    if (instance->WriteObject(asset).IsOk())
                    {
                        project.Settings().defaultUiFontId = instance->Id();
                    }
                }
            }
            else
            {
                LOG_WARNING(u8"Editor", u8"starter font missing ({}) - new project has no default UI font",
                            source);
            }
        }

        // 2) The default sky: Poly Haven's Kloofendal 48d Partly Cloudy (Pure Sky) at 1K, CC0 (its
        //    licence beside it), an equirectangular skybox texture. A 1024 x 512 HDR of 1.4 MB: the
        //    4096 x 2048 BlueSky (10 MB, nearly 3 s to cook) was more than a starting sky needs.
        {
            const String source = foundation::vfs::DataPath(
                dataRoot, u8"Templates/kloofendal_48d_partly_cloudy_puresky/kloofendal_48d_partly_cloudy_puresky_1k.hdr");
            Result<String> copied = pipeline::CopyIntoSources(import, source.AsView());
            if (copied.HasValue())
            {
                if (foundation::content::Instance* instance = GroupNamed(root, u8"Environment")->CreateInstance(
                        u8"Sky", pipeline::TextureAsset::StaticType()))
                {
                    pipeline::TextureAsset asset;
                    asset.fileName = foundation::vfs::SourcePath(copied.Value().AsView());
                    asset.SetupForEquirectangularSkybox();
                    (void)instance->WriteObject(asset);
                }
            }
        }

        // 3) Primitive meshes: the creators File > New > Primitives runs (the cube, sphere and
        //    plane; every one with allPrimitives).
        const String sourcesRoot(project.SourcesRoot().AsView());
        for (const pipeline::AssetCreator& creator : creators.All())
        {
            if (creator.category.AsView() != StringView(u8"Primitives"))
            {
                continue;
            }
            const StringView label = creator.label.AsView();
            const bool starter = label == StringView(u8"Cube") || label == StringView(u8"Sphere") ||
                                 label == StringView(u8"Plane");
            if (starter || allPrimitives)
            {
                (void)creator.Create(nullptr, root, sourcesRoot.AsView());
            }
        }

        LOG_INFO(u8"Editor", u8"starter content seeded (font/sky/primitives{})",
                 allPrimitives ? u8", all primitives" : u8"");
    }
}
