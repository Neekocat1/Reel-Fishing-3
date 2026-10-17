// Reel Fishing III mod (step 46a): fish region shapes (RF3Shape.h).

#include "RF3Shape.h"

#include <algorithm>
#include <cmath>

namespace rf3 {
namespace shape {

namespace {

// The nearest point to (x, z) on the segment a-b, and its squared distance.
float NearestOnSegment(const Point& a, const Point& b, float x, float z, Point* out) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float len2 = dx * dx + dz * dz;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / len2, 0.0f, 1.0f);
    }
    out->x = a.x + dx * t;
    out->z = a.z + dz * t;
    const float ex = x - out->x;
    const float ez = z - out->z;
    return ex * ex + ez * ez;
}

void NearestOnRing(const Ring& ring, float x, float z, float* best2, Point* best) {
    const size_t n = ring.size();
    for (size_t i = 0; i < n && n >= 2; i++) {
        Point p;
        const float d2 = NearestOnSegment(ring[i], ring[(i + 1) % n], x, z, &p);
        if (d2 < *best2) {
            *best2 = d2;
            *best = p;
        }
    }
}

float Cross(const Point& o, const Point& a, const Point& b) {
    return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
}

// Do segments p1-p2 and p3-p4 cross (touching counts)?
bool SegmentsCross(const Point& p1, const Point& p2, const Point& p3, const Point& p4) {
    const float d1 = Cross(p3, p4, p1);
    const float d2 = Cross(p3, p4, p2);
    const float d3 = Cross(p1, p2, p3);
    const float d4 = Cross(p1, p2, p4);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    auto onSeg = [](const Point& a, const Point& b, const Point& p) {
        return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.z, b.z) <= p.z &&
               p.z <= std::max(a.z, b.z);
    };
    return (d1 == 0 && onSeg(p3, p4, p1)) || (d2 == 0 && onSeg(p3, p4, p2)) || (d3 == 0 && onSeg(p1, p2, p3)) ||
           (d4 == 0 && onSeg(p1, p2, p4));
}

} // namespace

bool InRing(const Ring& ring, float x, float z) {
    const size_t n = ring.size();
    if (n < 3) {
        return false;
    }
    bool in = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = ring[i];
        const Point& b = ring[j];
        if (((a.z > z) != (b.z > z)) && (x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x)) {
            in = !in;
        }
    }
    return in;
}

bool Inside(const Ring& outline, const std::vector<Ring>& holes, float x, float z) {
    if (!InRing(outline, x, z)) {
        return false;
    }
    for (const Ring& h : holes) {
        if (InRing(h, x, z)) {
            return false;
        }
    }
    return true;
}

float NearestEdgePoint(const Ring& outline, const std::vector<Ring>& holes, float x, float z, Point* nearest) {
    float best2 = 1e30f;
    Point best = { x, z };
    NearestOnRing(outline, x, z, &best2, &best);
    for (const Ring& h : holes) {
        NearestOnRing(h, x, z, &best2, &best);
    }
    *nearest = best;
    return std::sqrt(best2);
}

float OutsideOutline(const Ring& outline, float x, float z) {
    if (outline.size() < 3 || InRing(outline, x, z)) {
        return 0.0f;
    }
    float best2 = 1e30f;
    Point best;
    NearestOnRing(outline, x, z, &best2, &best);
    return std::sqrt(best2);
}

bool ClampInside(const Ring& outline, const std::vector<Ring>& holes, float x, float z, float margin, float* outX,
                 float* outZ) {
    *outX = x;
    *outZ = z;
    if (outline.size() < 3 || Inside(outline, holes, x, z)) {
        return false;
    }
    Point q;
    const float d = NearestEdgePoint(outline, holes, x, z, &q);
    float ux = 0.0f;
    float uz = 0.0f;
    if (d > 1e-4f) {
        ux = (q.x - x) / d; // from the point toward the edge: further along it is inside
        uz = (q.z - z) / d;
    }
    for (float step : { margin, 1.0f }) {
        const float cx = q.x + ux * step;
        const float cz = q.z + uz * step;
        if (step > 0.0f && Inside(outline, holes, cx, cz)) {
            *outX = cx;
            *outZ = cz;
            return true;
        }
    }
    *outX = q.x;
    *outZ = q.z;
    return true;
}

float SignedArea(const Ring& ring) {
    float a = 0.0f;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; i++) {
        const Point& p = ring[i];
        const Point& q = ring[(i + 1) % n];
        a += p.x * q.z - q.x * p.z;
    }
    return a * 0.5f;
}

bool SelfCrossing(const Ring& ring) {
    const size_t n = ring.size();
    if (n < 4) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (j == i + 1 || (i == 0 && j == n - 1)) {
                continue; // neighbours share a point
            }
            if (SegmentsCross(ring[i], ring[(i + 1) % n], ring[j], ring[(j + 1) % n])) {
                return true;
            }
        }
    }
    return false;
}

bool Valid(const Ring& outline, const std::vector<Ring>& holes, std::string* why) {
    auto ringOk = [why](const Ring& r, const char* what) {
        if (r.size() < 3 || r.size() > static_cast<size_t>(kMaxRingPoints)) {
            *why = std::string(what) + ": needs 3 to 64 points";
            return false;
        }
        if (SelfCrossing(r)) { // before the area: a figure-eight's halves cancel out
            *why = std::string(what) + ": crosses itself";
            return false;
        }
        if (std::fabs(SignedArea(r)) < 1.0f) {
            *why = std::string(what) + ": has no area";
            return false;
        }
        return true;
    };
    if (!ringOk(outline, "outline")) {
        return false;
    }
    if (holes.size() > static_cast<size_t>(kMaxHoles)) {
        *why = "more than 8 holes";
        return false;
    }
    for (const Ring& h : holes) {
        if (!ringOk(h, "hole")) {
            return false;
        }
        for (const Point& p : h) {
            if (!InRing(outline, p.x, p.z)) {
                *why = "hole: a point outside the outline";
                return false;
            }
        }
    }
    why->clear();
    return true;
}

void Bounds(const Ring& ring, float* minX, float* maxX, float* minZ, float* maxZ) {
    if (ring.empty()) {
        *minX = *maxX = *minZ = *maxZ = 0.0f;
        return;
    }
    *minX = *maxX = ring[0].x;
    *minZ = *maxZ = ring[0].z;
    for (const Point& p : ring) {
        *minX = std::min(*minX, p.x);
        *maxX = std::max(*maxX, p.x);
        *minZ = std::min(*minZ, p.z);
        *maxZ = std::max(*maxZ, p.z);
    }
}

Ring BoxRing(float minX, float maxX, float minZ, float maxZ, float radians) {
    const float cx = (minX + maxX) * 0.5f;
    const float cz = (minZ + maxZ) * 0.5f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float bx[4] = { minX, maxX, maxX, minX };
    const float bz[4] = { minZ, minZ, maxZ, maxZ };
    Ring r;
    for (int i = 0; i < 4; i++) {
        const float dx = bx[i] - cx;
        const float dz = bz[i] - cz;
        r.push_back({ cx + dx * c - dz * s, cz + dx * s + dz * c });
    }
    return r;
}

} // namespace shape
} // namespace rf3
