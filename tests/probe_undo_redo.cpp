// Diagnostic probe: load a real project, rebuild its history the way
// Application::rebuildHistoryFromProject does, then walk Undo to the bottom and
// Redo back to the top the way Application::undoWithCascade / redoWithCascade
// do - History::undo/redo FOLLOWED BY the sketch cascade - and check the one
// invariant a user feels: Ctrl+Z moves the tip down by exactly one applied step
// and Ctrl+Y moves it up, never the other way. A cascade that replays from a
// step above the tip rolls it forward ("stuck in an undo/redo loop").
//
// Usage: probe_undo_redo <project.materializr> [--old-scan] [--max N]
//   --old-scan  use the pre-fix cascade scan (whole history) for an A/B check
//   --max N     stop each phase after N moves (default: all)
// Output is unbuffered and timed per move so a slow run still tells you where.

#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/OperationFactory.h"
#include "modeling/PushPullOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/SketchEditOp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace materializr;

namespace {

bool g_oldScan = false;
int g_cascades = 0;     // cascades that actually replayed something
int g_tipMoved = 0;     // ... and moved the tip while doing it

// Pre-fix scan: every step in the history, applied or not.
int scanAllSteps(History& h, Document& doc, int sketchId, int* matchedOut) {
    int earliest = -1, matched = 0;
    for (int i = 0; i < h.stepCount(); ++i) {
        Operation* op = const_cast<Operation*>(h.getStep(i));
        if (!op || !op->isEnabled()) continue;
        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (ext->rebuildProfileFromSketch(doc) && earliest < 0) earliest = i;
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            bool refs = false;
            for (int t = 0; t < pp->targetCount(); ++t)
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            if (!refs) continue;
            ++matched;
            if (pp->rebuildProfileFromSketch(doc, sketchId) && earliest < 0) earliest = i;
        }
    }
    if (matchedOut) *matchedOut = matched;
    return earliest;
}

// Headless mirror of Application::cascadeFromSketchEdit (no UI, no toasts).
void cascade(History& h, Document& doc, int sid) {
    if (sid < 0) return;
    if (auto sk = doc.getSketch(sid); sk && sk->isDetachedFromBody()) return;
    int matched = 0;
    const int earliest = g_oldScan ? scanAllSteps(h, doc, sid, &matched)
                                   : rederiveSketchDrivenOps(h, doc, sid, &matched);
    if (earliest < 0) return;
    ++g_cascades;
    const int tipBefore = h.currentStep();
    if (auto sk = doc.getSketch(sid))
        doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = h.editStep(earliest, doc, /*transactional=*/true);
    std::vector<int> disabled;
    while (!ok && h.lastEditFailStep() >= 0 && disabled.size() < 8) {
        const int bad = h.lastEditFailStep();
        h.setStepEnabled(bad, false, doc);
        disabled.push_back(bad);
        ok = h.editStep(earliest, doc, /*transactional=*/true);
    }
    if (!ok && !disabled.empty())
        for (int i : disabled) h.setStepEnabled(i, true, doc);
    doc.clearCascadeSketchOverrides();
    if (h.currentStep() != tipBefore) {
        ++g_tipMoved;
        std::printf("    cascade(sketch %d) replayed from step %d and MOVED THE TIP %d -> %d\n",
                    sid, earliest, tipBefore, h.currentStep());
    }
}

double ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) { std::fprintf(stderr, "usage: %s <project> [--old-scan] [--max N]\n", argv[0]); return 2; }
    int maxMoves = 1 << 30;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--old-scan")) g_oldScan = true;
        else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) maxMoves = std::atoi(argv[++i]);
    }

    Document doc;
    ProjectHistory loaded;
    auto res = ProjectIO::load(argv[1], doc, &loaded);
    if (!res.success) { std::fprintf(stderr, "load failed: %s\n", res.errorMessage.c_str()); return 1; }
    std::printf("loaded %d bodies, %zu history steps (saved by %s); scan=%s\n",
                res.bodiesLoaded, loaded.steps.size(), res.savedByVersion.c_str(),
                g_oldScan ? "OLD (whole history)" : "NEW (applied only)");

    History hist;
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shp] : loaded.initialState) running[id] = shp;
    auto toVec = [](const std::map<int, TopoDS_Shape>& m) {
        ReplayOp::BodyState v; for (const auto& [id, s] : m) v.push_back({id, s}); return v;
    };
    int baked = 0, real = 0;
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

        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty())
            op = ProjectIO::rehydrateSketchEditOp(st.params, doc);
        if (!op && !st.params.empty()) {
            auto cand = OperationFactory::create(st.typeId);
            if (cand && cand->deserializeParams(st.params) && cand->rehydrateFromReload(rs, doc))
                op = std::move(cand);
        }
        if (op) ++real; else {
            ++baked;
            auto r = std::make_unique<ReplayOp>(st.typeId, st.name, st.description,
                                                std::move(before), std::move(after));
            if (!st.params.empty()) r->setStoredParams(st.params);
            op = std::move(r);
        }
        op->setEnabled(st.enabled);
        hist.pushExecuted(std::move(op));
    }
    // What can the cascade even touch? Count ops it dispatches on / replays from.
    int sketchOps = 0, driven = 0;
    for (int i = 0; i < hist.stepCount(); ++i) {
        const Operation* o = hist.getStep(i);
        if (sketchIdForCascade(o, doc) >= 0) ++sketchOps;
        if (dynamic_cast<const ExtrudeOp*>(o) || dynamic_cast<const PushPullOp*>(o)) ++driven;
    }
    std::printf("history rebuilt: %d steps (%d editable, %d baked); %d sketch-edit steps the "
                "cascade fires on, %d editable extrude/push-pull steps it can replay from\n",
                hist.stepCount(), real, baked, sketchOps, driven);

    int problems = 0;
    auto nm = [&](int i) { const Operation* o = hist.getStep(i); return o ? o->name() : std::string("?"); };

    std::printf("\n--- Phase A: Ctrl+Z (undo + cascade) to the bottom ---\n");
    int moves = 0;
    while (hist.canUndo() && moves < maxMoves) {
        const int at = hist.currentStep();
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = hist.undo(doc);
        const int idx = hist.lastUndoneStep();
        if (ok) dispatchUndoRedoCascade(idx, hist.getStep(idx), doc, -1,
                                        [&](int sid) { cascade(hist, doc, sid); });
        const int now = hist.currentStep();
        ++moves;
        const double t = ms(t0);
        // Ctrl+Z must go DOWN. It may skip DISABLED steps (so by more than one),
        // but never up and never nowhere.
        if (!ok || now >= at) {
            ++problems;
            std::printf("  PROBLEM undo #%d @%d '%s': ok=%d tip %d -> %d (%.0f ms)\n",
                        moves, at, nm(at).c_str(), ok, at, now, t);
            if (!ok) break;
            if (now > at && moves > 4 * hist.stepCount()) break;   // runaway guard
        } else if (t > 2000.0) {
            std::printf("  slow undo @%d '%s': %.0f ms\n", at, nm(at).c_str(), t);
        }
    }
    std::printf("bottom: tip %d after %d undos\n", hist.currentStep(), moves);

    std::printf("\n--- Phase B: Ctrl+Y (redo + cascade) to the top ---\n");
    moves = 0;
    while (hist.canRedo() && moves < maxMoves) {
        const int at = hist.currentStep();
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = hist.redo(doc);
        const int idx = hist.lastRedoneStep();
        if (ok) dispatchUndoRedoCascade(idx, hist.getStep(idx), doc, -1,
                                        [&](int sid) { cascade(hist, doc, sid); });
        const int now = hist.currentStep();
        ++moves;
        if (!ok || now <= at) {
            ++problems;
            std::printf("  PROBLEM redo #%d from %d '%s': ok=%d tip %d -> %d (%.0f ms)\n",
                        moves, at, nm(at + 1).c_str(), ok, at, now, ms(t0));
            if (!ok) break;
        }
    }
    std::printf("top: tip %d of %d after %d redos\n", hist.currentStep(), hist.stepCount() - 1, moves);

    std::printf("\ncascades that replayed: %d, of which moved the tip: %d\n", g_cascades, g_tipMoved);
    problems += g_tipMoved;
    std::printf("%d problem(s)\n", problems);
    return problems ? 3 : 0;
}
