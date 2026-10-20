#include "DxfExport.h"
#include "../modeling/Sketch.h"

#include <cmath>
#include <cstdio>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {
// AutoCAD Color Index: 7 = white/black (whichever contrasts), 8 = dark grey.
constexpr int kAciDefault = 7;
constexpr int kAciGrey = 8;
} // namespace

// R12 (AC1009) ASCII: the lowest common denominator every laser-cutter and
// CAM package reads. Minimal file = HEADER ($ACADVER + $INSUNITS mm) and an
// ENTITIES section - R12 needs no TABLES for entities on layer 0. A TABLES
// section (LTYPE + LAYER) is added only when there are fills, to name the
// FILL layer and its colour.
DxfExportResult DxfExport::exportSketch(const std::string& filePath, const Sketch& sketch,
                                        const SketchExportOptions& opts) {
    DxfExportResult result;

    // Filled regions as closed loops (outer + holes), resolved before the
    // file is opened so a sketch with nothing to write never leaves one.
    std::vector<std::vector<std::vector<glm::vec2>>> fills;
    if (sketch.hasFills()) {
        const auto regions = sketch.buildRegions();
        for (int idx : sketch.filledRegions(regions)) {
            auto loops = sketch.regionLoops2D(regions[idx]);
            if (!loops.empty()) fills.push_back(std::move(loops));
        }
    }

    std::FILE* f = std::fopen(filePath.c_str(), "wb");
    if (!f) {
        result.errorMessage = "Could not open file for writing: " + filePath;
        return result;
    }

    // Group-code/value pair - DXF is just alternating lines of these.
    auto tag = [&](int code, const char* v) { std::fprintf(f, "%d\n%s\n", code, v); };
    auto tagd = [&](int code, double v) { std::fprintf(f, "%d\n%.6f\n", code, v); };
    auto tagi = [&](int code, int v) { std::fprintf(f, "%d\n%d\n", code, v); };

    tag(0, "SECTION"); tag(2, "HEADER");
    tag(9, "$ACADVER"); tag(1, "AC1009");
    tag(9, "$INSUNITS"); tagi(70, 4); // 4 = millimeters
    tag(0, "ENDSEC");

    const int fillAci = opts.greyFills ? kAciGrey : kAciDefault;
    if (!fills.empty()) {
        tag(0, "SECTION"); tag(2, "TABLES");
        tag(0, "TABLE"); tag(2, "LTYPE"); tagi(70, 1);
        tag(0, "LTYPE"); tag(2, "CONTINUOUS"); tagi(70, 0);
        tag(3, "Solid line"); tagi(72, 65); tagi(73, 0); tagd(40, 0.0);
        tag(0, "ENDTAB");
        tag(0, "TABLE"); tag(2, "LAYER"); tagi(70, 2);
        tag(0, "LAYER"); tag(2, "0");    tagi(70, 0); tagi(62, kAciDefault); tag(6, "CONTINUOUS");
        tag(0, "LAYER"); tag(2, "FILL"); tagi(70, 0); tagi(62, fillAci);     tag(6, "CONTINUOUS");
        tag(0, "ENDTAB");
        tag(0, "ENDSEC");
    }

    tag(0, "SECTION"); tag(2, "ENTITIES");

    int count = 0;

    for (const auto& l : sketch.getLines()) {
        if (l.isConstruction) continue;
        const SketchPoint* a = sketch.getPoint(l.startPointId);
        const SketchPoint* b = sketch.getPoint(l.endPointId);
        if (!a || !b) continue;
        tag(0, "LINE"); tag(8, "0");
        tagd(10, a->pos.x); tagd(20, a->pos.y); tagd(30, 0.0);
        tagd(11, b->pos.x); tagd(21, b->pos.y); tagd(31, 0.0);
        ++count;
    }

    for (const auto& c : sketch.getCircles()) {
        if (c.isConstruction) continue;
        const SketchPoint* ctr = sketch.getPoint(c.centerPointId);
        if (!ctr || c.radius <= 0.0) continue;
        tag(0, "CIRCLE"); tag(8, "0");
        tagd(10, ctr->pos.x); tagd(20, ctr->pos.y); tagd(30, 0.0);
        tagd(40, c.radius);
        ++count;
    }

    for (const auto& a : sketch.getArcs()) {
        if (a.isConstruction) continue;
        const SketchPoint* ctr = sketch.getPoint(a.centerPointId);
        const SketchPoint* s = sketch.getPoint(a.startPointId);
        const SketchPoint* e = sketch.getPoint(a.endPointId);
        if (!ctr || !s || !e || a.radius <= 0.0) continue;
        // Sketch arcs run CCW start→end - exactly DXF's ARC convention
        // (group 50 = start angle, 51 = end angle, degrees CCW from +X).
        double a0 = std::atan2(s->pos.y - ctr->pos.y, s->pos.x - ctr->pos.x) * 180.0 / M_PI;
        double a1 = std::atan2(e->pos.y - ctr->pos.y, e->pos.x - ctr->pos.x) * 180.0 / M_PI;
        tag(0, "ARC"); tag(8, "0");
        tagd(10, ctr->pos.x); tagd(20, ctr->pos.y); tagd(30, 0.0);
        tagd(40, a.radius);
        tagd(50, a0); tagd(51, a1);
        ++count;
    }

    for (const auto& sp : sketch.getSplines()) {
        if (sp.isConstruction) continue;
        if (sp.controlPointIds.size() < 2) continue;
        auto pts = sketch.sampleSpline2D(sp, 64);
        if (pts.size() < 2) continue;
        // R12 POLYLINE + VERTEX stream (LWPOLYLINE is R13+).
        tag(0, "POLYLINE"); tag(8, "0"); tagi(66, 1); tagi(70, 0);
        for (const glm::vec2& p : pts) {
            tag(0, "VERTEX"); tag(8, "0");
            tagd(10, p.x); tagd(20, p.y); tagd(30, 0.0);
        }
        tag(0, "SEQEND");
        ++count;
    }

    // Fills go last. Each loop is a CLOSED polyline (flag 70 = 1) and traces
    // the same boundary as the outlines above, which is why it lives on its
    // own layer: a reader can switch it off or give it a different operation.
    // The colour is also set on the entity itself, because not every reader
    // consults the layer table for it.
    for (const auto& loops : fills) {
        for (const auto& loop : loops) {
            tag(0, "POLYLINE"); tag(8, "FILL");
            if (opts.greyFills) tagi(62, kAciGrey);
            tagi(66, 1); tagi(70, 1);
            for (const glm::vec2& p : loop) {
                tag(0, "VERTEX"); tag(8, "FILL");
                tagd(10, p.x); tagd(20, p.y); tagd(30, 0.0);
            }
            tag(0, "SEQEND"); tag(8, "FILL");
            ++count;
            ++result.fillCount;
        }
    }

    tag(0, "ENDSEC");
    tag(0, "EOF");
    std::fclose(f);

    if (count == 0) {
        result.errorMessage = "Sketch has no exportable (non-construction) geometry.";
        std::remove(filePath.c_str());
        return result;
    }
    result.success = true;
    result.entityCount = count;
    return result;
}

} // namespace materializr
