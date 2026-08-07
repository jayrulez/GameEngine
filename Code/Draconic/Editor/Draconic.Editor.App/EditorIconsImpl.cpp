// Draconic::EditorApp - :editor_icons partition.
//
// Hand-authored editor icon set: inline SVG strings materialized ONCE into shared
// ui::SVGDrawable instances (the VG/SVG stack renders them crisp at any size). Ported from
// Sedulous.Editor/EditorIcons.bf - same glyphs, adapted to this engine's asset types. Inline
// strings are deliberate at this stage: editor chrome versioned with the code, no VFS/pipeline
// coupling, works on a fresh checkout; external icon files can come with full themes later
// (the drawables don't care where the string came from).
//
// Lifetime: the application calls Initialize() at startup and Shutdown() at teardown (explicit,
// deterministic - no static-destruction-order games with allocators). Access via
// EditorIcons::Get(); drawables are null before Initialize.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.editor.app;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;

namespace draconic::editor::app
{
    EditorIcons& EditorIcons::Get()
    {
        static EditorIcons instance;
        return instance;
    }

    void EditorIcons::Initialize()
    {
        if (m_initialized)
        {
            return;
        }
        m_initialized = true;
        translate = ui::BakedSVGDrawable::FromString(kTranslate);
        rotate = ui::BakedSVGDrawable::FromString(kRotate);
        scale = ui::BakedSVGDrawable::FromString(kScale);
        worldSpace = ui::BakedSVGDrawable::FromString(kWorldSpace);
        localSpace = ui::BakedSVGDrawable::FromString(kLocalSpace);
        grid = ui::BakedSVGDrawable::FromString(kGrid);
        scene = ui::BakedSVGDrawable::FromString(kScene);
        prefab = ui::BakedSVGDrawable::FromString(kPrefab);
        mesh = ui::BakedSVGDrawable::FromString(kMesh);
        skinnedMesh = ui::BakedSVGDrawable::FromString(kSkinnedMesh);
        material = ui::BakedSVGDrawable::FromString(kMaterial);
        texture = ui::BakedSVGDrawable::FromString(kTexture);
        particleFx = ui::BakedSVGDrawable::FromString(kParticleFx);
        animation = ui::BakedSVGDrawable::FromString(kAnimation);
        animGraph = ui::BakedSVGDrawable::FromString(kAnimGraph);
        skeleton = ui::BakedSVGDrawable::FromString(kSkeleton);
        folder = ui::BakedSVGDrawable::FromString(kFolder);
        unknown = ui::BakedSVGDrawable::FromString(kUnknown);
        close = ui::BakedSVGDrawable::FromString(kClose);
        add = ui::BakedSVGDrawable::FromString(kAdd);
        remove = ui::BakedSVGDrawable::FromString(kRemove);
        moveUp = ui::BakedSVGDrawable::FromString(kMoveUp);
        moveDown = ui::BakedSVGDrawable::FromString(kMoveDown);
        copy = ui::BakedSVGDrawable::FromString(kCopy);
    }

    void EditorIcons::Shutdown()
    {
        translate = nullptr;
        rotate = nullptr;
        scale = nullptr;
        worldSpace = nullptr;
        localSpace = nullptr;
        grid = nullptr;
        scene = nullptr;
        prefab = nullptr;
        mesh = nullptr;
        skinnedMesh = nullptr;
        material = nullptr;
        texture = nullptr;
        particleFx = nullptr;
        animation = nullptr;
        animGraph = nullptr;
        skeleton = nullptr;
        folder = nullptr;
        unknown = nullptr;
        add = nullptr;
        remove = nullptr;
        moveUp = nullptr;
        moveDown = nullptr;
        copy = nullptr;
        m_initialized = false;
    }

    Array<ui::BakedSVGDrawable*> EditorIcons::Bakeable() const
{
    Array<ui::BakedSVGDrawable*> icons;
    const RefPtr<ui::BakedSVGDrawable>* all[] = {
        &translate, &rotate,     &scale,    &worldSpace, &localSpace, &grid,   &scene,
        &prefab,    &mesh,       &skinnedMesh, &material, &texture,   &particleFx,
        &animation, &animGraph,  &skeleton, &folder,     &unknown,    &close,
        &add,       &remove,     &moveUp,   &moveDown,   &copy};
    for (const RefPtr<ui::BakedSVGDrawable>* icon : all)
    {
        if (icon->Get() != nullptr)
        {
            icons.PushBack(icon->Get());
        }
    }
    return icons;
}

ui::SVGDrawable* EditorIcons::ForAssetType(StringView typeName) const
    {
        if (typeName == u8"SceneDocument")
        {
            return scene.Get();
        }
        if (typeName == u8"ModelManifestAsset")
        {
            return prefab.Get();
        }
        if (typeName == u8"StaticMeshAsset")
        {
            return mesh.Get();
        }
        if (typeName == u8"SkinnedMeshAsset")
        {
            return skinnedMesh.Get();
        }
        if (typeName == u8"MaterialAsset")
        {
            return material.Get();
        }
        if (typeName == u8"TextureAsset")
        {
            return texture.Get();
        }
        if (typeName == u8"ImageAsset")
        {
            return texture.Get();
        }
        if (typeName == u8"ParticleEffectAsset")
        {
            return particleFx.Get();
        }
        if (typeName == u8"AnimationClipAsset")
        {
            return animation.Get();
        }
        if (typeName == u8"AnimationGraphAsset")
        {
            return animGraph.Get();
        }
        if (typeName == u8"SkeletonAsset")
        {
            return skeleton.Get();
        }
        return unknown.Get();
    }
}
