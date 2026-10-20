#pragma once
#include "SketchExportOptions.h"
#include <string>

namespace materializr {

class Sketch;

struct DxfExportResult {
    bool success = false;
    std::string errorMessage;
    int entityCount = 0;   // every entity written, fills included
    int fillCount = 0;     // closed loops written for filled regions
};

class DxfExport {
public:
    // Export a sketch's non-construction geometry as R12 ASCII DXF in
    // millimeters - the format laser cutters and CAM tools expect. Lines,
    // circles and arcs are exact entities; splines are sampled POLYLINEs.
    // Sketch coordinates go out as-is (DXF is Y-up like the sketch plane).
    //
    // Regions marked with the sketch Fill tool go out as closed POLYLINEs
    // (outer boundary plus each hole) on their own FILL layer: R12 has no
    // solid fill, and a layer is what laser software keys an operation to.
    // Grey (ACI 8) by default so they separate from the outlines on layer 0.
    static DxfExportResult exportSketch(const std::string& filePath, const Sketch& sketch,
                                        const SketchExportOptions& opts = {});
};

} // namespace materializr
