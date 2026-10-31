// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter:load - load a model file and cook it in one call.
///
/// Convenience over the model loaders and the cook step: registers the glTF and FBX
/// loaders, loads `path` into a Model IR, then cooks it into `db`, yielding the
/// ImportedModel manifest. An app drives import through this, which keeps the loader
/// dependency inside the importer library.

module;
#include "Core/Prelude.h"

export module modelimporter:load;

import foundation.core;
import foundation.model;
import foundation.model.io;
import foundation.model.gltf;
import foundation.model.fbx;
import foundation.content;
import :cook;

using namespace foundation::core;
namespace model = foundation::model;
namespace content = foundation::content;

export namespace pipeline
{

    // Load a glTF, GLB, FBX or OBJ file and cook it into `db`. `prefix` namespaces the created
    // content instances. Returns the load result (Ok on success); on a load failure the
    // cook is skipped. On Ok, `outModelGuid` is the cooked manifest (ModelResource) Guid to
    // Bind at runtime; it pulls in the model's meshes through dependency edges.
    [[nodiscard]] inline model::ModelLoadResult LoadAndCook(StringView path,
                                                            content::ContentDatabase& db,
                                                            StringView prefix, Guid& outModelGuid)
    {
        model::gltf::GltfLoader gltf;
        model::fbx::FbxLoader fbx;
        model::io::registerLoader(&gltf);
        model::io::registerLoader(&fbx);

        model::Model model;
        const model::ModelLoadResult r = model::io::loadModel(path, model);

        model::io::unregisterLoader(&fbx);
        model::io::unregisterLoader(&gltf);

        if (r != model::ModelLoadResult::Ok)
        {
            return r;
        }

        const Status s = CookModel(model, db, prefix, outModelGuid);
        return s.IsOk() ? model::ModelLoadResult::Ok : model::ModelLoadResult::InvalidData;
    }

} // namespace pipeline
