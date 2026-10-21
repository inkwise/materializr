// Diagnostic probe: load a real project, rebuild its history the way
// Application::rebuildHistoryFromProject does, then walk Undo all the way down
// and Redo all the way back up, then random-walk both. Reports every call that
// fails, every call that "succeeds" without changing the document, and any
// drift between the starting state and the state after a full undo/redo cycle.
// Ground truth for "undo gets stuck in an undo/redo loop".
//
// Usage: probe_undo_redo <project.materializr> [randomWalkSteps]

#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "io/ProjectIO.h"
#include "modeling/OperationFactory.h"
#include "modeling/ReplayOp.h"
#include "modeling/SketchEditOp.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace materializr;

// Cheap, order-independent fingerprint of the document's bodies.
static std::string sig(Document& doc) {
    std::string s;
    std::vector<int> ids = doc.getAllBodyIds();
    char buf[96];
    for (int id : ids) {
        double vol = 0, area = 0, bb[6] = {0, 0, 0, 0, 0, 0};
        try {
            GProp_GProps gv, ga;
            const TopoDS_Shape& sh = doc.getBody(id);
            if (!sh.IsNull()) {
                BRepGProp::VolumeProperties(sh, gv);  vol = gv.Mass();
                BRepGProp::SurfaceProperties(sh, ga); area = ga.Mass();
                Bnd_Box b;
                BRepBndLib::AddOptimal(sh, b);
                if (!b.IsVoid()) b.Get(bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
            }
        } catch (...) {}
        std::snprintf(buf, sizeof(buf), "%d:%.3f/%.3f@%.2f,%.2f,%.2f,%.2f,%.2f,%.2f%s;",
                      id, vol, area, bb[0], bb[1], bb[2], bb[3], bb[4], bb[5],
                      doc.isBodyVisible(id) ? "" : "h");
        s += buf;
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <project> [walk]\n", argv[0]); return 2; }
    const int walkSteps = argc > 2 ? std::atoi(argv[2]) : 200;

    Document doc;
    ProjectHistory loaded;
    auto res = ProjectIO::load(argv[1], doc, &loaded);
    if (!res.success) { std::fprintf(stderr, "load failed: %s\n", res.errorMessage.c_str()); return 1; }
    std::printf("loaded %d bodies, %zu history steps (saved by %s)\n",
                res.bodiesLoaded, loaded.steps.size(), res.savedByVersion.c_str());

    History hist;
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shp] : loaded.initialState) running[id] = shp;
    auto toVec = [](const std::map<int, TopoDS_Shape>& m) {
        ReplayOp::BodyState v; for (const auto& [id, s] : m) v.push_back({id, s}); return v;
    };
    int baked = 0;
    std::vector<bool> affects;
    for (size_t i = 0; i < loaded.steps.size(); ++i) {
        const auto& st = loaded.steps[i];
        ReplayOp::BodyState before = toVec(running);
        Operation::ReloadState rs;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) {
                rs.created.push_back(id); rs.createdAfter.push_back({id, shape});
            } else {
                rs.modifiedBefore.push_back({id, running[id]});
                rs.modifiedAfter.push_back({id, shape});
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end()) rs.deletedBefore.push_back({id, it->second});
        }
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);
        ReplayOp::BodyState after = toVec(running);
        affects.push_back(!st.changed.empty() || !st.deleted.empty());

        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty())
            op = ProjectIO::rehydrateSketchEditOp(st.params, doc);
        if (!op && !st.params.empty()) {
            auto cand = OperationFactory::create(st.typeId);
            if (cand && cand->deserializeParams(st.params) && cand->rehydrateFromReload(rs, doc))
                op = std::move(cand);
        }
        if (!op) {
            ++baked;
            auto r = std::make_unique<ReplayOp>(st.typeId, st.name, st.description,
                                                std::move(before), std::move(after));
            if (!st.params.empty()) r->setStoredParams(st.params);
            op = std::move(r);
        }
        op->setEnabled(st.enabled);
        hist.pushExecuted(std::move(op));
    }
    std::printf("history rebuilt: %d steps, %d baked (non-editable)\n", hist.stepCount(), baked);

    const std::string top = sig(doc);
    int problems = 0;
    auto name = [&](int i) { const Operation* o = hist.getStep(i); return o ? o->name() : std::string("?"); };

    std::printf("\n--- Phase A: undo to the bottom ---\n");
    int guard = hist.stepCount() * 3 + 10;
    while (hist.canUndo() && guard-- > 0) {
        const int at = hist.currentStep();
        const std::string before = sig(doc);
        const bool ok = hist.undo(doc);
        const std::string afterS = sig(doc);
        const bool changed = before != afterS;
        const bool moved = hist.currentStep() != at;
        const bool body = at >= 0 && at < (int)affects.size() && affects[at];
        if (!ok || !moved || (body && !changed)) {
            ++problems;
            std::printf("  undo @%d '%s': ok=%d moved=%d docChanged=%d enabled=%d  <-- PROBLEM\n",
                        at, name(at).c_str(), ok, moved, changed,
                        hist.getStep(at) ? hist.getStep(at)->isEnabled() : -1);
            if (!ok) break;   // stuck: the user cannot go further back
        }
    }
    std::printf("bottom reached at index %d\n", hist.currentStep());

    std::printf("\n--- Phase B: redo to the top ---\n");
    guard = hist.stepCount() * 3 + 10;
    while (hist.canRedo() && guard-- > 0) {
        const int at = hist.currentStep();
        const std::string before = sig(doc);
        const bool ok = hist.redo(doc);
        const std::string afterS = sig(doc);
        const bool changed = before != afterS;
        const bool moved = hist.currentStep() != at;
        const bool body = at + 1 >= 0 && at + 1 < (int)affects.size() && affects[at + 1];
        if (!ok || !moved || (body && !changed)) {
            ++problems;
            std::printf("  redo -> %d '%s': ok=%d moved=%d docChanged=%d enabled=%d  <-- PROBLEM\n",
                        at + 1, name(at + 1).c_str(), ok, moved, changed,
                        hist.getStep(at + 1) ? hist.getStep(at + 1)->isEnabled() : -1);
            if (!ok) break;
        }
    }
    std::printf("top reached at index %d of %d\n", hist.currentStep(), hist.stepCount() - 1);
    const bool same = sig(doc) == top;
    std::printf("state after full undo+redo %s the loaded state\n", same ? "MATCHES" : "DIFFERS FROM");
    if (!same) ++problems;

    std::printf("\n--- Phase C: random walk (%d moves) ---\n", walkSteps);
    std::srand(12345);
    for (int k = 0; k < walkSteps; ++k) {
        const bool wantUndo = (std::rand() % 2) == 0;
        if (wantUndo ? !hist.canUndo() : !hist.canRedo()) continue;
        const int at = hist.currentStep();
        const bool ok = wantUndo ? hist.undo(doc) : hist.redo(doc);
        if (!ok) {
            ++problems;
            std::printf("  walk %d: %s failed at index %d '%s'  <-- PROBLEM\n", k,
                        wantUndo ? "undo" : "redo", at, name(wantUndo ? at : at + 1).c_str());
        }
    }
    while (hist.canRedo() && hist.redo(doc)) {}
    std::printf("after walk + redo-all: index %d, state %s the loaded state\n",
                hist.currentStep(), sig(doc) == top ? "MATCHES" : "DIFFERS FROM");
    if (sig(doc) != top) ++problems;

    std::printf("\n%d problem(s)\n", problems);
    return problems ? 3 : 0;
}
