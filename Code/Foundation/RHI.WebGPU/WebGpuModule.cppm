// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu - WebGPU RHI backend.
///
/// Written against the standard webgpu.h; on desktop the implementation is the
/// wgpu-native runtime sidecar (dlopen'd, never linked), on web it is the browser's. A
/// NotSupported from this backend is a WebGPU API limit (no binding arrays, polygon
/// modes, 1D arrays or pipeline statistics in core WebGPU), not missing work.

export module foundation.rhi.webgpu;

export import :api;
export import :conversions;
export import :blit_helper;
export import :buffer;
export import :texture;
export import :texture_view;
export import :sampler;
export import :shader_module;
export import :bind_group_layout;
export import :bind_group;
export import :pipeline_layout;
export import :push_constant_emulator;
export import :pipeline_cache;
export import :render_pipeline;
export import :compute_pipeline;
export import :query_set;
export import :command_buffer;
export import :render_pass_encoder;
export import :compute_pass_encoder;
export import :render_bundle_encoder;
export import :command_encoder;
export import :command_pool;
export import :surface;
export import :swapchain;
export import :transfer_batch;
export import :fence;
export import :queue;
export import :device;
export import :adapter;
export import :backend;
