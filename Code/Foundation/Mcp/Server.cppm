// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp - :server partition
//
// The JSON-RPC 2.0 dispatcher + MCP lifecycle + tool/resource registry. HandleLine takes ONE
// newline-delimited message and says how it went: the response line to write, nothing (a
// notification), or NOT FINISHED - the tool asked to be re-entered, so the transport keeps the
// caller waiting and hands the same line in again on its next pump. It always survives bad input.
// Two error layers, never conflated: PROTOCOL failures are JSON-RPC error responses; TOOL
// failures are SUCCESSFUL responses whose result carries isError:true + the real error text (an
// agent must see the underlying cook/import message, not a protocol failure).
//
// v1 subset only: initialize / notifications/initialized / tools.list / tools.call /
// resources.list / resources.read / ping. Capabilities advertise exactly {tools, resources}.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.mcp:server;

import foundation.core;
import foundation.json;
import :schema;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace json = foundation::json;

export namespace foundation::mcp
{
    // Pinned MCP protocol revision. See https://modelcontextprotocol.io/specification/2025-06-18
    inline constexpr StringView kProtocolVersion = u8"2025-06-18";

    // JSON-RPC error codes (the subset we emit).
    enum class RpcError : i32
    {
        ParseError = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603,
    };

    // A tool turns validated args into a JSON result, or an error MESSAGE. The error channel is a
    // String (not a bare ErrorCode) precisely so agents receive the real underlying text.
    using ToolResult = Result<JsonValue, String>;

    /// What a handler hands back: its answer, or NOT FINISHED - the host keeps the caller
    /// waiting and re-enters the handler with the SAME arguments on its next pump, until a call
    /// answers. For a tool that must let its host make progress in between (a cook running on
    /// a background thread, a simulation that has to advance frames). Such a handler keeps its
    /// own progress state across the re-entries and gives up on its own timeout: a host that
    /// stopped pumping and a handler that never answers look the same to the caller. A handler
    /// that answers at once returns its ToolResult as always.
    struct ToolOutcome
    {
        Optional<ToolResult> answer; ///< empty = not finished

        template <typename T>
            requires std::is_convertible_v<T&&, ToolResult>
        ToolOutcome(T&& result) : answer(ToolResult(Forward<T>(result)))
        {
        }

        [[nodiscard]] static ToolOutcome NotFinished() { return ToolOutcome(); }
        [[nodiscard]] bool IsFinished() const noexcept { return answer.HasValue(); }

    private:
        ToolOutcome() = default;
    };
    using ToolHandler = Function<ToolOutcome(const JsonValue& args)>;

    /// What a call-aware tool keeps between the entries of one call. A tool derives its own; its
    /// destructor is its cleanup, run when the call answers or its caller leaves.
    class ToolCallState
    {
    public:
        virtual ~ToolCallState() = default;
    };

    /// One tools/call as a tool that answers NotFinished sees it across its re-entries: the same
    /// object every time the call comes back, so the tool keeps what it started in `state` rather
    /// than recognising its own call by its arguments (two identical calls in flight at once, from
    /// two agents, have the same arguments). The server owns it from the call's first entry to its
    /// answer, or until the transport says the caller went away (McpServer::AbandonCall).
    struct ToolCall
    {
        /// The transport's identity for the call; 0 when it gave none (known by its line).
        u64 id = 0;
        /// This call answered NotFinished before, and this is it coming back.
        bool isReentry = false;
        /// What the tool keeps between entries, destroyed when the call ends.
        UniquePtr<ToolCallState> state;

        template <typename T>
        [[nodiscard]] T* State() noexcept
        {
            return static_cast<T*>(state.Get());
        }
    };
    /// A handler that sees its call: what a tool answering NotFinished uses.
    using ToolCallHandler = Function<ToolOutcome(ToolCall& call, const JsonValue& args)>;

    /// How HandleLine dealt with one line.
    enum class LineState : u8
    {
        Answered,     ///< `response` is the line to write back
        Notification, ///< no response, by protocol
        NotFinished,  ///< the tool asked to be re-entered: hand the SAME line in again next pump
    };
    struct LineOutcome
    {
        LineState state = LineState::Notification;
        String response;
    };

    /// What a tool does to the world, as MCP's tool annotations say it (tools/list emits them
    /// under `annotations`): a client uses them to run reads freely and confirm writes, and
    /// the editor host to decide what a call may do unattended. Every registration states its
    /// class - there is no default, because an unannotated write reads as a read.
    struct ToolAnnotations
    {
        bool readOnly = false;   ///< readOnlyHint: changes nothing
        bool destructive = true; ///< destructiveHint: may overwrite or delete what exists
        bool idempotent = false; ///< idempotentHint: calling again with the same args changes nothing more
        bool openWorld = false;  ///< openWorldHint: reaches outside the project (never, here)

        /// Reads and reports; changes nothing.
        [[nodiscard]] static ToolAnnotations ReadOnly() { return {true, false, true, false}; }
        /// Adds something new (an asset, a log line, a project) without touching what exists.
        [[nodiscard]] static ToolAnnotations Creates() { return {false, false, false, false}; }
        /// Replaces existing data (a scene's source); the same call again lands the same state.
        [[nodiscard]] static ToolAnnotations Overwrites() { return {false, true, true, false}; }
        /// Removes what exists (an asset); calling again finds nothing more to remove.
        [[nodiscard]] static ToolAnnotations Deletes() { return {false, true, true, false}; }
        /// Regenerates derived output (a cook, a dist, the open project); nothing authored is lost.
        [[nodiscard]] static ToolAnnotations Rebuilds() { return {false, false, true, false}; }
        /// Changes the editor's SESSION (a page opened, a selection) and no authored data.
        [[nodiscard]] static ToolAnnotations Adjusts() { return {false, false, true, false}; }
    };

    struct Tool
    {
        String name;
        String description;
        JsonValue inputSchema;
        ToolAnnotations annotations;
        ToolHandler handler;
        ToolCallHandler callHandler; ///< set instead of `handler` for a tool that sees its call
    };

    // A resource exposes read-only text by URI; the reader returns content or an error message.
    using ResourceReader = Function<Result<String, String>()>;
    struct Resource
    {
        String uri;
        String name;
        String mimeType;
        String description;
        ResourceReader reader;
    };

    // A DYNAMIC resource set (e.g. the open project's scene sources - entries appear and
    // disappear as the project changes, so they cannot be registered statically). `list`
    // appends the CURRENT entries (their `reader` fields may be empty - reads go through
    // `read`); `read` answers a uri or returns an empty Optional when the uri is not this
    // provider's (the server then tries the next provider).
    struct ResourceProvider
    {
        Function<void(Array<Resource>&)> list;
        Function<Optional<Result<String, String>>(StringView)> read;
    };
}

namespace foundation::mcp::detail
{
    inline JsonValue Envelope(const JsonValue& id)
    {
        JsonValue r = JsonValue::MakeObject();
        r.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        r.Set(u8"id", id); // echoed verbatim - string OR number type is preserved by JsonValue
        return r;
    }
    inline JsonValue MakeResult(const JsonValue& id, JsonValue result)
    {
        JsonValue r = Envelope(id);
        r.Set(u8"result", Move(result));
        return r;
    }
    inline JsonValue MakeError(const JsonValue& id, RpcError code, String message)
    {
        JsonValue err = JsonValue::MakeObject();
        err.Set(u8"code", JsonValue::MakeNumber(static_cast<f64>(static_cast<i32>(code))));
        err.Set(u8"message", JsonValue::MakeString(Move(message)));
        JsonValue r = Envelope(id);
        r.Set(u8"error", Move(err));
        return r;
    }

    inline LineOutcome Answered(const JsonValue& message)
    {
        return LineOutcome{LineState::Answered, message.ToString()};
    }
}

export namespace foundation::mcp
{
    class McpServer
    {
        Array<Tool> m_tools;
        Array<Resource> m_resources;
        Array<ResourceProvider> m_resourceProviders;
        String m_serverName = String(u8"engine-mcp");
        String m_serverVersion = String(u8"0.1.0");
        Function<void(StringView, bool)> m_toolObserver;

        /// A call in flight, known by the transport's id, or by its line when it gave none.
        struct CallEntry
        {
            ToolCall call;
            String line; ///< the line, when the call has no id
        };
        /// The calls that answered NotFinished and have not answered since.
        Array<CallEntry> m_calls;
        /// The identity of the message HandleLine is handling: its id, or its line when 0.
        u64 m_callId = 0;
        StringView m_callLine;

    public:
        [[nodiscard]] StringView ServerName() const noexcept { return m_serverName.AsView(); }
        [[nodiscard]] StringView ServerVersion() const noexcept
        {
            return m_serverVersion.AsView();
        }

        void SetServerInfo(String name, String version)
        {
            m_serverName = Move(name);
            m_serverVersion = Move(version);
        }

        /// Told `(toolName, isError)` after every FINISHED tools/call, on the dispatching
        /// thread - a host refreshes what a write tool changed, or logs the agent's activity.
        /// Not-finished attempts and protocol failures (no tool ran) are not reported.
        void SetToolObserver(Function<void(StringView, bool)> observer)
        {
            m_toolObserver = Move(observer);
        }
        void RegisterTool(String name, String description, JsonValue schema,
                          ToolAnnotations annotations, ToolHandler handler)
        {
            m_tools.PushBack(
                Tool{Move(name), Move(description), Move(schema), annotations, Move(handler), {}});
        }
        /// A tool whose handler sees its call (ToolCall): one that answers NotFinished and keeps
        /// what it started in the call's state.
        void RegisterTool(String name, String description, JsonValue schema,
                          ToolAnnotations annotations, ToolCallHandler handler)
        {
            m_tools.PushBack(Tool{Move(name), Move(description), Move(schema), annotations,
                                  ToolHandler{}, Move(handler)});
        }
        /// The caller of an unfinished call went away (its connection closed): the call ends, its
        /// state destroyed, as if it had answered. Nothing when the id names no call in flight.
        void AbandonCall(u64 callId)
        {
            if (callId == 0)
            {
                return;
            }
            for (usize i = 0; i < m_calls.Size(); ++i)
            {
                if (m_calls[i].call.id == callId)
                {
                    m_calls.RemoveAt(i);
                    return;
                }
            }
        }
        /// How many calls are in flight: answered NotFinished, not yet answered or abandoned.
        [[nodiscard]] usize CallsInFlight() const noexcept { return m_calls.Size(); }
        void RegisterResource(String uri, String name, String mimeType, String description,
                              ResourceReader reader)
        {
            m_resources.PushBack(
                Resource{Move(uri), Move(name), Move(mimeType), Move(description), Move(reader)});
        }
        void RegisterResourceProvider(ResourceProvider provider)
        {
            m_resourceProviders.PushBack(Move(provider));
        }
        [[nodiscard]] usize ToolCount() const noexcept { return m_tools.Size(); }
        [[nodiscard]] usize ResourceCount() const noexcept { return m_resources.Size(); }

        // Handle ONE JSON-RPC message: the response line to write, a notification (nothing to
        // write), or NotFinished (re-enter with the same line next pump). Never throws;
        // malformed input yields a protocol error line. `callId` is the transport's identity for
        // the message, the same every time it hands an unfinished call back in (the HTTP host
        // numbers each pending request); 0 when it has none, the call then known by its line.
        [[nodiscard]] LineOutcome HandleLine(StringView line, u64 callId = 0)
        {
            m_callId = callId;
            m_callLine = line;
            json::ParseResult parsed = json::Parse(line);
            if (!parsed.ok)
            {
                return detail::Answered(detail::MakeError(
                    JsonValue::MakeNull(), RpcError::ParseError, String(u8"Parse error")));
            }
            const JsonValue& msg = parsed.value;
            // Top-level arrays are batch requests - not supported (later MCP revisions dropped them).
            if (msg.IsArray())
            {
                return detail::Answered(
                    detail::MakeError(JsonValue::MakeNull(), RpcError::InvalidRequest,
                                      String(u8"Batch requests are not supported")));
            }
            if (!msg.IsObject())
            {
                return detail::Answered(detail::MakeError(
                    JsonValue::MakeNull(), RpcError::InvalidRequest, String(u8"Invalid Request")));
            }

            const JsonValue methodVal = msg.Get(u8"method");
            const bool hasId = msg.Has(u8"id");
            if (!methodVal.IsString())
            {
                if (hasId)
                {
                    return detail::Answered(detail::MakeError(
                        msg.Get(u8"id"), RpcError::InvalidRequest,
                        String(u8"Invalid Request: 'method' must be a string")));
                }
                return {}; // malformed notification - ignored, per JSON-RPC
            }
            // No id member => notification. Unknown notifications (incl. notifications/cancelled) and
            // notifications/initialized are all accepted SILENTLY: never a response.
            if (!hasId)
            {
                return {};
            }

            const JsonValue id = msg.Get(u8"id");
            const JsonValue params = msg.Get(u8"params");
            return Dispatch(methodVal.AsString().AsView(), params, id);
        }

    private:
        /// The index of the call in flight under the current identity, a new one started when
        /// there is none.
        [[nodiscard]] usize FindOrStartCall()
        {
            for (usize i = 0; i < m_calls.Size(); ++i)
            {
                const CallEntry& entry = m_calls[i];
                if (m_callId != 0 ? entry.call.id == m_callId
                                  : (entry.call.id == 0 && entry.line.AsView() == m_callLine))
                {
                    return i;
                }
            }
            CallEntry entry;
            entry.call.id = m_callId;
            if (m_callId == 0)
            {
                entry.line = String(m_callLine);
            }
            m_calls.PushBack(Move(entry));
            return m_calls.Size() - 1;
        }

        [[nodiscard]] const Tool* FindTool(StringView name) const
        {
            for (usize i = 0; i < m_tools.Size(); ++i)
            {
                if (m_tools[i].name.AsView() == name)
                {
                    return &m_tools[i];
                }
            }
            return nullptr;
        }
        [[nodiscard]] const Resource* FindResource(StringView uri) const
        {
            for (usize i = 0; i < m_resources.Size(); ++i)
            {
                if (m_resources[i].uri.AsView() == uri)
                {
                    return &m_resources[i];
                }
            }
            return nullptr;
        }

        [[nodiscard]] LineOutcome Dispatch(StringView method, const JsonValue& params,
                                           const JsonValue& id)
        {
            if (method == StringView(u8"initialize"))
            {
                JsonValue result = JsonValue::MakeObject();
                // We support exactly our pinned revision; echo it (equals the client's when supported).
                result.Set(u8"protocolVersion", JsonValue::MakeString(kProtocolVersion));
                JsonValue caps = JsonValue::MakeObject();
                caps.Set(u8"tools", JsonValue::MakeObject());
                caps.Set(u8"resources", JsonValue::MakeObject());
                result.Set(u8"capabilities", Move(caps));
                JsonValue info = JsonValue::MakeObject();
                info.Set(u8"name", JsonValue::MakeString(m_serverName));
                info.Set(u8"version", JsonValue::MakeString(m_serverVersion));
                result.Set(u8"serverInfo", Move(info));
                return detail::Answered(detail::MakeResult(id, Move(result)));
            }
            if (method == StringView(u8"ping"))
            {
                return detail::Answered(detail::MakeResult(id, JsonValue::MakeObject()));
            }
            if (method == StringView(u8"tools/list"))
            {
                JsonValue tools = JsonValue::MakeArray();
                for (usize i = 0; i < m_tools.Size(); ++i)
                {
                    JsonValue jt = JsonValue::MakeObject();
                    jt.Set(u8"name", JsonValue::MakeString(m_tools[i].name));
                    jt.Set(u8"description", JsonValue::MakeString(m_tools[i].description));
                    jt.Set(u8"inputSchema", m_tools[i].inputSchema);
                    const ToolAnnotations& a = m_tools[i].annotations;
                    JsonValue annotations = JsonValue::MakeObject();
                    annotations.Set(u8"readOnlyHint", JsonValue::MakeBool(a.readOnly));
                    annotations.Set(u8"destructiveHint", JsonValue::MakeBool(a.destructive));
                    annotations.Set(u8"idempotentHint", JsonValue::MakeBool(a.idempotent));
                    annotations.Set(u8"openWorldHint", JsonValue::MakeBool(a.openWorld));
                    jt.Set(u8"annotations", Move(annotations));
                    tools.Add(Move(jt));
                }
                JsonValue result = JsonValue::MakeObject();
                result.Set(u8"tools", Move(tools));
                return detail::Answered(detail::MakeResult(id, Move(result)));
            }
            if (method == StringView(u8"tools/call"))
            {
                const JsonValue nameVal = params.Get(u8"name");
                if (!nameVal.IsString())
                {
                    return detail::Answered(detail::MakeError(id, RpcError::InvalidParams,
                                             String(u8"tools/call requires a string 'name'")));
                }
                const Tool* tool = FindTool(nameVal.AsString().AsView());
                if (tool == nullptr)
                {
                    return detail::Answered(detail::MakeError(id, RpcError::InvalidParams,
                                             Format(u8"unknown tool '{}'", nameVal.AsString().AsView())));
                }
                JsonValue args = params.Get(u8"arguments");
                if (args.IsNull())
                {
                    args = JsonValue::MakeObject();
                }
                Optional<String> schemaError = ValidateArgs(args, tool->inputSchema);
                if (schemaError.HasValue())
                {
                    return detail::Answered(detail::MakeError(
                        id, RpcError::InvalidParams,
                        Format(u8"{}: {}", tool->name.AsView(), schemaError.Value().AsView())));
                }
                // Run the tool. A tool that is not finished is asked again next pump (same
                // line, same args). BOTH finished outcomes are successful JSON-RPC responses;
                // a tool failure is reported as isError content, not a protocol error.
                // The call this is: the one in flight under this identity, or a new one.
                const usize callIndex = FindOrStartCall();
                ToolOutcome outcome = tool->callHandler ? tool->callHandler(m_calls[callIndex].call, args)
                                                        : tool->handler(args);
                if (!outcome.IsFinished())
                {
                    m_calls[callIndex].call.isReentry = true;
                    return LineOutcome{LineState::NotFinished, String()};
                }
                m_calls.RemoveAt(callIndex); // answered: its state goes
                ToolResult& answer = outcome.answer.Value();
                JsonValue item = JsonValue::MakeObject();
                item.Set(u8"type", JsonValue::MakeString(u8"text"));
                JsonValue result = JsonValue::MakeObject();
                if (answer.HasValue())
                {
                    item.Set(u8"text", JsonValue::MakeString(answer.Value().ToString()));
                    result.Set(u8"isError", JsonValue::MakeBool(false));
                }
                else
                {
                    item.Set(u8"text", JsonValue::MakeString(Move(answer.Error())));
                    result.Set(u8"isError", JsonValue::MakeBool(true));
                }
                JsonValue content = JsonValue::MakeArray();
                content.Add(Move(item));
                const bool isError = result.Get(u8"isError").AsBool();
                result.Set(u8"content", Move(content));
                if (m_toolObserver)
                {
                    m_toolObserver(tool->name.AsView(), isError);
                }
                return detail::Answered(detail::MakeResult(id, Move(result)));
            }
            if (method == StringView(u8"resources/list"))
            {
                // Static registrations + every provider's CURRENT entries (dynamic sets).
                Array<Resource> dynamic;
                for (usize p = 0; p < m_resourceProviders.Size(); ++p)
                {
                    m_resourceProviders[p].list(dynamic);
                }
                JsonValue arr = JsonValue::MakeArray();
                const auto append = [&arr](const Resource& r)
                {
                    JsonValue jr = JsonValue::MakeObject();
                    jr.Set(u8"uri", JsonValue::MakeString(r.uri));
                    jr.Set(u8"name", JsonValue::MakeString(r.name));
                    jr.Set(u8"mimeType", JsonValue::MakeString(r.mimeType));
                    jr.Set(u8"description", JsonValue::MakeString(r.description));
                    arr.Add(Move(jr));
                };
                for (usize i = 0; i < m_resources.Size(); ++i)
                {
                    append(m_resources[i]);
                }
                for (usize i = 0; i < dynamic.Size(); ++i)
                {
                    append(dynamic[i]);
                }
                JsonValue result = JsonValue::MakeObject();
                result.Set(u8"resources", Move(arr));
                return detail::Answered(detail::MakeResult(id, Move(result)));
            }
            if (method == StringView(u8"resources/read"))
            {
                const JsonValue uriVal = params.Get(u8"uri");
                if (!uriVal.IsString())
                {
                    return detail::Answered(detail::MakeError(id, RpcError::InvalidParams,
                                             String(u8"resources/read requires a string 'uri'")));
                }
                const String uriText = uriVal.AsString(); // AsString returns BY VALUE - keep it
                const StringView uri = uriText.AsView();
                const Resource* res = FindResource(uri);
                Optional<Result<String, String>> dynamicContent;
                String mimeType;
                if (res == nullptr)
                {
                    // Not a static registration - offer the uri to each provider in turn.
                    for (usize p = 0; p < m_resourceProviders.Size() && !dynamicContent.HasValue();
                         ++p)
                    {
                        dynamicContent = m_resourceProviders[p].read(uri);
                    }
                    if (!dynamicContent.HasValue())
                    {
                        return detail::Answered(detail::MakeError(id, RpcError::InvalidParams,
                                                 Format(u8"unknown resource '{}'", uri)));
                    }
                    // The answering provider's listing carries the entry's declared mime type.
                    mimeType = String(u8"text/plain");
                    Array<Resource> dynamic;
                    for (usize p = 0; p < m_resourceProviders.Size(); ++p)
                    {
                        m_resourceProviders[p].list(dynamic);
                    }
                    for (usize i = 0; i < dynamic.Size(); ++i)
                    {
                        if (dynamic[i].uri.AsView() == uri)
                        {
                            mimeType = dynamic[i].mimeType;
                            break;
                        }
                    }
                }
                else
                {
                    mimeType = res->mimeType;
                }
                Result<String, String> content =
                    (res != nullptr) ? res->reader() : Move(dynamicContent.Value());
                if (!content.HasValue())
                {
                    return detail::Answered(detail::MakeError(id, RpcError::InternalError, Move(content.Error())));
                }
                JsonValue entry = JsonValue::MakeObject();
                entry.Set(u8"uri", JsonValue::MakeString(String(uri)));
                entry.Set(u8"mimeType", JsonValue::MakeString(mimeType));
                entry.Set(u8"text", JsonValue::MakeString(Move(content.Value())));
                JsonValue contents = JsonValue::MakeArray();
                contents.Add(Move(entry));
                JsonValue result = JsonValue::MakeObject();
                result.Set(u8"contents", Move(contents));
                return detail::Answered(detail::MakeResult(id, Move(result)));
            }
            return detail::Answered(detail::MakeError(id, RpcError::MethodNotFound,
                                     Format(u8"method not found: {}", method)));
        }
    };

    /// host_info - the ops-hygiene tool EVERY host registers (from ezEngine's app_info):
    /// pid (a hung host is killed by pid), the build stamp
    /// (stale-binary detection), server + protocol versions, and whatever host-specific state
    /// the host supplies (the stdio host reports its open project). `buildStamp` is the host
    /// executable's BuildStamp() text; `hostState` may be empty.
    inline void RegisterHostInfoTool(McpServer& server, String buildStamp,
                                     Function<JsonValue()> hostState = {})
    {
        McpServer* s = &server;
        server.RegisterTool(
            u8"host_info",
            u8"The host process's identity: pid (kill a hung host by pid), buildStamp (detect a "
            u8"stale binary after a rebuild), server + MCP protocol versions, and host state "
            u8"(e.g. the open project). Read this first in a new session.",
            SchemaBuilder().Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, buildStamp = Move(buildStamp),
             hostState = Move(hostState)](const JsonValue&) -> ToolResult
            {
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"pid",
                        JsonValue::MakeNumber(static_cast<f64>(foundation::core::ProcessId())));
                out.Set(u8"buildStamp", JsonValue::MakeString(buildStamp));
                out.Set(u8"serverName", JsonValue::MakeString(String(s->ServerName())));
                out.Set(u8"serverVersion", JsonValue::MakeString(String(s->ServerVersion())));
                out.Set(u8"protocolVersion", JsonValue::MakeString(String(kProtocolVersion)));
                if (hostState)
                {
                    out.Set(u8"host", hostState());
                }
                return out;
            });
    }
}
