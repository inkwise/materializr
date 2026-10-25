// A Lathe is built from a sketch, so editing that sketch must rebuild the body
// (as it already does for Extrude and Push/Pull). Before, the cascade only
// looked at Extrude and Push/Pull steps: "[Cascade] sketchId=1: 0 matched" and
// the revolved solid stayed stale while the sketch moved on.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/RevolveOp.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <cmath>
#include <memory>

using namespace materializr;

namespace {

constexpr double kPi = 3.14159265358979323846;

double vol(const TopoDS_Shape& s) {
    if (s.IsNull()) return -1.0;
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

struct RectPts { int p1, p2, p3, p4; };

// Axis-aligned rectangle [x0,x1] x [y0,y1] on the XY plane, point ids kept so a
// test can drag its corners.
RectPts addRect(Sketch& sk, double x0, double y0, double x1, double y1) {
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    RectPts r;
    r.p1 = sk.addPoint({static_cast<float>(x0), static_cast<float>(y0)});
    r.p2 = sk.addPoint({static_cast<float>(x1), static_cast<float>(y0)});
    r.p3 = sk.addPoint({static_cast<float>(x1), static_cast<float>(y1)});
    r.p4 = sk.addPoint({static_cast<float>(x0), static_cast<float>(y1)});
    sk.addLine(r.p1, r.p2);
    sk.addLine(r.p2, r.p3);
    sk.addLine(r.p3, r.p4);
    sk.addLine(r.p4, r.p1);
    return r;
}

// A Lathe of `sid`'s outermost region about the world Y axis.
std::unique_ptr<RevolveOp> lathe(Document& doc, int sid, RevolveMode mode, int target = -1) {
    auto regions = doc.getSketch(sid)->buildRegions();
    EXPECT_FALSE(regions.empty());
    auto op = std::make_unique<RevolveOp>();
    op->setProfile(regions.front().face);
    op->setSketchSource(sid);
    op->setAxis(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)));
    op->setAngle(360.0);
    op->setMode(mode);
    if (target >= 0) op->setTargetBody(target);
    return op;
}

// What the app's sketch-edit cascade does: re-derive, then replay from the
// earliest affected step.
bool cascade(History& hist, Document& doc, int sid, int* matched = nullptr) {
    const int earliest = rederiveSketchDrivenOps(hist, doc, sid, matched);
    if (earliest < 0) return false;
    if (auto sk = doc.getSketch(sid))
        doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    const bool ok = hist.editStep(earliest, doc, /*transactional=*/true);
    doc.clearCascadeSketchOverrides();
    return ok;
}

} // namespace

TEST(RevolveCascade, EditingTheSketchRebuildsTheLathe) {
    Document doc; History hist;
    auto sk = std::make_shared<Sketch>();
    RectPts r = addRect(*sk, 5, 0, 15, 20);
    const int sid = doc.addSketch(sk, "Profile");
    ASSERT_TRUE(hist.pushOperation(lathe(doc, sid, RevolveMode::NewBody), doc));
    const int bid = doc.getAllBodyIds().front();
    ASSERT_NEAR(vol(doc.getBody(bid)), kPi * (15 * 15 - 5 * 5) * 20, 1.0);

    // Widen the ring: outer radius 15 -> 25.
    sk->movePoint(r.p2, {25.0f, 0.0f});
    sk->movePoint(r.p3, {25.0f, 20.0f});
    int matched = 0;
    ASSERT_TRUE(cascade(hist, doc, sid, &matched));
    EXPECT_EQ(matched, 1);

    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    EXPECT_EQ(doc.getAllBodyIds().front(), bid) << "the body morphs in place, same id";
    EXPECT_NEAR(vol(doc.getBody(bid)), kPi * (25 * 25 - 5 * 5) * 20, 1.0);
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(bid)).IsValid());
}

TEST(RevolveCascade, DownstreamLatheCutStillAppliesToTheRebuiltBody) {
    Document doc; History hist;
    auto base = std::make_shared<Sketch>();
    RectPts r = addRect(*base, 5, 0, 15, 20);
    const int baseId = doc.addSketch(base, "Base");
    ASSERT_TRUE(hist.pushOperation(lathe(doc, baseId, RevolveMode::NewBody), doc));
    const int bid = doc.getAllBodyIds().front();

    // A groove cut r6..r8 by a second Lathe in Subtract mode.
    auto cutter = std::make_shared<Sketch>();
    addRect(*cutter, 6, -1, 8, 21);
    const int cutId = doc.addSketch(cutter, "Groove");
    ASSERT_TRUE(hist.pushOperation(lathe(doc, cutId, RevolveMode::Subtract, bid), doc));
    ASSERT_NEAR(vol(doc.getBody(bid)),
                kPi * ((15 * 15 - 5 * 5) - (8 * 8 - 6 * 6)) * 20, 1.0);

    base->movePoint(r.p2, {25.0f, 0.0f});
    base->movePoint(r.p3, {25.0f, 20.0f});
    ASSERT_TRUE(cascade(hist, doc, baseId));
    EXPECT_NEAR(vol(doc.getBody(bid)),
                kPi * ((25 * 25 - 5 * 5) - (8 * 8 - 6 * 6)) * 20, 1.0)
        << "the groove must survive the rebuild";
}

TEST(RevolveCascade, UnrelatedSketchMatchesNothing) {
    Document doc; History hist;
    auto sk = std::make_shared<Sketch>();
    addRect(*sk, 5, 0, 15, 20);
    const int sid = doc.addSketch(sk, "Profile");
    ASSERT_TRUE(hist.pushOperation(lathe(doc, sid, RevolveMode::NewBody), doc));
    const int other = doc.addSketch(std::make_shared<Sketch>(), "Other");

    int matched = -1;
    EXPECT_EQ(rederiveSketchDrivenOps(hist, doc, other, &matched), -1);
    EXPECT_EQ(matched, 0);
}

TEST(RevolveCascade, ProfileThatNoLongerClosesLeavesTheBodyAlone) {
    // Delete a side of the profile: no closed region is left to revolve, so the
    // cascade reports "matched but couldn't rebuild" (the app shows a toast)
    // and the existing body is untouched.
    Document doc; History hist;
    auto sk = std::make_shared<Sketch>();
    addRect(*sk, 5, 0, 15, 20);
    const int sid = doc.addSketch(sk, "Profile");
    ASSERT_TRUE(hist.pushOperation(lathe(doc, sid, RevolveMode::NewBody), doc));
    const int bid = doc.getAllBodyIds().front();
    const double before = vol(doc.getBody(bid));

    sk->removeElement(sk->getLines().front().id);
    int matched = 0;
    EXPECT_FALSE(cascade(hist, doc, sid, &matched));
    EXPECT_EQ(matched, 1);
    EXPECT_NEAR(vol(doc.getBody(bid)), before, 1e-6);
}
