// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :layer_drawable partition
//
// Stacks multiple drawables with per-layer insets, drawn in order. Ported from
// Sedulous.UI/src/Drawing/LayerDrawable.bf. Layers own a RefPtr<Drawable> (auto-released).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:layer_drawable;

import foundation.core; // Array, RefPtr, Rectangle, Max
import :thickness;
import :control_state;
import :drawable;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class LayerDrawable : public Drawable
    {
        RTTI_OBJECT(LayerDrawable, Drawable)
    public:
        struct Layer
        {
            RefPtr<Drawable> Content;
            Thickness Inset;
        };

        LayerDrawable() = default;

        /// Consumes the caller's ref on `drawable` (held by value).
        void AddLayer(RefPtr<Drawable> drawable, Thickness inset = {})
        {
            m_layers.PushBack(Layer{Move(drawable), inset});
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            for (const Layer& layer : m_layers)
            {
                if (layer.Content)
                {
                    layer.Content->Draw(ctx, LayerBounds(bounds, layer.Inset));
                }
            }
        }
        void DrawState(UIDrawContext& ctx, const Rectangle& bounds, ControlState state) override
        {
            for (const Layer& layer : m_layers)
            {
                if (layer.Content)
                {
                    layer.Content->Draw(ctx, LayerBounds(bounds, layer.Inset), state);
                }
            }
        }

    private:
        [[nodiscard]] static Rectangle LayerBounds(const Rectangle& b,
                                                   const Thickness& inset) noexcept
        {
            return Rectangle{b.x + inset.Left, b.y + inset.Top,
                             Max(0.0f, b.width - inset.TotalHorizontal()),
                             Max(0.0f, b.height - inset.TotalVertical())};
        }

        Array<Layer> m_layers;
    };

    RTTI_DEFINE_OBJECT(LayerDrawable, "rtti::ui")
}
