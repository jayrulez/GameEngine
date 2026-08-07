// The native event bus (draconic.scene :events) - exercised with ZERO scripting, because the
// C++-only contract is load-bearing: a C++-only game must publish/subscribe with native callbacks.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;

using namespace draconic::foundation;
using namespace draconic::scene;

TEST_CASE("event bus: publish is deferred; Drain delivers the payload to a native subscriber")
{
    EventBus bus;
    f64 got = 0.0;
    int calls = 0;
    (void)bus.Subscribe(StringHash(u8"score"),
                        [&](const Variant& v)
                        {
                            ++calls;
                            if (const f64* n = v.TryGet<f64>())
                            {
                                got = *n;
                            }
                        });
    bus.Publish(StringHash(u8"score"), Variant::From<f64>(7.0));
    CHECK(calls == 0); // deferred - nothing fires until Drain
    CHECK(bus.PendingCount() == 1u);

    bus.Drain();
    CHECK(calls == 1);
    CHECK(got == doctest::Approx(7.0));
    CHECK(bus.PendingCount() == 0u);
}

TEST_CASE("event bus: name filtering + subscription order + unsubscribe")
{
    EventBus bus;
    String order;
    (void)bus.Subscribe(StringHash(u8"A"), [&](const Variant&) { order += u8"1"; });
    (void)bus.Subscribe(StringHash(u8"A"), [&](const Variant&) { order += u8"2"; });
    const u32 handleB = bus.Subscribe(StringHash(u8"B"), [&](const Variant&) { order += u8"B"; });

    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"12")); // both A subscribers, in subscription order; never B

    bus.Unsubscribe(handleB);
    bus.Publish(StringHash(u8"B"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"12")); // B unsubscribed -> no delivery
}

TEST_CASE("event bus: a handler's emit cascades in the SAME drain")
{
    EventBus bus;
    int aCalls = 0, bCalls = 0;
    (void)bus.Subscribe(StringHash(u8"A"),
                        [&](const Variant&)
                        {
                            ++aCalls;
                            bus.Publish(StringHash(u8"B"), Variant{}); // cascade
                        });
    (void)bus.Subscribe(StringHash(u8"B"), [&](const Variant&) { ++bCalls; });

    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(aCalls == 1);
    CHECK(bCalls == 1); // B delivered in the same Drain, not deferred to next frame
}

TEST_CASE("event bus: a runaway emit loop is bounded (Drain terminates, never hangs)")
{
    EventBus bus;
    int calls = 0;
    (void)bus.Subscribe(StringHash(u8"loop"),
                        [&](const Variant&)
                        {
                            ++calls;
                            bus.Publish(StringHash(u8"loop"), Variant{}); // re-emits forever
                        });
    bus.Publish(StringHash(u8"loop"), Variant{});
    bus.Drain(); // must terminate at the pass cap, not spin
    CHECK(calls >= 1);
    CHECK(calls <= static_cast<int>(EventBus::kMaxDrainPasses)); // capped, not infinite
}
