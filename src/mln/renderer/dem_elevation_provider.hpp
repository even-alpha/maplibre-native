#pragma once

#include <mln/tile/tile_id.hpp>
#include <mln/util/range.hpp>
#include <mln/util/tile_cover.hpp>

#include <mln/geometry/dem_data.hpp>

#include <map>
#include <optional>
#include <unordered_map>

namespace mln {

class RenderSource;

/// Answers "how high is the terrain here?" for util::tileCover, from the DEM source's
/// currently loaded tiles, so that sources are asked for the tiles the terrain mesh
/// actually needs (see util::TileElevationProvider).
///
/// A tile's own DEM is used when loaded, otherwise the nearest loaded ancestor's range,
/// which contains it. The answer only has to be conservative: too large an elevation
/// range loads a tile that turns out not to be visible, too small drops one that is.
///
/// For a tile with *no* loaded DEM covering it, it reports the aggregate range of the
/// terrain currently loaded in view rather than "unknown". This is what lets the
/// one-ring cover dilation in RenderTerrain::computeMeshCover re-test a not-yet-loaded
/// frontier neighbour against the elevation-extended frustum: an unknown tile is assumed
/// about as tall (or, for bathymetry, as deep) as its loaded neighbours, so the tile that
/// belongs in view is kept and requested instead of assumed flat and dropped. nullopt
/// only when nothing at all is loaded (cover stays flat, exactly as before).
/// Elevation ranges of every DEM tile the terrain has rendered, kept across frames.
///
/// The cover tests a tile's box against the frustum with the height of the DEM under it.
/// Reading that height from the DEM tiles rendered *this* frame fed the cover its own
/// output: once a child tile loaded, its parent left the rendered set, every sibling still
/// loading lost its parent's range, fell back to a looser one, and moved in or out of the
/// cover - which changed which tiles were rendered, every frame, with every source's cover
/// following. Ranges kept here only ever get finer as tiles load, so the cover converges.
class DEMRangeCache {
public:
    void remember(const CanonicalTileID&, const DEMData&, uint64_t frame);
    /// Min/max (metres, no exaggeration) over the DEM pixels under `id`, grown by a pixel
    /// each way, from the deepest remembered tile covering it; nullopt when none does.
    std::optional<Range<int32_t>> rangeUnder(const CanonicalTileID& id, uint64_t frame) const;
    void prune(uint64_t frame);
    bool empty() const { return entries.empty(); }

private:
    struct Answer {
        std::optional<Range<int32_t>> range;
        std::optional<CanonicalTileID> source;
    };
    Answer scanRangeUnder(const CanonicalTileID& id, uint64_t frame) const;

    struct Entry {
        int32_t dim;
        DEMData::MinMaxLevel cells;
        mutable uint64_t lastUsed;
    };
    std::map<CanonicalTileID, Entry> entries;
    // Answers stay valid until an entry is added or evicted, so a still map (redrawn for the
    // location puck or the compass) does not rescan the min/max cells every frame.
    mutable std::unordered_map<CanonicalTileID, Answer> answered;
    // The zooms with entries, so a lookup walks only those instead of every ancestor.
    uint8_t shallowestZoom = 255;
    uint8_t deepestZoom = 0;
};

class DEMElevationProvider final : public util::TileElevationProvider {
public:
    /// `ranges` may be null (no terrain), in which case every query reports unknown and the
    /// cover stays flat.
    explicit DEMElevationProvider(const DEMRangeCache* ranges, double exaggeration, uint64_t frame);

    std::optional<Range<double>> getTileElevationRange(const CanonicalTileID&) const override;

private:
    const DEMRangeCache* ranges;
    double exaggeration;
    uint64_t frame;
};

} // namespace mln
