// AudioBusLayoutPage tests (headless): the cycle guard is a pure free function, and the ASSET
// round-trips through the binary serializer (the page's undo-blob path). The tree/inspector
// wiring needs a live host and is exercised in the editor app.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.audio;
import draconic.audio.editor;
import draconic.editor.audio;

using namespace draconic::foundation;
namespace audio = draconic::audio;

TEST_CASE("bus layout page: AudioBusWouldCycle catches direct + transitive cycles")
{
    audio::AudioBusLayoutAsset asset;
    // A -> B -> C (parents point UP the chain).
    asset.custom[0].name = String(u8"A");
    asset.custom[0].parent = String(u8"B");
    asset.custom[1].name = String(u8"B");
    asset.custom[1].parent = String(u8"C");
    asset.custom[2].name = String(u8"C");
    asset.custom[2].parent = String(u8"Master");

    // Re-parenting C under A closes the loop (transitively). Under B likewise. Under a
    // fixed bus never cycles.
    CHECK(draconic::editor::AudioBusWouldCycle(asset, 2, u8"A"));
    CHECK(draconic::editor::AudioBusWouldCycle(asset, 2, u8"B"));
    CHECK_FALSE(draconic::editor::AudioBusWouldCycle(asset, 2, u8"Effects"));
    CHECK_FALSE(draconic::editor::AudioBusWouldCycle(asset, 2, u8"Master"));
    // A under C is the DIRECT ancestor case.
    CHECK(draconic::editor::AudioBusWouldCycle(asset, 0, u8"A"));
    // A under an unused name: no slot resolves - no cycle.
    CHECK_FALSE(draconic::editor::AudioBusWouldCycle(asset, 0, u8"Nope"));
}

TEST_CASE("bus layout page: asset round-trips buses + custom slots (undo blob path)")
{
    audio::AudioBusLayoutAsset a;
    a.music.volume = 0.3f;
    a.effects.lowpassHz = 1500.0f;
    a.custom[0].name = String(u8"drums");
    a.custom[0].parent = String(u8"Effects");
    a.custom[0].bus.lowpassHz = 1200.0f;
    a.custom[0].bus.reverbWet = 0.4f;

    // The custom-slot bank is gated on ar.Version() >= 2: the blob must ride a versioned
    // payload exactly like the page's undo snapshots (a raw serializer would DROP the bank).
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, audio::AudioBusLayoutAsset::StaticType());
        a.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    audio::AudioBusLayoutAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, audio::AudioBusLayoutAsset::StaticType());
        b.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.music.volume == doctest::Approx(0.3f));
    CHECK(b.effects.lowpassHz == doctest::Approx(1500.0f));
    CHECK(b.custom[0].name.AsView() == u8"drums");
    CHECK(b.custom[0].parent.AsView() == u8"Effects");
    CHECK(b.custom[0].bus.lowpassHz == doctest::Approx(1200.0f));
    CHECK(b.custom[0].bus.reverbWet == doctest::Approx(0.4f));
}
