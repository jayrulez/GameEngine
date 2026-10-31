// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Audio - the `foundation.audio` module.
//
// The engine's wrapper over the vendored miniaudio: AudioEngine (device, node graph and
// resource manager), the fixed bus groups (Effects, Music and UI under Master), the fixed
// voice pool with generation-checked handles, priority stealing and recent-play dedupe,
// fading stop and pause, the stream bridge from ma_vfs to foundation.vfs, and a headless
// mode. miniaudio is the only backend, with no abstraction layer, but ma_* types never cross
// the public surface. Cooked clip records and the resource factory live in
// foundation.audio.resource; scene integration lives in engine.audio.

export module foundation.audio;

export import :clip;
export import :cue;
export import :engine;
export import :reverb;
