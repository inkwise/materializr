// Undoing a Sketch Edit runs the sketch cascade, which replays the history
// chain from the earliest op built from that sketch. That scan used to cover
// the whole history, including the redo stack above the undo tip; a hit above
// the tip made History::editStep roll FORWARD through everything just undone,
// so a single Ctrl+Z could jump the tip forward ("stuck in an undo/redo loop").
// The scan now sees applied steps only.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <memory>

using namespace materializr;

namespace {

int addRectSketch(Document& doc, double x0, double y0, double w, double h) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int p0 = sk->addPoint({(float)x0, (float)y0});
    int p1 = sk->addPoint({(float)(x0 + w), (float)y0});
    int p2 = sk->addPoint({(float)(x0 + w), (float)(y0 + h)});
    int p3 = sk->addPoint({(float)x0, (float)(y0 + h)});
    sk->addLine(p0, p1); sk->addLine(p1, p2); sk->addLine(p2, p3); sk->addLine(p3, p0);
    return doc.addSketch(sk);
}

bool pushExtrude(History& hist, Document& doc, int sketchId, double dist) {
    auto ext = std::make_unique<ExtrudeOp>();
    ext->setSketchSource(sketchId);
    ext->setDistance(dist);
    if (!ext->rebuildProfileFromSketch(doc)) return false;
    return hist.pushOperation(std::move(ext), doc);
}

} // namespace

TEST(CascadeAppliedOnly, StepAboveTheUndoTipIsNotAReplayTarget) {
    Document doc;
    History hist;
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));   // step 0
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));   // step 1 - the only user of A

    ASSERT_TRUE(hist.undo(doc));                    // tip -> 0, step 1 is redo
    ASSERT_EQ(hist.currentStep(), 0);

    int matched = -1;
    const int earliest = rederiveSketchDrivenOps(hist, doc, sidA, &matched);
    EXPECT_EQ(earliest, -1) << "the only op built from the sketch is un-applied";
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(hist.currentStep(), 0) << "scanning must not move the tip";
}

TEST(CascadeAppliedOnly, AppliedStepIsStillFoundAndReplayStaysAtTheTip) {
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));   // step 0
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));   // step 1
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 7));   // step 2 - A again, above the tip

    ASSERT_TRUE(hist.undo(doc));                    // tip -> 1
    ASSERT_EQ(hist.currentStep(), 1);

    int matched = -1;
    const int earliest = rederiveSketchDrivenOps(hist, doc, sidA, &matched);
    EXPECT_EQ(earliest, 0);
    EXPECT_EQ(matched, 1) << "step 2 is above the tip and must not be counted";

    ASSERT_TRUE(hist.editStep(earliest, doc, /*transactional=*/true));
    EXPECT_EQ(hist.currentStep(), 1) << "the replay must stop at the undo tip";
}

// The mechanism behind the bug, pinned: replaying "from" a step above the tip is
// a roll-forward that re-applies the undone steps. This is WHY the cascade must
// never hand editStep such an index.
TEST(CascadeAppliedOnly, ReplayFromAboveTheTipRollsTheTipForward) {
    Document doc;
    History hist;
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));
    ASSERT_TRUE(hist.undo(doc));
    ASSERT_EQ(hist.currentStep(), 0);
    ASSERT_TRUE(hist.editStep(1, doc, /*transactional=*/true));
    EXPECT_EQ(hist.currentStep(), 1) << "an undone step came back by itself";
}
