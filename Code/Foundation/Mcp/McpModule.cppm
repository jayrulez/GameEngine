// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp - `foundation.mcp` (JSON-RPC 2.0, the MCP lifecycle, a tool and resource registry).
//
// The protocol layer, over foundation.json. It is transport-independent (stdio and an in-memory
// transport here, HTTP in foundation.mcp.http); tools and resources register against a McpServer.
// It implements a subset of MCP (initialize, tools, resources, ping); hosts (the headless
// Tools.Mcp, the editor) sit on top and contribute tools.

export module foundation.mcp;

export import :schema;
export import :server;
export import :transport;
