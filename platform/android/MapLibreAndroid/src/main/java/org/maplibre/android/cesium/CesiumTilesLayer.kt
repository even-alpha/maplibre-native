package org.maplibre.android.cesium

import androidx.annotation.Keep
import org.maplibre.android.style.layers.CustomLayer

/**
 * A layer that streams an OGC 3D Tiles tileset (photogrammetry meshes, glTF content) through
 * cesium-native and draws it in the map's projection, on the 3D terrain and occluded by it.
 * Its requests go through the map's own HTTP stack, so the OkHttp client given to
 * `HttpRequestUtil.setOkHttpClient` serves them too.
 *
 * Tiles load only while frames are drawn: a map that renders on demand should call
 * `MapLibreMap.triggerRepaint()` while [isLoading] is true.
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
class CesiumTilesLayer private constructor(id: String, private val hostPointer: Long) : CustomLayer(id, hostPointer) {

    @JvmOverloads
    constructor(
        id: String,
        tilesetUrl: String,
        maximumScreenSpaceError: Double = 16.0,
        heightOffsetMeters: Double = 0.0,
    ) : this(id, nativeCreateHost(tilesetUrl, maximumScreenSpaceError, heightOffsetMeters))

    /** Whether tiles for the last drawn view are still on their way. False once the layer is gone. */
    val isLoading: Boolean
        get() = nativeIsLoading(hostPointer)

    private companion object {
        @JvmStatic
        private external fun nativeCreateHost(
            tilesetUrl: String,
            maximumScreenSpaceError: Double,
            heightOffsetMeters: Double,
        ): Long

        @JvmStatic
        private external fun nativeIsLoading(hostPointer: Long): Boolean
    }
}
