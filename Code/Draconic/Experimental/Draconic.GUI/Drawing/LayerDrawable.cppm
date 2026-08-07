// Draconic GUI - :layer_drawable partition
//
// LayerDrawable: stacks multiple drawables with per-layer insets, drawn in order. This is
// the CSS-`background` compositor role of eepp's UINodeDrawable (a background color plus
// ordered layers): a background color is simply the bottom layer (a RectangleDrawable).
// Full CSS background-position/size/repeat/origin lands with the CSS phase.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:layer_drawable;

import draconic.foundation; // Array, RefPtr, Move, Max
import :rect;
import :thickness;
import :control_state;
import :drawable;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class LayerDrawable : public Drawable
    {
        DRACONIC_OBJECT(LayerDrawable, Drawable)
    public:
        struct Layer
        {
            RefPtr<Drawable> Content;
            Thickness Inset;
        };

        LayerDrawable() = default;

        // Consumes the caller's ref on `drawable` (held by value). Layers draw bottom-first.
        void AddLayer(RefPtr<Drawable> drawable, Thickness inset = {})
        {
            m_layers.PushBack(Layer{Move(drawable), inset});
        }

        [[nodiscard]] usize LayerCount() const noexcept { return m_layers.Size(); }
        void ClearLayers() { m_layers.Clear(); }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            for (const Layer& layer : m_layers)
                if (layer.Content)
                    layer.Content->Draw(ctx, LayerBounds(dest, layer.Inset));
        }

        void Draw(DrawContext& ctx, const Rect& dest, ControlState state) override
        {
            for (const Layer& layer : m_layers)
                if (layer.Content)
                    layer.Content->Draw(ctx, LayerBounds(dest, layer.Inset), state);
        }

    private:
        [[nodiscard]] static Rect LayerBounds(const Rect& b, const Thickness& inset) noexcept
        {
            return Rect{b.x + inset.Left, b.y + inset.Top,
                        foundation::Max(0.0f, b.width - inset.TotalHorizontal()),
                        foundation::Max(0.0f, b.height - inset.TotalVertical())};
        }

        Array<Layer> m_layers;
    };

    DRACONIC_DEFINE_OBJECT(LayerDrawable, "draconic::gui")
}
