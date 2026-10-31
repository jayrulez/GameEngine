// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter - the `modelimporter` umbrella module.
///
/// The top-level model import pipeline: it converts a loaded `foundation.model` Model
/// (the loader IR) into the engine's cooked *Source types and cooks them through the
/// editor stack into a content database. Import preserves the hierarchy (each node becomes
/// an entity) rather than merging the meshes.
export module modelimporter;

export import foundation.model.resource; // the cooked-model runtime types
export import :mesh_convert;
export import :anim_convert;
export import :cook;
export import :file_import;
export import :load;
