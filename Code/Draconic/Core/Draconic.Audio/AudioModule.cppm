// Draconic::Audio - the `draconic.audio` module.
//
// The engine wrapper over the vendored miniaudio (docs/design/audio.md): AudioEngine
// (device + node graph + resource manager), the default Master<-{Effects,Music,UI} bus
// groups, the fixed voice pool with generation-checked handles, priority stealing +
// recent-play dedupe, always-fade stop/pause, the ma_vfs -> draconic.vfs stream bridge,
// and a headless Null mode. miniaudio is the committed backend with NO abstraction
// layer, but ma_* types never cross the public surface. Cooked clip records + the
// resource factory live in draconic.audio.resource; scene integration lives in
// draconic.engine.audio.

export module draconic.audio;

export import :clip;
export import :cue;
export import :engine;
export import :reverb;
