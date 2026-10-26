#pragma once

#include <mln/style/layers/custom_layer_host.hpp>

#include <functional>
#include <memory>
#include <string>

namespace mln {
namespace cesium {

/// A custom layer that streams an OGC 3D Tiles tileset (photogrammetry meshes, glTF
/// content) through cesium-native and draws it with OpenGL (ES 3 on Android, 3.3 core on
/// desktop) in the map's own projection: tile geometry is converted from ECEF to web
/// mercator with metres of height, so it sits on the 3D terrain mesh.
///
/// cesium-native does the tileset traversal, level-of-detail selection, request
/// scheduling and glTF decoding; this host only turns each loaded tile into vertex
/// buffers and draws the tiles selected for the frame.
class CesiumTilesHost final : public style::CustomLayerHost {
public:
    /// `heightOffsetMeters` is added to every vertex's height, for data whose vertical datum
    /// is off; the camera cesium-native culls with moves the other way, so the two agree.
    ///
    /// `minimumZoom` and `minimumPitchDegrees` gate the layer on the camera: it fades in over
    /// the zoom level below `minimumZoom` and the 10 degrees of pitch below
    /// `minimumPitchDegrees`, and below either it is idle - no tile selection, no requests, no
    /// drawing. Zero turns a gate off.
    explicit CesiumTilesHost(std::string tilesetUrl,
                             double maximumScreenSpaceError = 16.0,
                             double heightOffsetMeters = 0.0,
                             double minimumZoom = 0.0,
                             double minimumPitchDegrees = 0.0);
    ~CesiumTilesHost() override;

    bool rendersIn3D() const override { return true; }

    void initialize(const style::CustomLayerInitParameters&) override;
    void render(const style::CustomLayerRenderParameters&) override;
    void contextLost() override;
    void deinitialize() override;

    /// How much of what the last view needs has loaded, 0..100. Lets a still render keep
    /// drawing frames until the tiles it shows are in. Safe from any thread.
    float loadProgress() const;

    /// Whether tiles for the last view are still on their way; false while the layer is
    /// hidden. Loading advances only while frames are drawn, so a map that renders on demand
    /// keeps repainting while this is true. Safe from any thread.
    bool isLoading() const;

    /// Turns the layer off without removing it, e.g. while its server is unreachable: off,
    /// it neither draws nor requests tiles. Safe from any thread.
    void setEnabled(bool enabled);

    /// Gives memory back on the next frame: evicts every cached tile the current view does
    /// not show, or all of them while the layer is hidden. Safe from any thread.
    void trimMemory();

    /// Runs `action` on `host` if it still exists, holding it alive meanwhile: the style owns
    /// hosts and may delete one at any time. Returns whether it ran.
    static bool withLiveHost(const CesiumTilesHost* host, const std::function<void(CesiumTilesHost&)>& action);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cesium
} // namespace mln
