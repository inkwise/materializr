// The seam of a round face is real topology but not a feature of the part, so
// the edge renderer and the body-outline highlight skip it (modeling/SeamEdges.h). The rim
// circles - shared by two DIFFERENT faces - and every box edge must stay.

#include "modeling/SeamEdges.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>

using namespace materializr;

namespace {

struct Counts { int total = 0, seams = 0, circles = 0, circlesKept = 0; };

Counts count(const TopoDS_Shape& s) {
    Counts c;
    const auto ef = edgeFaceMap(s);
    // Each edge once: an explorer visits it once per face that contains it.
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(s, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge e = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(e)) continue;
        ++c.total;
        const bool seam = isSeamEdge(e, ef);
        if (seam) ++c.seams;
        if (BRepAdaptor_Curve(e).GetType() == GeomAbs_Circle) {
            ++c.circles;
            if (!seam) ++c.circlesKept;
        }
    }
    return c;
}

} // namespace

TEST(SeamEdges, BoxHasNone) {
    const Counts c = count(BRepPrimAPI_MakeBox(10, 20, 30).Shape());
    EXPECT_EQ(c.total, 12);
    EXPECT_EQ(c.seams, 0);
}

TEST(SeamEdges, CylinderHasOneAndKeepsBothRims) {
    const Counts c = count(BRepPrimAPI_MakeCylinder(5.0, 12.0).Shape());
    EXPECT_EQ(c.seams, 1);
    EXPECT_EQ(c.circles, 2);
    EXPECT_EQ(c.circlesKept, 2) << "a rim is shared by two faces: never a seam";
}

TEST(SeamEdges, ConeAndSphereHaveOne) {
    EXPECT_EQ(count(BRepPrimAPI_MakeCone(5.0, 2.0, 10.0).Shape()).seams, 1);
    EXPECT_EQ(count(BRepPrimAPI_MakeSphere(5.0).Shape()).seams, 1);
}

TEST(SeamEdges, FullLatheHasASeamAndAPartialOneDoesNot) {
    // A rectangle ring revolved about Y.
    BRepBuilderAPI_MakeWire w;
    const gp_Pnt p1(5, 0, 0), p2(15, 0, 0), p3(15, 20, 0), p4(5, 20, 0);
    w.Add(BRepBuilderAPI_MakeEdge(p1, p2));
    w.Add(BRepBuilderAPI_MakeEdge(p2, p3));
    w.Add(BRepBuilderAPI_MakeEdge(p3, p4));
    w.Add(BRepBuilderAPI_MakeEdge(p4, p1));
    const TopoDS_Face profile = BRepBuilderAPI_MakeFace(w.Wire()).Face();
    const gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0));

    const Counts full = count(BRepPrimAPI_MakeRevol(profile, axis).Shape());
    EXPECT_GE(full.seams, 1) << "a 360 degree lathe is stitched shut along a seam";

    const Counts part = count(BRepPrimAPI_MakeRevol(profile, axis, 3.14159265 * 1.5).Shape());
    EXPECT_EQ(part.seams, 0) << "a 270 degree lathe has real end faces, no seam";
}
