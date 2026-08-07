// Draconic::VG::SVG - :types partition.
//
// SVG element/document model: SVGElementType, SVGTextAnchor, SVGElement (a parsed
// shape/group/text node with a tessellated Path + style), SVGDocument. Ported
// from Sedulous.VG.SVG (SVGElementType/SVGElement/SVGDocument). Element trees are
// value types (children owned by value); the transform is a Float4x4 (mirroring
// Sedulous's Matrix); colors are float Color.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg.svg:types;

import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;

export namespace draconic::vg::svg
{
    /// Types of SVG elements.
    enum class SVGElementType
    {
        Path,
        Group,
        Rectangle,
        Circle,
        Ellipse,
        Line,
        Polygon,
        Polyline,
        Text
    };

    /// Text anchor alignment (maps to the SVG text-anchor attribute).
    enum class SVGTextAnchor
    {
        Start,
        Middle,
        End
    };

    /// A parsed <linearGradient>/<radialGradient>: geometry + stops + spread, resolved
    /// by the renderer against the referencing element's bounds (objectBoundingBox, the
    /// SVG default) or used as document coordinates (userSpaceOnUse).
    struct SVGGradient
    {
        bool radial = false;
        // Linear geometry (fractions in objectBoundingBox units, else user units).
        f32 x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 0.0f;
        // Radial geometry (focal point unsupported - center-only, like the VG fill).
        f32 cx = 0.5f, cy = 0.5f, r = 0.5f;
        bool userSpace = false; // gradientUnits="userSpaceOnUse"
        draconic::vg::VGGradientSpread spread = draconic::vg::VGGradientSpread::Pad;
        Array<draconic::vg::GradientStop> stops;
    };

    /// A parsed SVG element.
    class SVGElement
    {
    public:
        SVGElementType type = SVGElementType::Path;
        Optional<draconic::vg::Path> path;         ///< Tessellatable geometry (shapes/paths).
        Float4x4 transform = Float4x4::Identity(); ///< Element transform.
        Optional<Color> fillColor;                 ///< Fill color (empty = none/inherit).
        String fillGradientId; ///< Set when fill="url(#id)" - resolved via the document.
        Optional<Color> strokeColor;               ///< Stroke color (empty = none).
        f32 strokeWidth = 1.0f;
        f32 opacity = 1.0f;
        Array<SVGElement> children; ///< Children (for group elements).

        // Text-specific fields.
        String textContent;
        f32 textX = 0.0f;
        f32 textY = 0.0f;
        f32 fontSize = 16.0f;
        SVGTextAnchor textAnchor = SVGTextAnchor::Start;
        bool fontBold = false;

        SVGElement() = default;
        explicit SVGElement(SVGElementType inType) : type(inType) {}

        /// Whether this element is a non-empty group.
        [[nodiscard]] bool IsGroup() const
        {
            return type == SVGElementType::Group && !children.IsEmpty();
        }
    };

    /// A parsed SVG document.
    class SVGDocument
    {
    public:
        f32 width = 0.0f;
        f32 height = 0.0f;
        Array<SVGElement> elements;
        HashMap<String, SVGGradient> gradients; ///< By id (from <defs> or inline).

        SVGDocument() = default;
        SVGDocument(f32 inWidth, f32 inHeight) : width(inWidth), height(inHeight) {}
    };
}
