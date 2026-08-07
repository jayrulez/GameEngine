/// Draconic::RenderSubsystem - the `:extract` partition.
///
/// Extraction: read a Scene's render components into a render::ExtractedScene (world-space
/// RenderData) + a render::ViewCamera, both pushed to the (scene-agnostic) renderer. This is
/// the one-way seam - this layer depends on both draconic.scene and draconic.render; the renderer
/// depends on neither. Run after the scene's transforms are current (the tick).
///
/// These are the providers in the design's terms (§5): a MeshComponent provider and the
/// camera reader. As more component types land (lights, probes), each gets its own provider
/// writing its own RenderData category into the snapshot.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.render:extract;

import draconic.foundation;
import draconic.scene;
import draconic.render;    // ExtractedScene / MeshRenderData / ViewCamera / categories
import draconic.materials; // BlendMode (category mapping)
import draconic.geometry;  // StaticMesh::bounds (world bounding sphere for shadow-caster culling)
import draconic.texture.resource; // texture::Texture (cooked product behind sprite/decal refs)
import :components;

using namespace draconic::foundation;

export namespace draconic::render
{

    // Packs an entity handle into the opaque MeshRenderData::entityId (for pick; opaque to core).
    [[nodiscard]] u64 PackEntity(scene::EntityHandle e) noexcept;

    // Maps a material's blend preset to a render category (the dispatch + sort key).
    [[nodiscard]] RenderCategory CategoryForMaterial(const materials::Material* m) noexcept;

    // Below this many mesh components, parallel extraction's overhead isn't worth it - extract
    // serially. (Tuned conservatively; the win is at thousands of renderables.)
    inline constexpr u32 kParallelExtractThreshold = 256;

    // World-space bounding-sphere radius of a local AABB under a transform: the diagonal half-extent
    // scaled by the largest axis scale (basis-row length, row-vector convention) - conservative but
    // cheap. Used for sphere-vs-light culling of shadow casters (phase 5.4).
    [[nodiscard]] f32 WorldBoundsRadius(const AABB& local, const Float4x4& world);

    // Fill one MeshRenderData from a component (a pure read of precomputed transforms + borrowed
    // resource pointers - safe to call concurrently across components after UpdateTransforms).
    void FillMeshRenderData(scene::Scene& scene, MeshComponent& mc, scene::EntityHandle e,
                            MeshRenderData& rd);

    // Fills `out` with one MeshRenderData per visible MeshComponent in `scene`, allocating from
    // `out`'s own arena. Serial. Assumes transforms are current; `out` should be Reset beforehand.
    void ExtractSceneInto(scene::Scene& scene, ExtractedScene& out);

    // Same, but extraction is parallelized across the global JobSystem when present + worthwhile:
    // each worker fills its own RenderContext arena + item list (no contention), then a
    // single-threaded merge gathers them into `out`. Falls back to serial (into ctx slot 0) when
    // the job system is absent or the scene is small. `out` is Reset; `ctx` arenas accumulate
    // across the frame (the caller BeginFrame's it once per frame).
    void ExtractSceneInto(scene::Scene& scene, ExtractedScene& out, RenderContext& ctx);

    // Fills `out` with ONE MultiMeshRenderData per visible InstancedMeshComponent - the set, not the
    // instances (per-frame CPU is O(1) in the instance count; the renderer holds the transforms in a
    // persistent GPU buffer keyed by the entity). The merged world bounds (center + sphere radius) are
    // recomputed ONLY when the component's version changed (a static set recomputes once), so the whole
    // set culls as a single AABB. Run after transforms are current, like the other extractors.
    void ExtractInstancedMeshesInto(scene::Scene& scene, ExtractedScene& out);

    // Fills `out` with one SpriteRenderData per visible SpriteComponent (serial - sprites are few). Stamps
    // the sprite renderer's dispatch id so emission routes them to the SpriteRenderer, and category
    // Transparent so they sort back-to-front and ride the blended forward pass alongside transparent meshes.
    void ExtractSpritesInto(scene::Scene& scene, ExtractedScene& out, u16 spriteRendererId);

    // Fills `out`'s decal list from the scene's DecalComponents (serial - decals are few). The box world
    // bakes the component `size` as an extra scale on top of the entity transform (Scale then world, row-
    // vector order), so the entity's rotation orients the projection axis and `size` sets the box extents.
    void ExtractDecalsInto(scene::Scene& scene, ExtractedScene& out);

    // Reads the scene's primary camera into `out` (view = inverse world; projection from its
    // fields). When `outClear` is given, also writes the camera's clear color. Returns false if
    // no primary CameraComponent exists.
    [[nodiscard]] bool ExtractPrimaryCamera(scene::Scene& scene, ViewCamera& out,
                                            Color* outClear = nullptr);

    // Packs every enabled LightComponent into `out` as a GpuLight shading input (world position +
    // forward direction from the entity's transform). Assumes transforms are current. The FIRST enabled
    // directional light flagged castsShadows becomes the scene's shadow caster (its cascades are fit to
    // the camera frustum later, in RenderFrame).
    void ExtractLightsInto(scene::Scene& scene, ExtractedScene& out);

    // Reads the scene's environment (ambient) into the snapshot. Defaults to the snapshot's own dim
    // ambient when the scene has no EnvironmentSystem. Premultiplies color × intensity.
    void ExtractEnvironmentInto(scene::Scene& scene, ExtractedScene& out);

    // Reads the scene's reflection probes into the snapshot. Each enabled probe becomes a render::ReflectionProbe
    // with its world-space capture center + box half-extents. Capped at kMaxReflectionProbes. The
    // ReflectionProbeSystem consumes the list (capture + prefilter + froxel assignment) at frame time.
    void ExtractReflectionProbesInto(scene::Scene& scene, ExtractedScene& out);

} // namespace draconic::render
