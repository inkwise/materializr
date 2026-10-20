#pragma once
#include "SketchExportOptions.h"
#include <string>

namespace materializr {

class Sketch;

struct SvgExportResult {
    bool success = false;
    std::string errorMessage;
    int curveCount = 0;
    int fillCount = 0;   // filled regions written (Fill tool)
};

class SvgExport {
public:
    // Export a sketch's non-construction geometry to a 1:1-millimeter SVG,
    // intended for laser cutters / 2.5D CNC. Curves are polyline-approximated
    // (lines exact; circles/arcs/splines tessellated). Y is flipped to SVG's
    // top-left origin so the part isn't mirrored. Construction geometry is
    // skipped (reference-only, not cut).
    //
    // Regions marked with the sketch Fill tool are written first as solid
    // paths (fill-rule evenodd, so holes stay open, no stroke) - the shapes a
    // laser fill-engraves - with the outlines drawn over them. Grey by
    // default (opts.greyFills), so they separate from the black outlines.
    static SvgExportResult exportSketch(const std::string& filePath, const Sketch& sketch,
                                        const SketchExportOptions& opts = {});
};

} // namespace materializr
