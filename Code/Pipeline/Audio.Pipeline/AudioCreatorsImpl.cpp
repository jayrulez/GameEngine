// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Audio.Pipeline - the audio domain's New Asset creators: what File > New and asset_create
// make, with no editor (the source database and the sources folder are all a creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module audio.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterAudioCreators(AssetCreatorRegistry& registry)
    {
        {
            AssetCreator creator;
            creator.label = String(u8"Audio Bus Layout");
            creator.category = String(u8"Audio");
            creator.type = &AudioBusLayoutAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                AudioBusLayoutAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"BusLayout"),
                                             AudioBusLayoutAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
        {
            AssetCreator creator;
            creator.label = String(u8"Sound Cue");
            creator.category = String(u8"Audio");
            creator.type = &SoundCueAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                SoundCueAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"SoundCue"),
                                             SoundCueAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}
