package org.maplibre.android.cesium

import androidx.annotation.Keep
import org.maplibre.android.style.layers.CustomLayer

/**
 * A layer that streams an OGC 3D Tiles tileset (photogrammetry meshes, glTF content) through
 * cesium-native and draws it in the map's projection, on the 3D terrain and occluded by it.
 *
 * Only in builds made with `MLN_WITH_CESIUM_TILES=ON`; elsewhere the native factory is missing
 * and the constructor throws [UnsatisfiedLinkError].
 *
 * @param id the layer id
 * @param tilesetUrl the tileset.json URL
 * @param maximumScreenSpaceError the level-of-detail target in pixels; lower is finer and heavier
 * @param heightOffsetMeters added to every vertex's height, for data whose vertical datum is off
 */
@Keep
class CesiumTilesLayer @JvmOverloads constructor(
    id: String,
    tilesetUrl: String,
    maximumScreenSpaceError: Double = 16.0,
    heightOffsetMeters: Double = 0.0,
) : CustomLayer(id, nativeCreateHost(tilesetUrl, maximumScreenSpaceError, heightOffsetMeters)) {

    private companion object {
        @JvmStatic
        private external fun nativeCreateHost(
            tilesetUrl: String,
            maximumScreenSpaceError: Double,
            heightOffsetMeters: Double,
        ): Long
    }
}
