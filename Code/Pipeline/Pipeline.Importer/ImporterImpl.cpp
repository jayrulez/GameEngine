// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Importer - implementation unit.
//
// File import: an OS file (drag-dropped onto the editor) becomes a source, its raw bytes copied
// into the project's Sources/ tree, and a typed Asset instance in the content DB whose import
// settings point at it. Cooking then takes the source to its product like any other asset (the
// imported file's content is part of the recipe hash).
//
// IFileImporter implementations live with their asset modules (texture, image, ...) and are
// registered by the executable; routing is by lowercase extension. Several importers may claim
// one extension: FindFor returns the first match, and FindAllFor returns them all so the editor
// can offer a chooser.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module pipeline.importer;

import foundation.core;
import foundation.content;

using namespace foundation::core;

namespace pipeline
{
    RefPtr<Object> IFileImporter::PrepareOnWorker(StringView /*sourcePath*/, IAllocator&)
    {
        return {};
    }
    u64 ImporterRegistry::Register(UniquePtr<IFileImporter> importer)
    {
        if (!importer)
        {
            return 0;
        }
        m_importers.PushBack(Move(importer));
        m_keys.PushBack(m_nextKey);
        return m_nextKey++;
    }

    bool ImporterRegistry::Unregister(u64 key)
    {
        for (usize i = 0; i < m_keys.Size(); ++i)
        {
            if (m_keys[i] == key)
            {
                m_importers.RemoveAt(i);
                m_keys.RemoveAt(i);
                return true;
            }
        }
        return false;
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

    Array<IFileImporter*> ImporterRegistry::FindAllFor(StringView extension) const
    {
        Array<IFileImporter*> matches;
        for (const UniquePtr<IFileImporter>& importer : m_importers)
        {
            if (importer->Accepts(extension))
            {
                matches.PushBack(importer.Get());
            }
        }
        return matches;
    }
}
