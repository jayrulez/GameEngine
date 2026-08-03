// Draconic::AudioEditor - reflection implementation unit: AudioClipAsset's reflected surface.
//
// Kept OUT of the AudioAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). The class declares identity via DRACONIC_OBJECT in the
// interface; this unit defines AudioClipAsset::StaticType() WITH properties + tooling attributes.
// No enums here, so no registrar is needed - the type reflection rides StaticType(), registered by
// the existing RegisterAudioAssets(). Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.audio.editor;

import draconic.core;
import draconic.editor;

using namespace draconic::core;

namespace draconic::audio
{
    DRACONIC_REFLECT(AudioClipAsset, "draconic::audio")
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
}
