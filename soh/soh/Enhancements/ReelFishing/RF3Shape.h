#pragma once

// Reel Fishing III mod (step 46a): the shape of a fish region, an outline of world points (x, z) with holes, seen from
// above. Pure maths (no game code), so the offline harness tests it. See docs/DESIGN.md step 46.

#include <string>
#include <vector>

namespace rf3 {
namespace shape {

struct Point {
    float x = 0.0f;
    float z = 0.0f;
};
using Ring = std::vector<Point>; // closed: the last point joins the first

constexpr int kMaxRingPoints = 64; // [assumed] limits (DESIGN step 46)
constexpr int kMaxHoles = 8;

// Is (x, z) inside the ring (crossing test; a ring of fewer than 3 points has no inside)?
bool InRing(const Ring& ring, float x, float z);
// In the outline and in no hole.
bool Inside(const Ring& outline, const std::vector<Ring>& holes, float x, float z);
// The nearest point on any edge of the outline and the holes; returns its distance (a huge value if there is no edge).
float NearestEdgePoint(const Ring& outline, const std::vector<Ring>& holes, float x, float z, Point* nearest);
// Distance outside the outline (0 inside it; holes don't count).
float OutsideOutline(const Ring& outline, float x, float z);
// A point inside the shape near (x, z): (x, z) itself if inside; else the nearest edge point, moved `margin` further
// in if that is still inside (else 1 unit, else the edge point). Returns true if the point moved.
bool ClampInside(const Ring& outline, const std::vector<Ring>& holes, float x, float z, float margin, float* outX,
                 float* outZ);
// Signed area (positive = counter-clockwise seen with x right and z up).
float SignedArea(const Ring& ring);
// Does any edge of the ring cross another, not counting neighbours?
bool SelfCrossing(const Ring& ring);
// Can this be a region's shape? 3..kMaxRingPoints points per ring, a non-zero area, no ring crossing itself, every hole's
// points inside the outline, at most kMaxHoles holes. `why` names the first problem.
bool Valid(const Ring& outline, const std::vector<Ring>& holes, std::string* why);
// The bounding box of a ring.
void Bounds(const Ring& ring, float* minX, float* maxX, float* minZ, float* maxZ);
// A box (minX..maxX, minZ..maxZ in its own frame, turned by `radians` about its centre) as a 4-point ring.
Ring BoxRing(float minX, float maxX, float minZ, float maxZ, float radians);

} // namespace shape
} // namespace rf3
