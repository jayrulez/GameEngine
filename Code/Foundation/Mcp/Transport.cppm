// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp - :transport partition
//
// Line-oriented transport for newline-delimited JSON-RPC (one message per line, with no
// Content-Length headers; those belong to LSP, not MCP). ITransport is the interface;
// InMemoryTransport drives tests and embedding; StdioTransport is the real stdin/stdout pipe.
// Nothing but responses may reach stdout: the host sends engine logs to stderr, or a stray print
// would corrupt the stream. Serve is the single-threaded loop: read a line, dispatch, write the
// response.

module;
#include "Core/Prelude.h"

#include <cstdio>

export module foundation.mcp:transport;

import foundation.core;
import :server;

using namespace foundation::core;

export namespace foundation::mcp
{
    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        // Reads the next inbound line (newline stripped) into `out`. Returns false at end of stream.
        [[nodiscard]] virtual bool ReadLine(String& out) = 0;
        // Writes one outbound line; the transport appends the message delimiter.
        virtual void WriteLine(StringView line) = 0;
    };

    // Feeds input lines and captures output lines: the transport for tests and in-process use.
    class InMemoryTransport final : public ITransport
    {
        Array<String> m_input;
        usize m_cursor = 0;
        Array<String> m_output;

    public:
        void Push(String line) { m_input.PushBack(Move(line)); }

        [[nodiscard]] bool ReadLine(String& out) override
        {
            if (m_cursor >= m_input.Size())
            {
                return false;
            }
            out = m_input[m_cursor++];
            return true;
        }
        void WriteLine(StringView line) override { m_output.PushBack(String(line)); }

        [[nodiscard]] usize OutputCount() const noexcept { return m_output.Size(); }
        [[nodiscard]] const String& Output(usize index) const { return m_output[index]; }
    };

    // Real stdin/stdout pipe. Reads a full line of any length; writes a line and flushes.
    class StdioTransport final : public ITransport
    {
    public:
        [[nodiscard]] bool ReadLine(String& out) override
        {
            out.Clear();
            bool sawAny = false;
            for (;;)
            {
                const int c = std::fgetc(stdin);
                if (c == EOF)
                {
                    return sawAny; // a final line without a trailing newline is still delivered
                }
                sawAny = true;
                if (c == '\n')
                {
                    return true;
                }
                if (c != '\r')
                {
                    out.PushBack(static_cast<char8_t>(c));
                }
            }
        }
        void WriteLine(StringView line) override
        {
            if (line.Size() > 0)
            {
                std::fwrite(line.Data(), 1, line.Size(), stdout);
            }
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    };

    // Serve loop: single-threaded read, dispatch and write, until the input stream ends. A
    // notification produces no output line. A tool that is not finished is re-entered with the
    // same line after a short sleep; stdio has no frame to pump, so this loop is the pump.
    inline void Serve(McpServer& server, ITransport& transport)
    {
        String line;
        while (transport.ReadLine(line))
        {
            LineOutcome outcome = server.HandleLine(line.AsView());
            while (outcome.state == LineState::NotFinished)
            {
                SleepMilliseconds(1);
                outcome = server.HandleLine(line.AsView());
            }
            if (outcome.state == LineState::Answered)
            {
                transport.WriteLine(outcome.response.AsView());
            }
        }
    }
}
