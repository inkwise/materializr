// Undoing past the step that drew a sketch empties it. The emptied sketch must
// not linger as a blank, openable entry (Finishing it deleted it, and the redo
// that should have refilled it then had nothing to land on - Sketch 1 lost for
// good). It becomes a hidden "ghost": gone from every listing, back on redo,
// and removed for real once a new step replaces the redo stack.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <cstdio>
#include <filesystem>
#include <memory>

using namespace materializr;

namespace {

class MockAddBodyOp : public Operation {
public:
    bool execute(Document& doc) override {
        m_id = doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "Box");
        return true;
    }
    bool undo(Document& doc) override { doc.removeBody(m_id); return true; }
    std::string name() const override { return "Add Box"; }
    std::string description() const override { return "Add box"; }
    void renderProperties() override {}
    std::string typeId() const override { return "mock_add_body"; }
private:
    int m_id = -1;
};

// A sketch in `doc` whose single recorded edit draws one circle.
struct Drawn {
    int sid = -1;
    std::shared_ptr<Sketch> live;
};

Drawn drawCircleStep(Document& doc, History& hist) {
    Drawn d;
    d.live = std::make_shared<Sketch>();
    d.sid = doc.addSketch(d.live, "Sketch");
    auto before = std::make_shared<Sketch>(*d.live);
    d.live->addCircle(d.live->addPoint({0, 0}), 5.0);
    auto after = std::make_shared<Sketch>(*d.live);
    auto op = std::make_unique<SketchEditOp>(d.live, before, after);
    op->setSketchId(d.sid);
    hist.pushExecuted(std::move(op), doc);
    return d;
}

} // namespace

TEST(SketchGhost, UndoHidesRedoRestores) {
    Document doc; History hist;
    Drawn d = drawCircleStep(doc, hist);
    ASSERT_EQ(doc.getAllSketchIds().size(), 1u);

    ASSERT_TRUE(hist.undo(doc));
    ASSERT_EQ(d.live->elementCount(), 0) << "setup: undo emptied the sketch";
    EXPECT_TRUE(syncGhostSketches(hist, doc));
    EXPECT_TRUE(doc.isSketchGhost(d.sid));
    EXPECT_TRUE(doc.getAllSketchIds().empty()) << "a ghost is not listed";
    EXPECT_EQ(doc.getAllSketchIdsIncludingGhosts().size(), 1u) << "but is still kept";
    EXPECT_NE(doc.getSketch(d.sid), nullptr);

    ASSERT_TRUE(hist.redo(doc));
    EXPECT_TRUE(syncGhostSketches(hist, doc));
    EXPECT_FALSE(doc.isSketchGhost(d.sid));
    EXPECT_EQ(doc.getAllSketchIds().size(), 1u);
    EXPECT_EQ(d.live->elementCount(), 1);
}

TEST(SketchGhost, NewStepBreaksTheRedoChainAndPurgesTheGhost) {
    Document doc; History hist;
    Drawn d = drawCircleStep(doc, hist);
    ASSERT_TRUE(hist.undo(doc));
    ASSERT_TRUE(syncGhostSketches(hist, doc));
    ASSERT_TRUE(doc.isSketchGhost(d.sid));

    // A fresh step replaces the redo stack: nothing can bring the sketch back.
    ASSERT_TRUE(hist.pushOperation(std::make_unique<MockAddBodyOp>(), doc));
    EXPECT_TRUE(syncGhostSketches(hist, doc));
    EXPECT_EQ(doc.getAllSketchIdsIncludingGhosts().size(), 0u);
    EXPECT_EQ(doc.getSketch(d.sid), nullptr);
}

TEST(SketchGhost, GhostSurvivesWhileRedoStillPending) {
    Document doc; History hist;
    Drawn d = drawCircleStep(doc, hist);
    ASSERT_TRUE(hist.undo(doc));
    ASSERT_TRUE(syncGhostSketches(hist, doc));
    // Re-running with nothing changed is a no-op, and the ghost stays put.
    EXPECT_FALSE(syncGhostSketches(hist, doc));
    EXPECT_TRUE(doc.isSketchGhost(d.sid));
}

TEST(SketchGhost, SketchBeingEditedIsNeverTouched) {
    Document doc; History hist;
    Drawn d = drawCircleStep(doc, hist);
    ASSERT_TRUE(hist.undo(doc));
    EXPECT_FALSE(syncGhostSketches(hist, doc, /*keepSketchId=*/d.sid));
    EXPECT_FALSE(doc.isSketchGhost(d.sid));
}

TEST(SketchGhost, PlainEmptySketchWithNoRedoIsLeftAlone) {
    // An empty sketch no history step refers to (e.g. just created through an
    // API before it is filled) is not a ghost and must not be purged.
    Document doc; History hist;
    const int sid = doc.addSketch(std::make_shared<Sketch>(), "Fresh");
    EXPECT_FALSE(syncGhostSketches(hist, doc));
    EXPECT_FALSE(doc.isSketchGhost(sid));
    EXPECT_EQ(doc.getAllSketchIds().size(), 1u);
}

TEST(SketchGhost, SaveLeavesGhostsOutOfTheFile) {
    // A save keeps only the applied steps, so the redo that would refill a
    // ghost is not in the file. Writing the empty ghost anyway would reload as
    // a blank, openable sketch no step refers to - the very thing ghosts hide.
    Document doc; History hist;
    Drawn kept = drawCircleStep(doc, hist);
    Drawn ghost = drawCircleStep(doc, hist);
    ASSERT_TRUE(hist.undo(doc));
    ASSERT_TRUE(syncGhostSketches(hist, doc));
    ASSERT_TRUE(doc.isSketchGhost(ghost.sid));
    ASSERT_FALSE(doc.isSketchGhost(kept.sid));

    const std::string path =
        (std::filesystem::temp_directory_path() / "mzr_ghost_save_test.mzr").string();
    ASSERT_TRUE(ProjectIO::save(path, doc, nullptr).success);

    Document loaded;
    ASSERT_TRUE(ProjectIO::load(path, loaded, nullptr).success);
    std::remove(path.c_str());
    EXPECT_EQ(loaded.getAllSketchIdsIncludingGhosts().size(), 1u)
        << "only the live sketch is written";
    EXPECT_NE(loaded.getSketch(kept.sid), nullptr);
    EXPECT_EQ(loaded.getSketch(ghost.sid), nullptr);
}
