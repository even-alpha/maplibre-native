#pragma once

#include <memory>
#include <array>

namespace mln {

class PaintParameters;

namespace style {

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

    /// Standard projection matrix (nearZ = 1 tile unit).
    /// Use this for 2D/flat custom geometry.
    std::array<double, 16> projectionMatrix;

    /// A 4×4 matrix representing the map view’s current near clip projection state.
    std::array<double, 16> nearClippedProjectionMatrix;

    CustomLayerRenderParameters(const PaintParameters&);
};

} // namespace style
} // namespace mln
