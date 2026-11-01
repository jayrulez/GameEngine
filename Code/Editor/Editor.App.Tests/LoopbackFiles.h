// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A loopback HTTP server for the tests, pumped on the test's thread: it answers each connection once
// with the file its path names (200, the bytes) or a 404, then closes. Include it after importing
// foundation.core and foundation.net.
#pragma once

class LoopbackFiles
{
public:
    LoopbackFiles() : m_listener(0, true) {}
    [[nodiscard]] foundation::core::u16 Port() const { return m_listener.BoundPort(); }
    /// The URL `path` is served at ("/samples.json").
    [[nodiscard]] foundation::core::String Url(foundation::core::StringView path) const
    {
        return foundation::core::Format(u8"http://127.0.0.1:{}{}", Port(), path);
    }
    void Serve(foundation::core::StringView path, foundation::core::Array<foundation::core::byte> body)
    {
        m_files.PushBack(File{foundation::core::String(path), foundation::core::Move(body)});
    }
    void Serve(foundation::core::StringView path, foundation::core::StringView text)
    {
        foundation::core::Array<foundation::core::byte> body;
        for (const foundation::core::utf8char c : text)
            body.PushBack(static_cast<foundation::core::byte>(c));
        Serve(path, foundation::core::Move(body));
    }

    void Pump()
    {
        using namespace foundation::core;
        for (foundation::net::TcpSocket socket = m_listener.Accept(); socket.IsOpen(); socket = m_listener.Accept())
        {
            Connection c;
            c.socket = Move(socket);
            m_connections.PushBack(Move(c));
        }
        for (Connection& c : m_connections)
        {
            byte buffer[4096];
            for (i64 n = c.socket.Receive(Span<byte>(buffer, sizeof(buffer))); n > 0; n = c.socket.Receive(Span<byte>(buffer, sizeof(buffer))))
                c.request += StringView(reinterpret_cast<const utf8char*>(buffer), static_cast<usize>(n));
            if (c.reply.IsEmpty() && c.request.AsView().ContainsIgnoreCase(u8"\r\n\r\n"))
                Answer(c);
            while (c.sent < c.reply.Size())
            {
                const i64 n = c.socket.Send(Span<const byte>(c.reply.Data() + c.sent, c.reply.Size() - c.sent));
                if (n <= 0)
                    break;
                c.sent += static_cast<usize>(n);
            }
            if (!c.reply.IsEmpty() && c.sent == c.reply.Size())
                c.socket = foundation::net::TcpSocket{}; // closed: the body is all there
        }
    }

private:
    struct File
    {
        foundation::core::String path;
        foundation::core::Array<foundation::core::byte> body;
    };
    struct Connection
    {
        foundation::net::TcpSocket socket;
        foundation::core::String request;
        foundation::core::Array<foundation::core::byte> reply;
        foundation::core::usize sent = 0;
    };

    void Answer(Connection& c)
    {
        using namespace foundation::core;
        // "GET /path HTTP/1.1": the path between the first two spaces.
        const StringView line = c.request.AsView();
        usize start = 0;
        while (start < line.Size() && line[start] != u8' ')
            ++start;
        usize end = start + 1;
        while (end < line.Size() && line[end] != u8' ')
            ++end;
        const StringView path = start < line.Size() ? line.SubStr(start + 1, end - start - 1) : StringView{};
        const File* found = nullptr;
        for (const File& f : m_files)
            if (f.path.AsView() == path)
                found = &f;
        const String head = found != nullptr ? Format(u8"HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", found->body.Size())
                                             : String(u8"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        for (const utf8char ch : head)
            c.reply.PushBack(static_cast<byte>(ch));
        if (found != nullptr)
            for (const byte b : found->body)
                c.reply.PushBack(b);
    }

    foundation::net::TcpListener m_listener;
    foundation::core::Array<File> m_files;
    foundation::core::Array<Connection> m_connections;
};
