// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A new shader from nothing (the Shader creator) and from a dropped file (ShaderFileImporter):
// each writes or stages the .hlsl in Sources/ and links it from a shader asset named like its
// instance, the name materials find it by. The creator's starter compiles, every variant of both
// stages, against the engine's shader library.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import foundation.shaders;
import shaders.pipeline;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace pipeline;

namespace
{
    bool HaveCompiler()
    {
        foundation::shaders::Compiler* compiler = nullptr;
        if (!foundation::shaders::createCompiler(foundation::shaders::CompilerDesc{}, compiler)
                 .IsOk() ||
            compiler == nullptr)
        {
            return false;
        }
        compiler->Destroy();
        return true;
    }

    // The shader asset `instance` holds.
    RefPtr<ISerializable> Read(foundation::content::Instance& instance)
    {
        return instance.ReadObject();
    }
}

TEST_CASE("shader creator: the starter compiles, every variant, against the engine's library")
{
    const String dataRoot = FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    const String library = DataPath(dataRoot.AsView(), u8"Engine/Shaders");
    ShaderCheckOptions options;
    options.shaderLibrary = library.AsView();
    const ShaderCheck check = CheckShaderSource(kShaderStarter, u8"Shader.hlsl", options);
    for (const ShaderDiagnostic& d : check.errors)
    {
        INFO(doctest::String(reinterpret_cast<const char*>(FormatShaderDiagnostic(d).CStr())));
        CHECK(false);
    }
    CHECK(check.IsOk());
    CHECK(check.block.properties.Size() == 6u);
    // WIND: the starter sways by its material's Wind group.
    CHECK(check.vertexMask == (foundation::shaders::ShaderFlags::Skinned | foundation::shaders::ShaderFlags::Instanced |
                               foundation::shaders::ShaderFlags::Wind));
}

TEST_CASE("shader creator: a new shader links its starter file and takes its instance's name")
{
    RegisterShaderAsset();
    constexpr StringView kDb = u8"scratch_shader_creator_db";
    constexpr StringView kSources = u8"scratch_shader_creator_src";
    (void)RemoveDirectoryRecursive(kDb);
    (void)RemoveDirectoryRecursive(kSources);
    REQUIRE(CreateDirectories(kDb));
    REQUIRE(CreateDirectories(kSources));
    {
        NativeFileSystem mount(kDb, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                                u8".rasset");
        AssetCreatorRegistry creators{DefaultAllocator()};
        RegisterShaderCreators(creators);
        const AssetCreator* creator = creators.FindByLabel(u8"Low-Level Shader");
        REQUIRE(creator != nullptr);
        CHECK(creator->category == u8"Materials");

        const String sourcesRoot = PathJoin(GetCurrentDirectory().AsView(), kSources);
        foundation::content::Instance* first =
            creator->Create(nullptr, db.RootGroup(), sourcesRoot.AsView(), u8"Lava");
        foundation::content::Instance* second =
            creator->Create(nullptr, db.RootGroup(), sourcesRoot.AsView(), u8"Lava");
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(db.RootGroup()->GetGroup(u8"Shaders") != nullptr); // its default folder

        RefPtr<ISerializable> object = Read(*second);
        const auto* asset = Cast<ShaderAsset>(object.Get());
        REQUIRE(asset != nullptr);
        // The second took a unique name, and its shader name and file follow it.
        CHECK(asset->name.AsView() == second->Name());
        CHECK(asset->name.AsView() != first->Name());
        String expectedFile(second->Name());
        expectedFile += u8".hlsl";
        CHECK(asset->fileName.View() == expectedFile.AsView());
        CHECK(asset->vertexEntryPoint == u8"VSMain");
        CHECK(FileExists(PathJoin(kSources, expectedFile.AsView()).AsView()));
    }
    (void)RemoveDirectoryRecursive(kDb);
    (void)RemoveDirectoryRecursive(kSources);
}

TEST_CASE("shader importer: a dropped .hlsl is staged into Sources and linked, named by its stem")
{
    RegisterShaderAsset();
    constexpr StringView kRoot = u8"scratch_shader_import";
    constexpr StringView kDb = u8"scratch_shader_import_db";
    const String sourcesRoot = PathJoin(kRoot, u8"Sources");
    (void)RemoveDirectoryRecursive(kRoot);
    (void)RemoveDirectoryRecursive(kDb);
    REQUIRE(CreateDirectories(sourcesRoot.AsView()));
    REQUIRE(CreateDirectories(kDb));
    const String loose = PathJoin(kRoot, u8"Hologram.hlsl");
    REQUIRE(WriteFile(loose.AsView(),
                      Span<const byte>(reinterpret_cast<const byte*>(kShaderStarter.Data()),
                                       kShaderStarter.Size()))
                .IsOk());
    {
        NativeFileSystem mount(kDb, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                                u8".rasset");
        ShaderFileImporter importer;
        CHECK(importer.Accepts(u8"hlsl"));
        CHECK_FALSE(importer.Accepts(u8"hlsli")); // a library include is not a shader
        ImportContext context{DefaultAllocator(), sourcesRoot.AsView()};
        Result<foundation::content::Instance*> imported =
            importer.Import(loose.AsView(), context, *db.RootGroup(), nullptr, nullptr, nullptr);
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value() != nullptr);
        RefPtr<ISerializable> object = Read(*imported.Value());
        const auto* asset = Cast<ShaderAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->name == u8"Hologram");
        CHECK(asset->fileName.View() == StringView(u8"Hologram.hlsl"));
        CHECK(FileExists(PathJoin(sourcesRoot.AsView(), u8"Hologram.hlsl").AsView()));
    }
    (void)RemoveDirectoryRecursive(kRoot);
    (void)RemoveDirectoryRecursive(kDb);
}

// The surface creators: Lit and Unlit Surface Shader beside Low-Level Shader, each starter a
// surface shader that compiles, every program and variant, against the engine's library.
TEST_CASE("shader creator: the surface starters compile as surface shaders")
{
    AssetCreatorRegistry creators{DefaultAllocator()};
    RegisterShaderCreators(creators);
    CHECK(creators.FindByLabel(u8"Lit Surface Shader") != nullptr);
    CHECK(creators.FindByLabel(u8"Unlit Surface Shader") != nullptr);
    CHECK(creators.FindByLabel(u8"Low-Level Shader") != nullptr);
    CHECK(creators.FindByLabel(u8"Shader") == nullptr); // no bare "Shader": each names its kind
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    const String library = DataPath(FindDataRoot().AsView(), u8"Engine/Shaders");
    ShaderCheckOptions options;
    options.shaderLibrary = library.AsView();
    const struct
    {
        StringView text;
        SurfaceModel model;
    } starters[2] = {{kLitSurfaceShaderStarter, SurfaceModel::Lit},
                     {kUnlitSurfaceShaderStarter, SurfaceModel::Unlit}};
    for (const auto& starter : starters)
    {
        const ShaderCheck check = CheckShaderSource(starter.text, u8"Starter.hlsl", options);
        for (const ShaderDiagnostic& d : check.errors)
        {
            INFO(doctest::String(reinterpret_cast<const char*>(FormatShaderDiagnostic(d).CStr())));
            CHECK(false);
        }
        CHECK(check.IsOk());
        CHECK(check.surface == starter.model);
        CHECK_FALSE(check.hasVertex); // the lava's Vertex() is a commented example
        CHECK(check.programs.Size() == 3u);
    }
}
