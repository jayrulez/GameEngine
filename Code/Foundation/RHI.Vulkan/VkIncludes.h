// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#ifndef DRACO_RHI_VK_INCLUDES_H_
#define DRACO_RHI_VK_INCLUDES_H_

#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

// After both includes, so it holds even if vulkan_win32.h (or anything added later) pulls
// <windows.h> back in. winnt.h defines MemoryBarrier as an object-like macro (__faststorefence
// on x64), which eats the RHI's `MemoryBarrier` struct (RHI/Descriptors.cppm) in any TU that
// includes both: "expected ';' after expression", then "unknown type name '__faststorefence'".
// WIN32_LEAN_AND_MEAN and NOMINMAX are set above for the same reason: this header makes
// <windows.h> safe for the rest of the build. Nothing here wants the fence intrinsic; Core's
// :atomic owns memory ordering.
#ifdef _WIN32
#undef MemoryBarrier
#endif

#endif // DRACO_RHI_VK_INCLUDES_H_
