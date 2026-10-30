// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer`: what the model viewer tool (Tools.ModelViewer) does apart from its
// window, so it is tested headless (Documentation/Specs/model-viewer.md).

module;
#include "Core/Prelude.h"

export module modelviewer;

export import :library; // a model file opened, cooked into the viewer's cache and bound
export import :tabs;    // the models open, a tab each, and the side panel's rows
export import :playback; // the animation bar's clip, play, pause, step, scrub, loop and speed
export import :browse;   // folders listed, models collected, thumbnails' files, the filters
export import :shortlist; // the models starred, their notes, the CSV
export import :store;     // the library: entries, collections that nest, members' names, tags
export import :search;    // the library searched
export import :commands;  // every operation as a command, the session, the undo history
export import :mcp;       // the commands as MCP tools, and the agent's guide
export import :window;    // the window's commands, and their windowless stand-ins
export import :loads;     // model files read on a thread of their own
export import :settings;
export import :exports;   // a collection as CSV, or its files copied out  // the preferences and recents, and their commands
