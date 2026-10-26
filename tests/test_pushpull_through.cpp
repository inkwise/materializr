// Regression: a push/pull cut deeper than the body consumed the ENTIRE solid -
// BRepAlgoAPI_Cut returns an empty compound, execute() stored it via
// updateBody, tessellation then failed every preview frame (stale mesh) and
// the body vanished on commit. A cut that would leave no solid must be
// REFUSED (document untouched); a partial cut must keep working.

#include "core/Document.h"
#include "modeling/PushPullOp.h"

#include <gtest/gtest.h>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pln.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cmath>

namespace {

// The planar face whose outward normal is ~+Z at the top of the shape.
TopoDS_Face topFace(const TopoDS_Shape& s, double zTop) {
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
        if (surf.IsNull() || !surf->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
        BRepGProp_Face g(f); double u1,u2,v1,v2; g.Bounds(u1,u2,v1,v2);
        gp_Pnt c; gp_Vec n; g.Normal(0.5*(u1+u2),0.5*(v1+v2),c,n);
        if (std::abs(n.Z()) > 0.9 && c.Z() > zTop - 0.1) return f;
    }
    return TopoDS_Face();
}

double solidVolume(const TopoDS_Shape& s) {
    GProp_GProps gp;
    BRepGProp::VolumeProperties(s, gp);
    return gp.Mass();
}

bool hasSolid(const TopoDS_Shape& s) {
    return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More();
}

} // namespace

TEST(PushPullThrough, CutConsumingWholeBodyIsRefused) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    int id = doc.addBody(box, "Box");
    const double vol0 = solidVolume(doc.getBody(id));
    ASSERT_GT(vol0, 1.0);

    TopoDS_Face top = topFace(doc.getBody(id), 10.0);
    ASSERT_FALSE(top.IsNull());

    PushPullOp op;
    std::vector<PushPullOp::Target> targets(1);
    targets[0].profile = top;
    targets[0].sourceBodyId = id;
    op.setTargets(std::move(targets));
    op.setDistance(-50.0); // cut 5x deeper than the body - would consume it all
    op.setCutIntersecting(true); // app sets this for every cut direction

    const bool applied = op.execute(doc);

    // The document must still hold the intact solid - never an empty shape.
    TopoDS_Shape after = doc.getBody(id);
    EXPECT_TRUE(hasSolid(after))
        << "cut-through stored a solid-less shape (the vanishing-body bug)";
    EXPECT_NEAR(solidVolume(after), vol0, 1e-6)
        << "refused cut must leave the body untouched";
    EXPECT_FALSE(applied) << "an all-consuming cut must report failure so "
                             "History::pushOperation rejects it";
}

TEST(PushPullThrough, PartialCutStillWorks) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    int id = doc.addBody(box, "Box");
    const double vol0 = solidVolume(doc.getBody(id));

    TopoDS_Face top = topFace(doc.getBody(id), 10.0);
    ASSERT_FALSE(top.IsNull());

    PushPullOp op;
    std::vector<PushPullOp::Target> targets(1);
    targets[0].profile = top;
    targets[0].sourceBodyId = id;
    op.setTargets(std::move(targets));
    op.setDistance(-5.0); // pocket half way - legitimate
    op.setCutIntersecting(true);

    ASSERT_TRUE(op.execute(doc));
    TopoDS_Shape after = doc.getBody(id);
    ASSERT_TRUE(hasSolid(after));
    EXPECT_LT(solidVolume(after), vol0 - 1.0) << "partial cut must remove material";
    EXPECT_GT(solidVolume(after), 1.0);
}

// A grille is dozens of slot regions cut in one gesture. They must land as ONE
// boolean per body: the old per-region loop re-booleaned an ever more complex
// body (50 s for 150 slots on a real lid) and silently left some slots uncut.
TEST(PushPullThrough, ManySlotCutIsOneCompleteBoolean) {
    Document doc;
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 100, 100, 10).Shape();
    int id = doc.addBody(plate, "Plate");
    const double vol0 = solidVolume(doc.getBody(id));

    const int kSlots = 40;                     // 40 slots, 2 x 40, 2 mm deep
    std::vector<PushPullOp::Target> targets;
    for (int i = 0; i < kSlots; ++i) {
        const double x = 2.0 + 2.4 * i;        // 0.4 mm webs between slots
        gp_Pln pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1));
        TopoDS_Face f = BRepBuilderAPI_MakeFace(
            pln, x, x + 2.0, 30.0, 70.0).Face();
        PushPullOp::Target t;
        t.profile = f;
        t.sourceBodyId = id;
        targets.push_back(t);
    }
    PushPullOp op;
    op.setTargets(std::move(targets));
    // Sketch-sourced, like a real grille: keeps the profiles from being
    // re-resolved to the plate's own top face.
    for (int i = 0; i < kSlots; ++i) op.setSketchSource(i, 1, i);
    op.setDistance(-2.0);
    op.setCutIntersecting(true);

    ASSERT_TRUE(op.execute(doc));
    TopoDS_Shape after = doc.getBody(id);
    ASSERT_TRUE(hasSolid(after));
    int solids = 0;
    for (TopExp_Explorer ex(after, TopAbs_SOLID); ex.More(); ex.Next()) ++solids;
    EXPECT_EQ(solids, 1);
    EXPECT_TRUE(BRepCheck_Analyzer(after).IsValid());
    EXPECT_NEAR(vol0 - solidVolume(after), kSlots * 2.0 * 40.0 * 2.0, 1e-3)
        << "every slot must be cut";

    ASSERT_TRUE(op.undo(doc));
    EXPECT_NEAR(solidVolume(doc.getBody(id)), vol0, 1e-6);
}
