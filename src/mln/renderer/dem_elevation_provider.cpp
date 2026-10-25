#include <mln/renderer/dem_elevation_provider.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mln {

namespace {
constexpr size_t maxRememberedTiles = 768;
constexpr uint64_t unusedFramesBeforeEviction = 600;
} // namespace

void DEMRangeCache::remember(const CanonicalTileID& id, const DEMData& dem, uint64_t frame) {
    if (auto it = entries.find(id); it != entries.end()) {
        it->second.lastUsed = frame;
        return;
    }
    const auto* finest = dem.getFinestMinMax();
    if (!finest || dem.dim <= 0) {
        return;
    }
    entries.emplace(id, Entry{dem.dim, *finest, frame});
    answered.clear();
    shallowestZoom = std::min(shallowestZoom, id.z);
    deepestZoom = std::max(deepestZoom, id.z);
}

std::optional<Range<int32_t>> DEMRangeCache::rangeUnder(const CanonicalTileID& id, uint64_t frame) const {
    if (entries.empty()) {
        return std::nullopt;
    }
    if (const auto known = answered.find(id); known != answered.end()) {
        if (known->second.source) {
            entries.find(*known->second.source)->second.lastUsed = frame;
        }
        return known->second.range;
    }
    const Answer answer = scanRangeUnder(id, frame);
    answered.emplace(id, answer);
    return answer.range;
}

DEMRangeCache::Answer DEMRangeCache::scanRangeUnder(const CanonicalTileID& id, uint64_t frame) const {
    // The deepest remembered tile covering `id`: its own, else each ancestor in turn.
    for (int zoom = std::min<int>(id.z, deepestZoom); zoom >= shallowestZoom; --zoom) {
        const CanonicalTileID candidate = id.scaledTo(static_cast<uint8_t>(zoom));
        const auto it = entries.find(candidate);
        if (it == entries.end()) {
            continue;
        }
        const Entry& entry = it->second;
        entry.lastUsed = frame;

        // The DEM pixels under `id`, grown by one because the mesh interpolates between samples.
        const double scale = std::exp2(id.z - candidate.z);
        const double pixelsPerTile = entry.dim / scale;
        const double left = (id.x - candidate.x * scale) * pixelsPerTile;
        const double top = (id.y - candidate.y * scale) * pixelsPerTile;
        const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(left)) - 1, 0, entry.dim - 1);
        const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(top)) - 1, 0, entry.dim - 1);
        const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(left + pixelsPerTile)), x0, entry.dim - 1);
        const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(top + pixelsPerTile)), y0, entry.dim - 1);

        const auto& cells = entry.cells;
        int32_t lo = std::numeric_limits<int32_t>::max();
        int32_t hi = std::numeric_limits<int32_t>::lowest();
        for (int32_t cy = y0 / cells.cellSize; cy <= y1 / cells.cellSize; ++cy) {
            for (int32_t cx = x0 / cells.cellSize; cx <= x1 / cells.cellSize; ++cx) {
                const size_t cell = static_cast<size_t>(cy) * cells.cells + cx;
                lo = std::min(lo, cells.min[cell]);
                hi = std::max(hi, cells.max[cell]);
            }
        }
        return {Range<int32_t>{lo, hi}, candidate};
    }
    return {};
}

void DEMRangeCache::prune(uint64_t frame) {
    if (entries.size() <= maxRememberedTiles) {
        return;
    }
    for (auto it = entries.begin(); it != entries.end();) {
        if (frame - it->second.lastUsed > unusedFramesBeforeEviction) {
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    answered.clear();
    shallowestZoom = 255;
    deepestZoom = 0;
    for (const auto& [id, entry] : entries) {
        shallowestZoom = std::min(shallowestZoom, id.z);
        deepestZoom = std::max(deepestZoom, id.z);
    }
}

DEMElevationProvider::DEMElevationProvider(const DEMRangeCache* ranges_, double exaggeration_, uint64_t frame_)
    : ranges(ranges_),
      exaggeration(exaggeration_),
      frame(frame_) {}

std::optional<Range<double>> DEMElevationProvider::getTileElevationRange(const CanonicalTileID& id) const {
    if (!ranges || ranges->empty()) {
        return std::nullopt;
    }
    std::optional<Range<double>> result;
    if (const auto range = ranges->rangeUnder(id, frame)) {
        // Exaggeration is applied to the mesh in the terrain vertex shader, so the bounds
        // have to carry it too, or an exaggerated peak would still be culled.
        result = Range<double>{range->min * exaggeration, range->max * exaggeration};
    }
    // else: no DEM seen here yet, flat as before
    return result;
}

} // namespace mln
