// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer`: the model viewer tool's (Tools.ModelViewer) logic, kept apart from
// its window so it can be tested headless.

module;
#include "Core/Prelude.h"

export module modelviewer;

export import editor.modelpreview; // opening a model file into the viewer's cache, and its clips' playback
export import :tabs;    // the open models, one tab each, and the side panel's rows
export import :browse;   // folder listing, model collection, thumbnail files, the filters
export import :shortlist; // starred models, their notes, and the CSV
export import :store;     // the library: entries, nested collections, member names, tags
export import :search;    // library search
export import :commands;  // every operation as a command, the session, the undo history
export import :mcp;       // the commands as MCP tools, and the agent's guide
export import :window;    // the window's commands, and their windowless stand-ins
export import :loads;     // model files read on a worker thread
export import :settings;  // the preferences and recents, and their commands
export import :exports;   // a collection as CSV, or its files copied out
