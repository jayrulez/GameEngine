// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Audio - reflection implementation unit: AudioClipAsset's reflected surface.
//
// Kept out of the AudioAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit a
// gcm cluster. The classes declare identity with RTTI_OBJECT in the interface; this unit defines
// their StaticType() with properties and tooling attributes. There are no enums, so no registrar
// is needed: the reflection comes with StaticType(), registered by RegisterAudioAssets().

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module audio.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;
using namespace foundation::audio;

namespace pipeline{
    REFLECT_MEMBERS(AudioClipAsset, "rtti::pipeline::audio")
    {
        builder.Attribute("displayName", String(u8"Audio Clip"))
            .Attribute("category", String(u8"Audio"))
            .Property<&AudioClipAsset::stream>("stream")
            .PropAttribute("displayName", String(u8"Stream"))
            .PropAttribute("description", String(u8"Decode on the fly at runtime (music/ambience)"))
            .Property<&AudioClipAsset::keepCompressed>("keepCompressed")
            .PropAttribute("displayName", String(u8"Keep Compressed"))
            .Property<&AudioClipAsset::forceMono>("forceMono")
            .PropAttribute("displayName", String(u8"Force Mono"))
            .Property<&AudioClipAsset::loop>("loop")
            .Property<&AudioClipAsset::loopStartFrame>("loopStartFrame")
            .PropAttribute("displayName", String(u8"Loop Start Frame"))
            .PropAttribute("visibleWhen", String(u8"loop"))
            .Property<&AudioClipAsset::loopEndFrame>("loopEndFrame")
            .PropAttribute("displayName", String(u8"Loop End Frame"))
            .PropAttribute("description", String(u8"0 = clip end"))
            .PropAttribute("visibleWhen", String(u8"loop"))
            .Property<&AudioClipAsset::trimTrailingSilence>("trimTrailingSilence")
            .PropAttribute("displayName", String(u8"Trim Trailing Silence"))
            .Property<&AudioClipAsset::normalize>("normalize")
            .PropAttribute("displayName", String(u8"Normalize"))
            .Property<&AudioClipAsset::gain>("gain")
            .PropAttribute("displayName", String(u8"Gain"))
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.01f, 0.0f});
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(AudioBusLayoutAsset, "rtti::pipeline::audio")
    {
        builder.DataVersion(2)
            .Attribute("displayName", String(u8"Audio Bus Layout"));
    }

    REFLECT_MEMBERS(SoundCueAsset, "rtti::pipeline::audio")
    {
        builder.Attribute("displayName", String(u8"Sound Cue"));
    }
}
