// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebGPU headers for the global module fragment (module-partition rule: the GMF may
// contain only #includes). webgpu.h is the standard C header the backend is written
// against; wgpu.h adds wgpu-native's extensions (adapter enumeration, DevicePoll,
// SPIR-V shader ingestion, log callback), which exist only in the desktop sidecar,
// never the browser.
//
// On web the standard webgpu.h comes from the emdawnwebgpu Emscripten port (Dawn's
// header, same spec); wgpu-native's wgpu.h does not exist there, so the extensions it
// declares are compiled out (see WebGpuApi's NATIVE_EXT split).
#ifndef FOUNDATION_RHI_WEBGPU_INCLUDES_H
#define FOUNDATION_RHI_WEBGPU_INCLUDES_H

#include "Core/Prelude.h"

#include <webgpu/webgpu.h>
#if !PLATFORM_WEB
#include <webgpu/wgpu.h>
#endif

#endif // FOUNDATION_RHI_WEBGPU_INCLUDES_H
