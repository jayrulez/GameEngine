// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Pipeline's entry. Load registers the note's types through the context and adds its
// builder and its creator to the host's pipeline registries (the editor's, a tool's), each recorded
// and taken out again when the plugin unloads.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.content;
import foundation.runtime;
import pipeline.core;
import pipeline.importer; // PipelinePluginServices
import nativesample.pipeline;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        context.Types().Register(nativesample::NoteAsset::StaticType());
        context.Types().Register(nativesample::NoteProduct::StaticType());
        foundation::core::RegisterSerializable<nativesample::NoteAsset>(context.Serializables());
        foundation::core::RegisterSerializable<nativesample::NoteProduct>(context.Serializables());
        pipeline::PipelinePluginServices* pipeline = context.Find<pipeline::PipelinePluginServices>();
        if (pipeline == nullptr)
        {
            return;
        }
        (void)pipeline->AddBuilder(foundation::core::MakeUnique<nativesample::NoteBuilder>(context.Allocator()));
        pipeline::AssetCreator creator;
        creator.label = foundation::core::String(nativesample::kNoteCreator);
        creator.category = foundation::core::String(u8"Native Sample");
        creator.type = &nativesample::NoteAsset::StaticType();
        creator.defaultGroup = foundation::core::String(u8"Notes");
        creator.run = [](const pipeline::AssetCreationContext& at)
        {
            nativesample::NoteAsset note;
            return pipeline::CreateWrittenInstance(at.Target(), at.NameOr(u8"Note"), nativesample::NoteAsset::StaticType(), note);
        };
        (void)pipeline->AddCreator(foundation::core::Move(creator));
    }
}

PLUGIN_PART(&Load, nullptr)
