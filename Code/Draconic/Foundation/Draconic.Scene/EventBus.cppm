// Draconic::Scene - the `:events` partition.
//
// A native, name-keyed event bus. It is a SCENE facility (and a run-scoped one later), with NO
// scripting dependency: C++ systems Publish/Subscribe directly with native callbacks, so a
// C++-only game is a first-class participant. Script reaches it through a BRIDGE (the
// ScriptPhysicsContactBridge pattern), never the reverse - the bus never delivers "to script".
//
// Delivery is DEFERRED: Publish enqueues; Drain (called at the scene tick's top level, no VM call
// active) delivers every queued event to its subscribers IN SUBSCRIPTION ORDER. A handler may
// Publish again - those cascade in the SAME Drain, bounded to kMaxDrainPasses to break a runaway
// loop. Payload is a Variant - the one currency that also crosses C++<->script and Wren<->AngelScript.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.scene:events;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::scene
{
    class EventBus
    {
    public:
        // Cascade bound: a handler that keeps emitting cannot spin the drain forever.
        static constexpr u32 kMaxDrainPasses = 8;

        // Publish `payload` under event name `name`. Delivered (deferred) at the next Drain.
        void Publish(StringHash name, Variant payload)
        {
            m_queue.PushBack(PendingEvent{name, Move(payload)});
        }

        // Subscribe a native callback to event `name`; fires (in subscription order) for every
        // matching event at Drain time. Returns a handle for Unsubscribe.
        [[nodiscard]] u32 Subscribe(StringHash name, Function<void(const Variant&)> callback)
        {
            const u32 handle = m_nextHandle++;
            m_subscribers.PushBack(Subscriber{name, handle, Move(callback)});
            return handle;
        }

        // Remove a subscription by its handle. Safe if already gone. (Not for use from inside a
        // handler mid-Drain - do subscription churn outside delivery, or via the script bridge's
        // owner-teardown sweep.)
        void Unsubscribe(u32 handle)
        {
            for (usize i = 0; i < m_subscribers.Size(); ++i)
            {
                if (m_subscribers[i].handle == handle)
                {
                    m_subscribers.RemoveAt(i);
                    return;
                }
            }
        }

        // Deliver all queued events. Cascaded (a handler's Publish is delivered in this same call)
        // up to kMaxDrainPasses; within one event, subscribers fire in subscription order.
        void Drain()
        {
            u32 pass = 0;
            while (!m_queue.IsEmpty())
            {
                if (++pass > kMaxDrainPasses)
                {
                    DRACONIC_LOG_WARNING(
                        u8"Scene",
                        u8"event bus drain hit the {} pass cap - dropping the rest (runaway emit?)",
                        kMaxDrainPasses);
                    m_queue.Clear();
                    break;
                }
                // Take the current batch; a handler's Publish appends to a fresh queue (next pass).
                Array<PendingEvent> batch = Move(m_queue);
                m_queue = Array<PendingEvent>{};
                for (const PendingEvent& event : batch)
                {
                    // Snapshot the count so a handler that subscribes does not receive its own event.
                    const usize count = m_subscribers.Size();
                    for (usize i = 0; i < count && i < m_subscribers.Size(); ++i)
                    {
                        if (m_subscribers[i].name == event.name)
                        {
                            m_subscribers[i].callback(event.payload);
                        }
                    }
                }
            }
        }

        // Drop all subscribers + pending events (scene teardown).
        void Clear()
        {
            m_subscribers.Clear();
            m_queue.Clear();
        }

        [[nodiscard]] usize SubscriberCount() const noexcept { return m_subscribers.Size(); }
        [[nodiscard]] usize PendingCount() const noexcept { return m_queue.Size(); }

    private:
        struct Subscriber
        {
            StringHash name;
            u32 handle;
            Function<void(const Variant&)> callback;
        };
        struct PendingEvent
        {
            StringHash name;
            Variant payload;
        };
        Array<Subscriber> m_subscribers;
        Array<PendingEvent> m_queue;
        u32 m_nextHandle = 1;
    };
}
