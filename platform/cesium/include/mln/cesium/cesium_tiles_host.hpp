#pragma once

#include <mln/style/layers/custom_layer_host.hpp>

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
    explicit CesiumTilesHost(std::string tilesetUrl,
                             double maximumScreenSpaceError = 16.0,
                             double heightOffsetMeters = 0.0);
    ~CesiumTilesHost() override;

    bool rendersIn3D() const override { return true; }

    void initialize(const style::CustomLayerInitParameters&) override;
    void render(const style::CustomLayerRenderParameters&) override;
    void contextLost() override;
    void deinitialize() override;

    /// How much of what the last view needs has loaded, 0..100. Lets a still render keep
    /// drawing frames until the tiles it shows are in.
    float loadProgress();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cesium
} // namespace mln
