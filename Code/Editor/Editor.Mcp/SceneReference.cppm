// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :scene_reference partition
//
// The scene format reference an agent authors scenes from (Documentation/Specs/
// scene-format-reference.md): two generated documents, built by the running host from its own
// registrations so they cannot describe a different build.
//   * SceneExample.scene.xml - a real SaveScene of a scene composed in code: every registered
//     component at its defaults on an entity of its own, every settings block, a parent and
//     child, a script behaviour carrying one override of each property kind. Exactly what
//     scene_write accepts, because it is what the engine writes.
//   * SceneSchema.json - what each Serialize body wrote, recorded through the real write path
//     (SchemaRecorder): keys, kinds and defaults in wire order, the data-version chain, joined to
//     reflection for enum names, attributes and reference targets.
// Both are deterministic: seeded guids, composition order, no stamps. The hosts serve them as
// `docs://generated/*` beside the shipping docs, and `component_schema` answers one entry.

module;
#include "Core/Prelude.h"

export module editor.mcp:scene_reference;

import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;

using namespace foundation::core;

export namespace editor::mcp
{
    inline constexpr StringView kSceneExampleUri = u8"docs://generated/SceneExample.scene.xml";
    inline constexpr StringView kSceneSchemaUri = u8"docs://generated/SceneSchema.json";

    struct SceneReference
    {
        String exampleXml; ///< the composed example scene, as SaveScene writes it
        String schemaJson; ///< `schema`, pretty-printed: the bytes the resource serves
        foundation::json::JsonValue schema;
    };

    /// Generates both documents from the engine composition (every manager and settings system,
    /// every factory description) and `builders` (cooked form -> asset type). Reads declarations
    /// only, so every host produces the same bytes for the same code.
    [[nodiscard]] SceneReference
    GenerateSceneReference(IAllocator& allocator, const pipeline::BuilderRegistry& builders);

    /// The source asset types a resource reference to `product` takes: the runtime type's factory
    /// description in the engine composition gives its cooked forms, and the builder producing
    /// each gives its asset type (a texture's and a render texture's for a Texture). A cooked form
    /// itself (what nothing makes at run time) takes every asset whose product is one or derives
    /// from one (a StaticMeshSource: the static and the skinned mesh). Declarations only, nothing
    /// constructed. Empty when nothing in this composition makes the product.
    [[nodiscard]] Array<const TypeInfo*> SourceAssetTypesFor(const pipeline::BuilderRegistry& builders,
                                                             const TypeInfo& product);

    /// The schema entry for `name`: a component by wire name (a record's `type`) or reflected type
    /// name, or a settings block by system id or type name; ASCII case folded. Null when none.
    [[nodiscard]] foundation::json::JsonValue
    FindSchemaEntry(const foundation::json::JsonValue& schema, StringView name);

    /// `docs://generated/SceneExample.scene.xml` (application/xml) and
    /// `docs://generated/SceneSchema.json` (application/json), serving `reference`'s bytes.
    void RegisterSceneReferenceResources(foundation::mcp::McpServer& server,
                                         const SceneReference& reference);

    /// `component_schema`: one component's or settings block's schema entry by name.
    void RegisterComponentSchemaTool(foundation::mcp::McpServer& server,
                                     const SceneReference& reference);
}
