// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :editor_icons partition.
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
#include "Core/Prelude.h"
#include <initializer_list>

module editor.app;

import foundation.core;
import foundation.ui;

using namespace foundation::core;
namespace ui = foundation::ui;

namespace editor::app
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
        translate = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kTranslate);
        rotate = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kRotate);
        scale = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kScale);
        worldSpace = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kWorldSpace);
        localSpace = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kLocalSpace);
        grid = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kGrid);
        scene = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kScene);
        prefab = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kPrefab);
        mesh = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kMesh);
        skinnedMesh = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kSkinnedMesh);
        material = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kMaterial);
        texture = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kTexture);
        particleFx = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kParticleFx);
        animation = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kAnimation);
        animGraph = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kAnimGraph);
        skeleton = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kSkeleton);
        folder = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kFolder);
        unknown = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kUnknown);
        viewList = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kViewList);
        viewGrid = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kViewGrid);
        close = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kClose);
        search = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kSearch);
        add = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kAdd);
        remove = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kRemove);
        moveUp = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kMoveUp);
        moveDown = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kMoveDown);
        copy = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kCopy);
        edit = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kEdit);
        brushRaise = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kBrushRaise);
        brushLower = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kBrushLower);
        brushSmooth = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kBrushSmooth);
        brushFlatten = ui::BakedSVGDrawable::FromString(editor::EditorRootAllocator(), kBrushFlatten);
    }

    // The one slot table: Shutdown and Bakeable both walk it, so a slot added to the class
    // cannot be missed by either (the `close` icon was missing from a hand-written Shutdown
    // list and was released at process exit, after the allocator on main's stack was gone -
    // ASan's stack-use-after-return, 2026-09-21).
    Array<RefPtr<ui::BakedSVGDrawable>*> EditorIcons::Slots()
    {
        Array<RefPtr<ui::BakedSVGDrawable>*> slots;
        for (RefPtr<ui::BakedSVGDrawable>* slot :
             {&translate, &rotate,      &scale,       &worldSpace, &localSpace,  &grid,
              &scene,     &prefab,      &mesh,        &skinnedMesh, &material,   &texture,
              &particleFx, &animation,  &animGraph,   &skeleton,   &folder,      &unknown,
              &viewList,  &viewGrid,
              &close,     &search,     &add,         &remove,      &moveUp,     &moveDown,    &copy,
              &edit,      &brushRaise,  &brushLower,  &brushSmooth, &brushFlatten})
        {
            slots.PushBack(slot);
        }
        return slots;
    }

    void EditorIcons::Shutdown()
    {
        for (RefPtr<ui::BakedSVGDrawable>* slot : Slots())
        {
            *slot = nullptr;
        }
        m_initialized = false;
    }

    Array<ui::BakedSVGDrawable*> EditorIcons::Bakeable() const
    {
        Array<ui::BakedSVGDrawable*> icons;
        for (RefPtr<ui::BakedSVGDrawable>* slot : const_cast<EditorIcons*>(this)->Slots())
        {
            if (slot->Get() != nullptr)
            {
                icons.PushBack(slot->Get());
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
