/// Draconic::ModelImporter:load - load a model file + cook it in one call.
///
/// Convenience over the model loaders + the cook step: registers the glTF/FBX
/// loaders, loads `path` into a Model IR, then cooks it into `db`, yielding the
/// ImportedModel manifest. This is the clean seam an app (or, later, the editor)
/// drives; it keeps the loader dependency inside the importer library.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.modelimporter:load;

import draconic.foundation;
import draconic.model;
import draconic.model.io;
import draconic.model.gltf;
import draconic.model.fbx;
import draconic.content;
import :cook;

using namespace draconic::foundation;
namespace model = draconic::model;
namespace content = draconic::content;

export namespace draconic::modelimporter
{

    // Load a glTF/GLB/FBX/OBJ file and cook it into `db`. `prefix` namespaces the created
    // content instances. Returns the load result (Ok on success); on a load failure the
    // cook is skipped. On Ok, `outModelGuid` is the cooked manifest (ModelResource) Guid to
    // Bind at runtime - it pulls in the model's meshes via dependency edges.
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

} // namespace draconic::modelimporter
