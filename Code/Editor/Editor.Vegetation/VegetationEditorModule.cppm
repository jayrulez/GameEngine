// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.vegetation - the vegetation editing module's public interface: the Paint Vegetation
// viewport brush, its viewport-tool provider and the panel registrar, and the vegetation mask page.

export module editor.vegetation;

export import :pick;    // VegetationPick: the terrain footprint under the cursor (both brushes)
export import :paint;   // VegetationPaintTool, RegisterVegetationViewportTools, RegisterVegetationToolPanels
export import :scatter; // VegetationScatterTool (Paint Props)
export import :mask_page; // VegetationMaskPage
