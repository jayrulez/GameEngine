// Draconic Editor App - :asset_picker_slot partition
//
// A reference "slot": a button that shows the current asset's name (or a placeholder like "None"),
// clickable (via the inherited OnClick) to open a type-filtered asset picker the consumer wires.
// Reusable across asset-ref pickers. The reserved preview drawable is where an asset THUMBNAIL will
// render alongside the text in future (which is why this lives in the editor-app layer, not the
// asset-agnostic UI framework). For now it renders exactly like its text-button base.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:asset_picker_slot;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;
namespace ui = draconic::ui;

export namespace draconic::editor::app
{
    class AssetPickerSlot : public ui::Button
    {
        DRACONIC_OBJECT(AssetPickerSlot, ui::Button)
    public:
        explicit AssetPickerSlot(StringView text) : ui::Button(text) {}

        /// Reserved: an asset thumbnail to render alongside the text (not yet drawn).
        void SetPreview(ui::SVGDrawable* preview) { m_preview = preview; }

    private:
        [[maybe_unused]] ui::SVGDrawable* m_preview = nullptr; // future: thumbnail preview
    };

    DRACONIC_DEFINE_OBJECT(AssetPickerSlot, "draconic::editor::app")
}
