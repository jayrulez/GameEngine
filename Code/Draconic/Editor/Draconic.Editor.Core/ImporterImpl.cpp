// Draconic::EditorCore - :importer partition.
//
// The file-import seam (asset-pipeline design §7): an OS file (drag-dropped onto the editor)
// becomes a SOURCE - the raw bytes copied into the project's Sources/ tree - plus a typed Asset
// instance in the content DB whose import settings point at it. Cooking then owns the
// source -> product path like any other asset (the imported file's content is part of the
// recipe hash).
//
// IFileImporter implementations live with their asset modules (texture/image/...) and are
// registered by the executable; routing is by lowercase extension. Several importers may claim
// one extension - v1 takes the FIRST match (the Sedulous-style chooser dialog is a later
// nicety; the registry API already exposes all matches).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.editor.core;

import draconic.foundation;
import draconic.content;
import :project;

using namespace draconic::foundation;

namespace draconic::editor
{
    RefPtr<Object> IFileImporter::PrepareOnWorker(StringView /*sourcePath*/) { return {}; }
    void ImporterRegistry::Register(UniquePtr<IFileImporter> importer)
    {
        if (importer)
        {
            m_importers.PushBack(Move(importer));
        }
    }

    IFileImporter* ImporterRegistry::FindFor(StringView extension) const
    {
        for (const UniquePtr<IFileImporter>& importer : m_importers)
        {
            if (importer->Accepts(extension))
            {
                return importer.Get();
            }
        }
        return nullptr;
    }
}
