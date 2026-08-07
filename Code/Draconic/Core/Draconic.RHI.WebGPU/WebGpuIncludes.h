// WebGPU headers for the global module fragment (module-partition rule: the GMF may
// contain ONLY #includes). webgpu.h is the STANDARD C header the backend is written
// against; wgpu.h adds wgpu-native's extensions (adapter enumeration, DevicePoll,
// SPIR-V shader ingestion, log callback) - desktop-sidecar-only, never the browser.
//
// On web the standard webgpu.h comes from the emdawnwebgpu Emscripten port (Dawn's
// header, same spec); wgpu-native's wgpu.h does not exist there, so the extensions it
// declares are compiled out (see WebGpuApi's NATIVE_EXT split).
#ifndef DRACONIC_RHI_WEBGPU_INCLUDES_H
#define DRACONIC_RHI_WEBGPU_INCLUDES_H

#include "Draconic.Foundation/Prelude.h"

#include <webgpu/webgpu.h>
#if !DRACONIC_PLATFORM_WEB
#include <webgpu/wgpu.h>
#endif

#endif // DRACONIC_RHI_WEBGPU_INCLUDES_H
