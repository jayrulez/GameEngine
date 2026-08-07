// Draconic UI - :drag_data partition
//
// Base class for drag-and-drop payload data. Subclass to carry typed data; the Format string enables
// type matching between drag sources and drop targets. Ported from Sedulous.UI/src/DragDrop/DragData.bf.
// Per the locked decision, DragData derives Object + DRACONIC_OBJECT so subtype recovery uses our RTTI
// (Cast<T>) and the manager can own it via RefPtr (Beef `~delete _` -> RAII).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:drag_data;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Base class for drag-and-drop payload data.
    class DragData : public Object
    {
        DRACONIC_OBJECT(DragData, Object)
    public:
        explicit DragData(StringView format) : m_format(format) {}

        /// Format string for type identification (e.g. "view/reorder", "text/plain").
        [[nodiscard]] StringView Format() const { return m_format.AsView(); }

    private:
        String m_format;
    };

    DRACONIC_DEFINE_OBJECT(DragData, "draconic::ui")
}
