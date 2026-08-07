// NetEcho - a console proof of the draconic.net stack end to end: a reliable-UDP server + client
// over real localhost sockets exchange a few chat lines (client sends, server echoes), then exit.
// Demonstrates wire -> INetTransport -> reliable-UDP -> real UDP sockets with no graphics.
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

int main()
{
    ConsoleSink consoleSink;
    GlobalLogger().AddSink(&consoleSink);

    net::UdpSocket serverSock(0);
    net::UdpSocket clientSock(0);
    if (!serverSock.IsOpen() || !clientSock.IsOpen())
    {
        DRACONIC_LOG_ERROR(u8"NetEcho", u8"failed to open UDP sockets");
        return 1;
    }
    DRACONIC_LOG_INFO(u8"NetEcho", u8"server on :{}, client on :{}", serverSock.BoundPort(),
                      clientSock.BoundPort());

    net::ReliableTransport server(serverSock);
    net::ReliableTransport client(clientSock);
    server.SetAccepting(true);
    const net::PeerId serverAsSeenByClient = client.Connect(serverSock.LocalEndpoint());

    const StringView lines[] = {u8"hello", u8"from the client", u8"over reliable-UDP"};
    constexpr usize kLineCount = 3;

    bool clientConnected = false;
    usize sent = 0;
    usize echoed = 0;

    for (int tick = 0; tick < 1000 && echoed < kLineCount; ++tick)
    {
        client.Update(10.0f);
        server.Update(10.0f);

        // Server: echo every received message straight back to its sender.
        net::NetEvent ev;
        while (server.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Connected)
            {
                DRACONIC_LOG_INFO(u8"NetEcho", u8"server: peer {} connected", ev.peer);
            }
            else if (ev.kind == net::NetEventKind::Received)
            {
                const StringView text(reinterpret_cast<const utf8char*>(ev.payload.Data()),
                                      ev.payload.Size());
                DRACONIC_LOG_INFO(u8"NetEcho", u8"server: recv '{}' -> echo", text);
                server.Send(ev.peer, 0, ev.payload.AsSpan(), net::Reliability::ReliableOrdered);
            }
        }

        // Client: note the connection, print echoes, and pace out the chat lines.
        while (client.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Connected)
            {
                clientConnected = true;
                DRACONIC_LOG_INFO(u8"NetEcho", u8"client: connected to server");
            }
            else if (ev.kind == net::NetEventKind::Received)
            {
                const StringView text(reinterpret_cast<const utf8char*>(ev.payload.Data()),
                                      ev.payload.Size());
                DRACONIC_LOG_INFO(u8"NetEcho", u8"client: echo <- '{}'", text);
                ++echoed;
            }
        }

        if (clientConnected && sent < kLineCount && (tick % 5) == 0)
        {
            const StringView line = lines[sent++];
            client.Send(serverAsSeenByClient, 0,
                        Span<const byte>(reinterpret_cast<const byte*>(line.Data()), line.Size()),
                        net::Reliability::ReliableOrdered);
        }
    }

    if (echoed == kLineCount)
    {
        DRACONIC_LOG_INFO(u8"NetEcho", u8"all {} lines round-tripped - OK", kLineCount);
        return 0;
    }
    DRACONIC_LOG_ERROR(u8"NetEcho", u8"only {}/{} lines echoed", echoed, kLineCount);
    return 1;
}
