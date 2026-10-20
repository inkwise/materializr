#pragma once

namespace materializr {

// Choices offered when a sketch is exported to SVG or DXF. Shared by both
// exporters so the dialog drives them identically.
struct SketchExportOptions {
    // Regions marked with the Fill tool go out grey, so they stay distinct
    // from the black outlines (laser software hands each colour its own
    // operation: cut the outlines, engrave the grey). Off keeps the fill the
    // same colour as the outlines (SVG: black; DXF: the layer's default).
    // DXF additionally always puts fills on their own FILL layer.
    bool greyFills = true;
};

} // namespace materializr
