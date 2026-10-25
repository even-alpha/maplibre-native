# The Cesium 3D Tiles custom layer: cesium-native does the tileset work, this library
# draws it with OpenGL ES 3 inside MapLibre's own projection. Enabled with
# MLN_WITH_CESIUM_TILES=ON; needs an installed cesium-native on CMAKE_PREFIX_PATH.

find_package(cesium-native CONFIG REQUIRED)

add_library(
    mbgl-cesium-tiles STATIC
    ${PROJECT_SOURCE_DIR}/platform/cesium/src/cesium_tiles_host.cpp
)

target_include_directories(
    mbgl-cesium-tiles
    PUBLIC ${PROJECT_SOURCE_DIR}/platform/cesium/include
)

target_link_libraries(
    mbgl-cesium-tiles
    PUBLIC
        mbgl-core
        Cesium3DTilesSelection
        CesiumCurl
        CesiumGltfContent
        CesiumGltf
        CesiumImage
        CesiumGeospatial
        CesiumGeometry
        CesiumAsync
        CesiumUtility
)

if(APPLE)
    find_library(MLN_CESIUM_OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(mbgl-cesium-tiles PUBLIC ${MLN_CESIUM_OPENGL_FRAMEWORK})
elseif(ANDROID)
    target_link_libraries(mbgl-cesium-tiles PUBLIC GLESv3)
else()
    target_link_libraries(mbgl-cesium-tiles PUBLIC GLESv2)
endif()

target_compile_definitions(
    mbgl-cesium-tiles
    PUBLIC MLN_WITH_CESIUM_TILES=1
)
