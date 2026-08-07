/// Draconic::Net - `draconic.net:rpc` partition.
///
/// Remote procedure calls (docs/design/networking.md §3 `draconic.net.rpc`, P1). An RpcTable maps
/// RPC names to handlers; Call serializes (rpc-id + args) and sends it over a NetSession on a
/// reserved channel; the receiver Dispatches it to the registered handler. Args are written/read
/// through the bit-exact wire layer - the same primitives the (later) reflection auto-marshaling
/// will emit, so a `Replicated`/reflected-arg convenience layers cleanly on top of this mechanism.
///
/// For the turn-based target this is the PRIMARY gameplay path: a client Calls an order RPC on the
/// server; the server validates + resolves it and CallAlls the result back out.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.net:rpc;

import draconic.foundation;
import :transport;
import :session;
import :wire;

using namespace draconic::foundation;

export namespace draconic::net
{

    // Channel 254 is reserved for RPC (255 is session control). Route Received events on this channel
    // to RpcTable::Dispatch; games use channels 0..253 for their own raw messages.
    inline constexpr u8 kRpcChannel = 254;

    // A name->handler registry for remote calls over a NetSession. Register once; Call/CallAll to invoke
    // remotely; Dispatch the matching Received events. Handlers read their args from the given BitReader.
    class RpcTable
    {
    public:
        // sender = who invoked it; args = the call's serialized arguments (may be empty).
        using Handler = Function<void(PeerId sender, BitReader& args)>;

        // Register a handler for `name` (re-registering replaces it). Names are hashed to a u32 id on the
        // wire, so both ends must use the SAME names.
        void On(StringView name, Handler handler)
        {
            m_handlers.InsertOrAssign(Hash(name), static_cast<Handler&&>(handler));
        }

        // Invoke `name` on one peer. `writeArgs` serializes the arguments (pass an empty function for none).
        void Call(NetSession& session, PeerId peer, StringView name,
                  const Function<void(BitWriter&)>& writeArgs = {},
                  Reliability reliability = Reliability::ReliableOrdered)
        {
            BitWriter w;
            w.WriteU32(Hash(name));
            if (writeArgs)
            {
                writeArgs(w);
            }
            session.Send(peer, kRpcChannel, w.Data(), reliability);
        }

        // Invoke `name` on every connected peer (server broadcast).
        void CallAll(NetSession& session, StringView name,
                     const Function<void(BitWriter&)>& writeArgs = {},
                     Reliability reliability = Reliability::ReliableOrdered)
        {
            BitWriter w;
            w.WriteU32(Hash(name));
            if (writeArgs)
            {
                writeArgs(w);
            }
            session.Broadcast(kRpcChannel, w.Data(), reliability);
        }

        // Is a Received event an RPC? (Route those to Dispatch; handle the rest yourself.)
        [[nodiscard]] static bool IsRpcChannel(u8 channel) noexcept
        {
            return channel == kRpcChannel;
        }

        // Dispatch a received RPC payload to its handler (no-op if the id is unregistered or truncated).
        void Dispatch(PeerId sender, Span<const byte> payload)
        {
            BitReader r(payload);
            const u32 id = r.ReadU32();
            if (!r.Ok())
            {
                return;
            }
            if (Handler* h = m_handlers.Find(id))
            {
                (*h)(sender, r);
            }
        }

        // Convenience: drain a session's events, routing RPCs here and returning the rest via `onOther`.
        void Pump(NetSession& session, const Function<void(const NetEvent&)>& onOther = {})
        {
            NetEvent ev;
            while (session.PollEvent(ev))
            {
                if (ev.kind == NetEventKind::Received && IsRpcChannel(ev.channel))
                {
                    Dispatch(ev.peer, ev.payload.AsSpan());
                }
                else if (onOther)
                {
                    onOther(ev);
                }
            }
        }

        // The wire id a name maps to (FNV-1a). Exposed for tests / debugging.
        [[nodiscard]] static u32 Hash(StringView name) noexcept
        {
            u32 h = 2166136261u;
            for (usize i = 0; i < name.Size(); ++i)
            {
                h ^= static_cast<u8>(name[i]);
                h *= 16777619u;
            }
            return h;
        }

    private:
        HashMap<u32, Handler> m_handlers;
    };

}
