#pragma once
#include "Operation.h"
#include "Document.h"
#include "History.h"
#include "../modeling/ExtrudeOp.h"
#include "../modeling/PushPullOp.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/SketchTransformOp.h"
#include <functional>

namespace materializr {

// The sketch id `op`'s undo()/redo() drove via the sketch-edit cascade (a
// SketchEditOp/SketchTransformOp's own undo/redo only reverts sketch geometry;
// the body it drives is updated separately via SketchEditedEvent). -1 if `op`
// is null or isn't a sketch-mutating step.
inline int sketchIdForCascade(const Operation* op, const Document& doc) {
    if (!op) return -1;
    if (auto* se = dynamic_cast<const SketchEditOp*>(op)) {
        auto target = se->getTarget();
        return target ? doc.findSketchId(target.get()) : -1;
    }
    if (auto* st = dynamic_cast<const SketchTransformOp*>(op))
        return st->getSketchId();
    return -1;
}

// Re-derive the profile of every enabled sketch-sourced extrude / push-pull that
// references `sketchId` and return the index of the EARLIEST one that rebuilt
// (-1 if none). `*matchedOut` counts the ones that reference the sketch at all.
//
// Only steps that are currently APPLIED (index <= currentStep) are considered.
// The caller replays the chain from the returned index; a step above the tip is
// part of the redo stack, and handing one back made History::editStep take its
// "roll FORWARD" branch (index > tip) and re-execute everything in between -
// so undoing a Sketch Edit whose sketch drives a LATER op jumped the tip
// forward instead of back (the "stuck in an undo/redo loop" report). A step
// above the tip is rebuilt anyway when redo re-applies the sketch edit that
// feeds it, which runs this again.
inline int rederiveSketchDrivenOps(History& history, Document& doc, int sketchId,
                                   int* matchedOut = nullptr) {
    int earliest = -1, matched = 0;
    const int applied = history.currentStep();
    for (int i = 0; i <= applied && i < history.stepCount(); ++i) {
        Operation* op = const_cast<Operation*>(history.getStep(i));
        if (!op || !op->isEnabled()) continue;
        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (ext->rebuildProfileFromSketch(doc) && earliest < 0) earliest = i;
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            bool refs = false;
            const int tc = pp->targetCount();
            for (int t = 0; t < tc; ++t)
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            if (!refs) continue;
            ++matched;
            if (pp->rebuildProfileFromSketch(doc, sketchId) && earliest < 0)
                earliest = i;
        }
    }
    if (matchedOut) *matchedOut = matched;
    return earliest;
}

// Fires `cascade(sketchId)` for every sketch that needs its driven body
// resynced after an undo()/redo() call, given:
// - `appliedIdx`: History::lastUndoneStep()/lastRedoneStep() from that call
//   (< 0 means nothing was actually applied - fires nothing, unconditionally)
// - `appliedOp`: history.getStep(appliedIdx) (null when appliedIdx < 0)
// - `doc`: for sketch-id resolution
// - `activeSketchId`: the in-progress sketch session's id, or -1 when not in
//   sketch mode / no active sketch (keeps an in-progress sketch's driven body
//   in sync even for an upstream op that isn't itself a sketch op)
// Fires each distinct sketch id at most once (the active sketch and the
// applied op's sketch are often the same id).
template <typename Cascade>
void dispatchUndoRedoCascade(int appliedIdx, const Operation* appliedOp,
                             const Document& doc, int activeSketchId,
                             Cascade&& cascade) {
    if (appliedIdx < 0) return; // nothing was actually applied - never cascade
    // A Fill-tool step moved no geometry: neither its sketch nor the active
    // one has anything new to push into a body.
    if (auto* se = dynamic_cast<const SketchEditOp*>(appliedOp); se && se->onlyFillsChanged())
        return;
    int cascaded = -1;
    if (activeSketchId >= 0) {
        cascade(activeSketchId);
        cascaded = activeSketchId;
    }
    if (int sid = sketchIdForCascade(appliedOp, doc); sid >= 0 && sid != cascaded)
        cascade(sid);
}

} // namespace materializr
