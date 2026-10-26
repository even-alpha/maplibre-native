#pragma once

#include <memory>
#include <array>
#include <cstdint>
#include <vector>

namespace mln {

class PaintParameters;

namespace style {

/// A terrain tile's draped fills and lines on a transparent background, for a 3D host to
/// paint onto its models: the tile's web mercator square (origin x0, y0 and side `size`, in
/// world units 0..1, y growing south) and the OpenGL texture holding it, premultiplied.
/// Sample it like the terrain does: at (u, 1 - v) for the tile-relative position (u, v).
struct ModelOverlay {
    double x0;
    double y0;
    double size;
    uint32_t texture;
    /// Changes whenever the texture's content was re-rendered (e.g. to rebuild mipmaps)
    uint64_t version;
};

/**
 * Parameters that define the current camera position for a
 * `CustomLayerHost::render()` function.
 */
struct CustomLayerRenderParameters {
    double width;
    double height;
    /// Device pixels per logical pixel; `width` and `height` are logical.
    double pixelRatio;
    double latitude;
    double longitude;
    double zoom;
    double bearing;
    double pitch;
    double fieldOfView;
    /// Metres above sea level of the point the camera orbits (the ground under the map
    /// centre when the centre is clamped to terrain, else 0).
    double centerAltitude;
    /// Whether 3D terrain is drawn this frame, and the factor its heights are scaled by.
    /// A 3D host draws metres above sea level when terrain is on; off terrain the map is a
    /// flat plane at sea level.
    bool terrainEnabled;
    double terrainExaggeration;
    /// Overlays for the area the host asked for in CustomLayerHost::modelOverlayBounds,
    /// rendered this frame; empty without terrain or on backends other than OpenGL.
    std::vector<ModelOverlay> modelOverlays;

    /// Standard projection matrix (nearZ = 1 tile unit).
    /// Use this for 2D/flat custom geometry.
    std::array<double, 16> projectionMatrix;

    /// A 4×4 matrix representing the map view’s current near clip projection state.
    std::array<double, 16> nearClippedProjectionMatrix;

    CustomLayerRenderParameters(const PaintParameters&);
};

} // namespace style
} // namespace mln
