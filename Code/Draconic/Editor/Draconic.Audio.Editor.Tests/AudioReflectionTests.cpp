// Reflection track P1: AudioClipAsset's reflected surface. Verifies the authored bool/scalar
// fields enumerate with attributes (loop-frame fields gated on `loop`), and round-trip through
// get/set. No enums on this asset.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <initializer_list>
import draconic.foundation;
import draconic.audio.editor;

using namespace draconic::foundation;

namespace
{
    bool CEq(const char* a, const char* b)
    {
        if (a == nullptr || b == nullptr)
        {
            return a == b;
        }
        while (*a != '\0' && *b != '\0')
        {
            if (*a != *b)
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }
}

TEST_CASE("reflection-p1: AudioClipAsset exposes its authored fields with attributes")
{
    draconic::audio::RegisterAudioAssets();
    const TypeInfo& type = draconic::audio::AudioClipAsset::StaticType();

    CHECK(CEq(type.name, "AudioClipAsset"));
    CHECK(PropertyCount(type) == 9u);
    for (const char* name : {"stream", "keepCompressed", "forceMono", "loop", "loopStartFrame",
                             "loopEndFrame", "trimTrailingSilence", "normalize", "gain"})
    {
        CHECK_MESSAGE(FindProperty(type, name) != nullptr, name);
    }

    // The loop-frame fields only apply when looping (visibleWhen the generic page evaluates).
    const PropertyInfo* loopStart = FindProperty(type, "loopStartFrame");
    REQUIRE(loopStart != nullptr);
    const Attribute* vis = FindAttribute(*loopStart, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    CHECK(vis->value.Get<String>().AsView() == StringView(u8"loop"));

    const PropertyInfo* gain = FindProperty(type, "gain");
    REQUIRE(gain != nullptr);
    CHECK(FindAttribute(*gain, u8"range") != nullptr);
}

TEST_CASE("reflection-p1: AudioClipAsset bool/scalar properties round-trip")
{
    draconic::audio::RegisterAudioAssets();
    const TypeInfo& type = draconic::audio::AudioClipAsset::StaticType();

    draconic::audio::AudioClipAsset asset;
    Instance inst = Instance::From(&asset);

    const PropertyInfo* stream = FindProperty(type, "stream");
    REQUIRE(stream != nullptr);
    CHECK(SetProperty(*stream, inst, Variant::From(true)).IsOk());
    CHECK(GetProperty(*stream, inst).Get<bool>() == true);
    CHECK(asset.stream == true);

    const PropertyInfo* gain = FindProperty(type, "gain");
    REQUIRE(gain != nullptr);
    CHECK(SetProperty(*gain, inst, Variant::From(2.5f)).IsOk());
    CHECK(asset.gain == doctest::Approx(2.5f));

    const PropertyInfo* loopEnd = FindProperty(type, "loopEndFrame");
    REQUIRE(loopEnd != nullptr);
    CHECK(SetProperty(*loopEnd, inst, Variant::From<u64>(44100ull)).IsOk());
    CHECK(asset.loopEndFrame == 44100ull);
}
