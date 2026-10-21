// A chamfer whose setback lands EXACTLY on a neighbouring edge (1 mm on the
// 1 mm shoulder between a 22 and a 20 mm diameter) used to fail with "invalid
// geometry": the shoulder is consumed whole and a zero-width face is left.
// ChamferOp now retries a hair short of the limit, so it builds and leaves a
// sliver. A value that is genuinely too big must still be refused.

#include "modeling/ChamferOp.h"
#include "core/Document.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <cmath>

using namespace materializr;

namespace {

// 22 mm diameter x 10 high, a 20 mm diameter x 10 high stacked on top.
TopoDS_Shape steppedShaft() {
    TopoDS_Shape lower = BRepPrimAPI_MakeCylinder(11.0, 10.0).Shape();
    TopoDS_Shape upper =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), 10.0, 10.0).Shape();
    return BRepAlgoAPI_Fuse(lower, upper).Shape();
}

// The internal (concave) circle where the 20 mm wall meets the shoulder.
TopoDS_Edge innerCorner(const TopoDS_Shape& body) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Circle) continue;
        if (std::abs(c.Circle().Radius() - 10.0) > 1e-6) continue;
        if (std::abs(c.Circle().Location().Z() - 10.0) > 1e-6) continue;
        return TopoDS::Edge(ex.Current());
    }
    return {};
}

bool chamferAt(double d, TopoDS_Shape* out = nullptr) {
    Document doc;
    int id = doc.addBody(steppedShaft(), "shaft");
    TopoDS_Edge e = innerCorner(doc.getBody(id));
    EXPECT_FALSE(e.IsNull());
    ChamferOp op;
    op.setBody(id);
    op.setEdges({e});
    op.setDistance(d);
    const bool ok = op.execute(doc);
    if (ok && out) *out = doc.getBody(id);
    return ok;
}

} // namespace

TEST(ChamferAtLimit, WellInsideTheShoulderBuilds) {
    EXPECT_TRUE(chamferAt(0.5));
}

TEST(ChamferAtLimit, ExactlyTheShoulderWidthBuildsAsAValidSolid) {
    TopoDS_Shape r;
    ASSERT_TRUE(chamferAt(1.0, &r)) << "setback == shoulder width must build";
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
    GProp_GProps g;
    BRepGProp::VolumeProperties(r, g);
    EXPECT_GT(g.Mass(), 1000.0);
}

TEST(ChamferAtLimit, WayPastTheShoulderIsStillRefused) {
    EXPECT_FALSE(chamferAt(3.0));
}
