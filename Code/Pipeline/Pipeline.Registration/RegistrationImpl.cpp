// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Registration - implementation unit.
//
// The wide fan-in lives here, in one TU, keeping the interface BMI lean for the hosts. This is
// the one place the engine's full builder, importer and type set is written down; hosts call the
// entry points instead of restating the list.

module;
#include "Core/Prelude.h"

module pipeline.registration;

import foundation.core;
import foundation.script.facades; // RegisterScriptFacadeReflection (the script cooks' surface)
import foundation.content;
import foundation.animation;
import foundation.particles;
import foundation.scene;
import foundation.scene.resource;
import pipeline.core;
import pipeline.importer;
import texture.pipeline;
import fonts.pipeline;
import image.pipeline;
import heightfield.pipeline;
import foundation.heightfield.resource;
import terrain.pipeline;
import foundation.terrain.resource;
import vegetation.pipeline;
import foundation.vegetation.resource;
import foundation.image.resource;
import geometry.pipeline;
import animation.pipeline;
import propertyanimation.pipeline;
import materials.pipeline;
import shaders.pipeline;
import particles.pipeline;
import foundation.input;
import foundation.input.resource;
import input.pipeline;
import modelimporter;
import foundation.physics;
import foundation.physics.resource;
import physics.pipeline;
import render.pipeline;
import engine.render; // the render profiles' cooked records and products
import foundation.navigation.resource;
import navigation.pipeline;
import foundation.vg.resource;
import vg.pipeline;
import foundation.gui.resources; // the GUI's documents and sheets
import gui.pipeline;
import foundation.audio;
import foundation.audio.resource;
import audio.pipeline;
import foundation.script;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
import script.angelscript.pipeline;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
import script.luau.pipeline;
#endif
import foundation.script.resource;
import script.pipeline;
import scene.pipeline;

using namespace foundation::core;

namespace pipeline
{
    namespace
    {
        template <typename T>
        void AddBuilder(BuilderRegistry& registry)
        {
            registry.Register(UniquePtr<IAssetBuilder>(registry.Allocator().New<T>(),
                                                       registry.Allocator()));
        }

        template <typename T>
        void AddImporter(ImporterRegistry& registry)
        {
            registry.Register(UniquePtr<IFileImporter>(registry.Allocator().New<T>(),
                                                       registry.Allocator()));
        }
    }

    void RegisterPipelineTypes()
    {
        // Everything a builder registers at build time goes here first, on the caller's thread,
        // so the parallel cook phase never performs a first-time registration: the registries
        // lock their writes, but a reader iterating the type list (the script cooks) while another
        // worker first-registers is still a race. After this, the builders' own calls are no-ops.
        foundation::core::RegisterCoreTypes();
        foundation::script::RegisterScriptFacadeReflection();

        RegisterAssetReflection(); // base Asset::fileName and SourcePath
        RegisterTextureAsset();
        RegisterFontAsset(); // asset and FontResource product
        RegisterImageAsset();
        RegisterHeightfieldAsset();
        foundation::heightfield::RegisterHeightfieldResourceTypes(); // Heightfield product and source
        RegisterTerrainAsset();
        foundation::terrain::RegisterTerrainResourceTypes(); // Terrain product and source
        RegisterSplatmapAsset();
        foundation::terrain::RegisterSplatmapResourceTypes(); // Splatmap product and source
        RegisterVegetationMaskAsset();
        foundation::vegetation::RegisterVegetationMaskResourceTypes(); // Vegetation mask product and source
        RegisterMeshAssets();
        RegisterAnimationAssets();
        RegisterPropertyAnimationAssets();
        RegisterMaterialAsset();
        RegisterShaderAsset();
        RegisterParticleEffectAsset();
        RegisterInputMapAsset();
        RegisterModelManifestAsset();
        // Product/resource types: ReadObject constructs cooked products by type name, so the
        // runtime-facing types must be registered too (meshes/materials/textures/animation/
        // manifest via the model-importer helper, plus the image resource).
        foundation::model::RegisterModelResourceTypes();
        foundation::image::RegisterImageResource();
        RegisterPhysicsAssets();
        foundation::physics::RegisterPhysicsResource();
        RegisterRenderProfileAssets();
        engine::render::RegisterRenderProfileResources();
        RegisterNavigationZoneAsset();
        foundation::navigation::RegisterNavigationResource();
        RegisterVGAssets();
        foundation::vg::RegisterVGResource();
        RegisterGuiAssets();
        foundation::gui::RegisterGuiResource();
        RegisterAudioAssets();
        foundation::audio::RegisterAudioResource();
        RegisterScriptAssets();
        foundation::script::RegisterScriptResource();
        // The script builder resolves a per-language cook through the registry; registering the
        // backends and their cooks, for both languages, is the composition root's job.
#ifdef OPTION_HAS_ANGELSCRIPT
        foundation::script::angelscript::RegisterAngelScriptBackend();
        RegisterAngelScriptScriptCook();
#endif
#ifdef OPTION_HAS_LUAU
        foundation::script::RegisterLuauScriptBackend();
        RegisterLuauScriptCook();
#endif
        // Scenes are packed and read as SceneDocument (export staging and a headless scene cook):
        // register the type and its serializer so ReadObject/WriteObject round-trip them.
        GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
        // The same for prefabs (a PrefabDocument primary and the referenced scene stream), which the
        // registry otherwise refuses to read back as an unknown type.
        GlobalTypeRegistry().Register(foundation::scene::PrefabDocument::StaticType());
        RegisterSerializable<foundation::scene::PrefabDocument>();
        RegisterSerializable<foundation::scene::SceneDocument>();
    }

    void RegisterAllBuilders(BuilderRegistry& registry, StringView dataRoot)
    {
        AddBuilder<TextureAssetBuilder>(registry);
        AddBuilder<RenderTextureAssetBuilder>(registry);
        AddBuilder<FontAssetBuilder>(registry);
        AddBuilder<ImageAssetBuilder>(registry);
        AddBuilder<HeightfieldAssetBuilder>(registry);
        AddBuilder<TerrainAssetBuilder>(registry);
        AddBuilder<SplatmapAssetBuilder>(registry);
        AddBuilder<VegetationMaskAssetBuilder>(registry);
        AddBuilder<StaticMeshAssetBuilder>(registry);
        AddBuilder<SkinnedMeshAssetBuilder>(registry);
        AddBuilder<SkeletonAssetBuilder>(registry);
        AddBuilder<AnimationClipAssetBuilder>(registry);
        AddBuilder<AnimationGraphAssetBuilder>(registry);
        AddBuilder<PropertyAnimationClipAssetBuilder>(registry);
        AddBuilder<MaterialAssetBuilder>(registry);
        registry.Register(UniquePtr<IAssetBuilder>(
            registry.Allocator().New<ShaderAssetBuilder>(dataRoot), registry.Allocator()));
        AddBuilder<ParticleEffectAssetBuilder>(registry);
        AddBuilder<InputMapAssetBuilder>(registry);
        AddBuilder<ModelManifestAssetBuilder>(registry);
        AddBuilder<CollisionShapeAssetBuilder>(registry);
        AddBuilder<PhysicalMaterialAssetBuilder>(registry);
        AddBuilder<EnvironmentProfileAssetBuilder>(registry);
        AddBuilder<PostProcessProfileAssetBuilder>(registry);
        AddBuilder<NavigationZoneAssetBuilder>(registry);
        AddBuilder<VectorImageAssetBuilder>(registry);
        AddBuilder<GuiDocumentAssetBuilder>(registry);
        AddBuilder<GuiStyleSheetAssetBuilder>(registry);
        AddBuilder<AudioClipAssetBuilder>(registry);
        AddBuilder<AudioBusLayoutAssetBuilder>(registry);
        AddBuilder<SoundCueAssetBuilder>(registry);
        AddBuilder<ScriptClassAssetBuilder>(registry);
    }

    void RegisterAllImporters(ImporterRegistry& registry)
    {
        AddImporter<TextureFileImporter>(registry);
        AddImporter<ModelFileImporter>(registry);
        AddImporter<VectorImageFileImporter>(registry);
        AddImporter<GuiFileImporter>(registry);
        AddImporter<AudioFileImporter>(registry);
        AddImporter<ScriptFileImporter>(registry);
        AddImporter<ShaderFileImporter>(registry);
        AddImporter<FontAssetImporter>(registry);
        AddImporter<ImageFileImporter>(registry);
        AddImporter<HeightfieldFileImporter>(registry);
        AddImporter<SplatmapFileImporter>(registry);
        AddImporter<VegetationMaskFileImporter>(registry);
    }

    void AfterImport(IAllocator& allocator, foundation::content::Instance& primary,
                     const ImportOptions* options)
    {
        ModelPrefabResult prefab;
        ModelPrefabResult generatedScene;
        (void)GenerateForImport(allocator, primary, options, prefab, generatedScene);
    }

    usize RegisterAllCreators(AssetCreatorRegistry& registry)
    {
        RegisterSceneCreators(registry);
        RegisterInputCreators(registry);
        RegisterTextureCreators(registry);
        RegisterNavigationCreators(registry);
        RegisterParticleCreators(registry);
        RegisterGeometryCreators(registry);
        RegisterMaterialCreators(registry);
        RegisterShaderCreators(registry);
        RegisterPhysicsCreators(registry);
        RegisterRenderCreators(registry);
        RegisterAudioCreators(registry);
        RegisterGuiCreators(registry);
        RegisterHeightfieldCreators(registry);
        RegisterTerrainCreators(registry);
        RegisterVegetationCreators(registry);
        RegisterAnimationCreators(registry);
        RegisterPropertyAnimationCreators(registry);
        return RegisterScriptCreators(registry);
    }
}
