#pragma once
// A cylinder, cone, sphere, torus or full lathe is one round face stitched shut
// along a SEAM: an edge that appears twice in that face, once on each side.
// It is real topology (the surface is periodic and has to start somewhere) but
// it is not a feature of the part - there is no crease there - so drawing it,
// or letting a click land on it, shows a line that is not in the model.
//
// Include from .cpp files only (OCCT headers; see the Windows build notes on
// widely-included headers).

#include <BRep_Tool.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace materializr {

// edge -> the faces that contain it. Build once per shape and reuse it for
// every edge of that shape.
inline TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap(const TopoDS_Shape& shape) {
    TopTools_IndexedDataMapOfShapeListOfShape map;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, map);
    return map;
}

// True when `edge` is closed on one of its faces (the same edge bounds that
// face on both sides). A circular rim edge is shared by two DIFFERENT faces, so
// it is never a seam.
inline bool isSeamEdge(const TopoDS_Edge& edge,
                       const TopTools_IndexedDataMapOfShapeListOfShape& edgeFaces) {
    if (!edgeFaces.Contains(edge)) return false;
    for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromKey(edge)); it.More(); it.Next())
        if (BRep_Tool::IsClosed(edge, TopoDS::Face(it.Value()))) return true;
    return false;
}

} // namespace materializr
