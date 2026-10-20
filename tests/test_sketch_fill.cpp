// Sketch Fill tool: cosmetic region fills for SVG export (laser engraving).
//
// The contract pinned here:
//   - a fill toggles per region and is keyed by an anchor point, so it
//     follows its region through a renumbering region build;
//   - it survives save/load and the SketchEditOp undo snapshots;
//   - it changes NOTHING modeling reads (region key, profile shape);
//   - SVG export writes each filled region as one solid evenodd path with
//     its holes as subpaths, and leaves unfilled regions alone.

#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "core/Document.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "io/SvgExport.h"
#include "io/DxfExport.h"
#include "io/DxfImport.h"

#include <gtest/gtest.h>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include "test_tmp_path.h"

using materializr::ProjectIO;
using materializr::Sketch;
using materializr::SketchEditOp;
using materializr::SvgExport;
using materializr::DxfExport;
using materializr::SketchExportOptions;

namespace {

void addRect(Sketch& sk, float x0, float y0, float x1, float y1) {
    const int a = sk.addPoint({x0, y0}), b = sk.addPoint({x1, y0});
    const int c = sk.addPoint({x1, y1}), d = sk.addPoint({x0, y1});
    sk.addLine(a, b); sk.addLine(b, c); sk.addLine(c, d); sk.addLine(d, a);
}

// A 40x20 plate with a 10x10 window: two regions, the frame (with a hole)
// and the window.
Sketch frameWithWindow() {
    Sketch sk;
    addRect(sk, 0, 0, 40, 20);
    addRect(sk, 15, 5, 25, 15);
    return sk;
}

int regionContaining(const Sketch& sk, const std::vector<Sketch::Region>& regions,
                     glm::vec2 p) {
    for (size_t i = 0; i < regions.size(); ++i)
        if (sk.isPointInRegion(regions[i], p)) return static_cast<int>(i);
    return -1;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

} // namespace

TEST(SketchFill, TogglesPerRegion) {
    Sketch sk = frameWithWindow();
    const auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u);
    const int frame = regionContaining(sk, regions, {5, 10});
    const int window = regionContaining(sk, regions, {20, 10});
    ASSERT_GE(frame, 0);
    ASSERT_GE(window, 0);

    EXPECT_TRUE(sk.toggleFill(regions, frame));
    EXPECT_EQ(sk.filledRegions(regions), std::vector<int>{frame});
    EXPECT_TRUE(sk.toggleFill(regions, window));
    EXPECT_EQ(sk.filledRegions(regions).size(), 2u);

    EXPECT_FALSE(sk.toggleFill(regions, frame)) << "a second click clears";
    EXPECT_EQ(sk.filledRegions(regions), std::vector<int>{window});
    EXPECT_EQ(sk.getFillAnchors().size(), 1u);
    EXPECT_EQ(sk.filledRegionsCached(), std::vector<int>{window});
}

TEST(SketchFill, ChangesNothingModelingReads) {
    Sketch plain = frameWithWindow();
    Sketch filled = frameWithWindow();
    const auto regions = filled.buildRegions();
    filled.toggleFill(regions, regionContaining(filled, regions, {5, 10}));

    // Same region key: a fill must not invalidate the region cache or look
    // like a geometry edit to anything keyed on it.
    EXPECT_EQ(plain.regionKey(), filled.regionKey());
    EXPECT_TRUE(filled.regionsCached());

    // Same extrudable profile area.
    auto area = [](const TopoDS_Shape& s) {
        GProp_GProps p;
        BRepGProp::SurfaceProperties(s, p);
        return p.Mass();
    };
    EXPECT_NEAR(area(plain.buildProfileShape()), area(filled.buildProfileShape()), 1e-6);
}

TEST(SketchFill, FollowsItsRegionThroughARenumber) {
    Sketch sk = frameWithWindow();
    auto regions = sk.buildRegions();
    const int window = regionContaining(sk, regions, {20, 10});
    sk.toggleFill(regions, window);

    // Add an unrelated shape: the region build changes, indices may shift.
    addRect(sk, 50, 0, 60, 10);
    regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 3u);
    EXPECT_EQ(sk.filledRegions(regions),
              std::vector<int>{regionContaining(sk, regions, {20, 10})});
}

TEST(SketchFill, SurvivesSaveAndLoad) {
    Sketch sk = frameWithWindow();
    const auto regions = sk.buildRegions();
    sk.toggleFill(regions, regionContaining(sk, regions, {20, 10}));

    std::ostringstream out;
    ProjectIO::writeSketchBody(out, sk);
    Sketch reloaded;
    std::istringstream in(out.str());
    ProjectIO::parseSketchBody(in, reloaded, "SKETCH_END");

    const auto r2 = reloaded.buildRegions();
    EXPECT_EQ(reloaded.filledRegions(r2),
              std::vector<int>{regionContaining(reloaded, r2, {20, 10})});
}

TEST(SketchFill, OldFilesLoadUnfilled) {
    // A body with no FILL block at all - every file written before the tool.
    Sketch sk = frameWithWindow();
    std::ostringstream out;
    ProjectIO::writeSketchBody(out, sk);
    std::string text = out.str();
    const size_t at = text.find("FILL_COUNT");
    ASSERT_NE(at, std::string::npos);
    text.erase(at, text.find("SKETCH_END") - at);

    Sketch reloaded;
    std::istringstream in(text);
    ProjectIO::parseSketchBody(in, reloaded, "SKETCH_END");
    EXPECT_FALSE(reloaded.hasFills());
    EXPECT_EQ(reloaded.getLines().size(), 8u);
}

TEST(SketchFill, UndoSnapshotsKeepTheFillAcrossReload) {
    // The fill toggle is a SketchEditOp; its before/after snapshots are what
    // undo restores after a reload, so they must carry the anchors.
    Document doc;
    auto live = std::make_shared<Sketch>(frameWithWindow());
    const int sid = doc.addSketch(live, "Plate");
    auto before = std::make_shared<Sketch>(*live);
    const auto regions = live->buildRegions();
    live->toggleFill(regions, regionContaining(*live, regions, {20, 10}));
    auto after = std::make_shared<Sketch>(*live);

    SketchEditOp op(live, before, after);
    op.setSketchId(sid);
    EXPECT_EQ(op.description(), "Fill region");
    EXPECT_TRUE(op.onlyFillsChanged());

    // ...so undoing/redoing it never cascades into a body rebuild.
    int cascades = 0;
    materializr::dispatchUndoRedoCascade(0, &op, doc, sid, [&](int) { ++cascades; });
    EXPECT_EQ(cascades, 0);

    // A real geometry edit still does.
    auto moved = std::make_shared<Sketch>(*after);
    moved->movePoint(moved->getPoints().front().id, {1.0f, 1.0f});
    SketchEditOp geo(live, after, moved);
    EXPECT_FALSE(geo.onlyFillsChanged());
    materializr::dispatchUndoRedoCascade(0, &geo, doc, -1, [&](int) { ++cascades; });
    EXPECT_EQ(cascades, 1);
    auto back = ProjectIO::rehydrateSketchEditOp(op.serializeWithDocument(doc), doc);
    ASSERT_NE(back, nullptr);
    EXPECT_FALSE(back->getBeforeSnapshot()->hasFills());
    EXPECT_EQ(back->getAfterSnapshot()->getFillAnchors().size(), 1u);
}

TEST(SketchFill, SvgExportWritesFilledRegionsOnly) {
    Sketch sk = frameWithWindow();
    const auto regions = sk.buildRegions();
    sk.toggleFill(regions, regionContaining(sk, regions, {5, 10}));   // the frame

    const std::string path = mzrtest::tmpPath("mzr_sketch_fill.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    const std::string svg = readFile(path);
    std::remove(path.c_str());

    EXPECT_EQ(res.fillCount, 1);
    EXPECT_EQ(count(svg, "<g id=\"fills\""), 1);
    EXPECT_NE(svg.find("fill-rule=\"evenodd\""), std::string::npos);
    // The frame's path carries its outer loop AND the window as a hole, so
    // the window stays open under evenodd.
    const size_t g = svg.find("<g id=\"fills\"");
    const size_t gEnd = svg.find("</g>", g);
    const std::string group = svg.substr(g, gEnd - g);
    EXPECT_EQ(count(group, "<path"), 1);
    EXPECT_EQ(count(group, "M"), 2) << "outer loop + one hole subpath";
    EXPECT_EQ(count(group, "Z"), 2);
    // Outlines are still exported as before.
    EXPECT_EQ(res.curveCount, 2);
}

TEST(SketchFill, SvgExportWithoutFillsIsUnchanged) {
    Sketch sk = frameWithWindow();
    const std::string path = mzrtest::tmpPath("mzr_sketch_nofill.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    const std::string svg = readFile(path);
    std::remove(path.c_str());
    EXPECT_EQ(res.fillCount, 0);
    EXPECT_EQ(svg.find("fills"), std::string::npos);
}

TEST(SketchFill, SvgFillsAreGreyUnlessTurnedOff) {
    Sketch sk = frameWithWindow();
    const auto regions = sk.buildRegions();
    sk.toggleFill(regions, regionContaining(sk, regions, {5, 10}));
    const std::string path = mzrtest::tmpPath("mzr_sketch_fill_grey.svg");

    ASSERT_TRUE(SvgExport::exportSketch(path, sk).success);   // default: grey
    std::string svg = readFile(path);
    EXPECT_NE(svg.find("<g id=\"fills\" fill=\"#808080\""), std::string::npos);
    // Outlines stay black, so the two separate on import.
    EXPECT_NE(svg.find("stroke=\"#000000\""), std::string::npos);

    SketchExportOptions black;
    black.greyFills = false;
    ASSERT_TRUE(SvgExport::exportSketch(path, sk, black).success);
    svg = readFile(path);
    std::remove(path.c_str());
    EXPECT_NE(svg.find("<g id=\"fills\" fill=\"#000000\""), std::string::npos);
    EXPECT_EQ(svg.find("#808080"), std::string::npos);
}

TEST(SketchFill, DxfFillsAreClosedLoopsOnTheirOwnGreyLayer) {
    Sketch sk = frameWithWindow();
    const auto regions = sk.buildRegions();
    sk.toggleFill(regions, regionContaining(sk, regions, {5, 10}));   // frame + hole
    const std::string path = mzrtest::tmpPath("mzr_sketch_fill.dxf");

    auto res = DxfExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    std::string dxf = readFile(path);
    EXPECT_EQ(res.fillCount, 2) << "outer loop + the window as a hole";
    EXPECT_EQ(res.entityCount, 8 + 2) << "8 outline lines + 2 fill loops";
    // The FILL layer is declared, grey (ACI 8), and the loops sit on it.
    EXPECT_NE(dxf.find("\nFILL\n70\n0\n62\n8\n"), std::string::npos);
    EXPECT_EQ(count(dxf, "POLYLINE\n8\nFILL\n62\n8\n66\n1\n70\n1\n"), 2)
        << "closed (70=1), grey, on layer FILL";
    EXPECT_EQ(count(dxf, "LINE\n8\n0\n"), 8) << "outlines unchanged on layer 0";

    // Off: same layer and loops, but no grey override (default colour 7).
    SketchExportOptions plain;
    plain.greyFills = false;
    ASSERT_TRUE(DxfExport::exportSketch(path, sk, plain).success);
    dxf = readFile(path);
    EXPECT_NE(dxf.find("\nFILL\n70\n0\n62\n7\n"), std::string::npos);
    EXPECT_EQ(count(dxf, "POLYLINE\n8\nFILL\n66\n1\n70\n1\n"), 2);

    // The layer table must not upset our own importer.
    Sketch back;
    EXPECT_TRUE(materializr::DxfImport::importFile(path, back).success);
    std::remove(path.c_str());
}

TEST(SketchFill, DxfWithoutFillsIsUnchanged) {
    Sketch sk = frameWithWindow();
    const std::string path = mzrtest::tmpPath("mzr_sketch_nofill.dxf");
    auto res = DxfExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    const std::string dxf = readFile(path);
    std::remove(path.c_str());
    EXPECT_EQ(res.fillCount, 0);
    EXPECT_EQ(res.entityCount, 8);
    EXPECT_EQ(dxf.find("FILL"), std::string::npos);
    EXPECT_EQ(dxf.find("TABLES"), std::string::npos) << "no layer table when there is nothing to name";
}
