// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Portable push-constant declaration.
//
//   PUSH_CONSTANT(TonemapPush, pc, space1);
//
// expands per target so one source line serves every backend:
//
//   Native (default): Vulkan, DX12, and wgpu-native WebGPU:
//     [[vk::push_constant]] ConstantBuffer<TonemapPush> pc : register(b0, space1)
//     DXC lowers the vk attribute to a SPIR-V push-constant block (Vulkan push constants; naga
//     turns it into a WGSL var<immediate>, which wgpu-native's Immediates feature serves). On DXIL
//     the attribute is ignored and it is a plain cbuffer at (b0, space1) that the DX12 root
//     signature places as root constants.
//
//   Browser WebGPU (define PUSH_CONSTANT_AS_CBUFFER, set by the web cook):
//     ConstantBuffer<TonemapPush> pc : register(b0, space1)
//     Browser WebGPU has no push constants or immediates (pipeline layouts with push-constant
//     ranges fail), so the block becomes an ordinary uniform buffer. Under the engine's binding
//     shift scheme (CBV shift 0) the `space` becomes its own bind group, @group(space)
//     @binding(0): a reserved slot the browser pipeline layout binds as a uniform buffer and
//     the encoder feeds through a per-draw uniform-buffer write instead of SetImmediates.
//
// The `space` argument is passed as a token (space0, space1, ...) and keeps each shader's
// placement, so the DX12 root-signature and Vulkan set assignments follow the shader. The
// register is always b0 (a shader has a single push-constant block). There is no trailing
// semicolon: write one at the call site.

#ifndef PUSH_CONSTANT_HLSLI
#define PUSH_CONSTANT_HLSLI

#ifdef PUSH_CONSTANT_AS_CBUFFER
#define PUSH_CONSTANT(Type, name, space) ConstantBuffer<Type> name : register(b0, space)
#else
#define PUSH_CONSTANT(Type, name, space)                                                            \
    [[vk::push_constant]] ConstantBuffer<Type> name : register(b0, space)
#endif

#endif // PUSH_CONSTANT_HLSLI
