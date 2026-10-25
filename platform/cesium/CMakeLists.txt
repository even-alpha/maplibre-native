# The Cesium 3D Tiles custom layer: cesium-native does the tileset work, this library
# draws it with OpenGL ES 3 inside MapLibre's own projection. Enabled with
# MLN_WITH_CESIUM_TILES=ON; needs a built cesium-native. Added from the root project and
# from the Android library project, so paths are relative to this directory.

get_filename_component(MLN_CESIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Where cesium-native was installed and where its vcpkg dependencies are, as two plain
# paths (an Android Gradle build cannot pass a semicolon list). They go on the prefix path
# and, for a cross build whose toolchain restricts find_package to the sysroot, on the find
# root path too.
set(MLN_CESIUM_NATIVE_PREFIX "" CACHE PATH "cesium-native install prefix")
set(MLN_CESIUM_VCPKG_DIR "" CACHE PATH "cesium-native's vcpkg installed/<triplet> directory")
foreach(dir IN LISTS MLN_CESIUM_NATIVE_PREFIX MLN_CESIUM_VCPKG_DIR)
    if(dir)
        list(APPEND CMAKE_PREFIX_PATH "${dir}")
        list(APPEND CMAKE_FIND_ROOT_PATH "${dir}")
    endif()
endforeach()

# cesium-native's vcpkg dependencies come as Debug and Release imported configurations;
# without a mapping, a RelWithDebInfo build (what Android Gradle calls release) links the
# Debug ones.
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "Release;RelWithDebInfo;")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "Release;MinSizeRel;")

find_package(cesium-native CONFIG REQUIRED)

add_library(
    mbgl-cesium-tiles STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cesium_tiles_host.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/file_source_asset_accessor.cpp
)

target_include_directories(
    mbgl-cesium-tiles
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# cesium-native's install step does not always lay out its own headers (it did not on
# Linux), so take them from the source tree as well: a sibling checkout by default.
set(MLN_CESIUM_NATIVE_SOURCE_DIR "${MLN_CESIUM_ROOT}/../cesium-native" CACHE PATH "cesium-native source checkout, for its headers")
file(GLOB MLN_CESIUM_NATIVE_INCLUDE_DIRS
    ${MLN_CESIUM_NATIVE_SOURCE_DIR}/*/include
    ${MLN_CESIUM_NATIVE_SOURCE_DIR}/*/generated/include
)
target_include_directories(mbgl-cesium-tiles SYSTEM PUBLIC ${MLN_CESIUM_NATIVE_INCLUDE_DIRS})

target_link_libraries(
    mbgl-cesium-tiles
    PUBLIC
        mbgl-core
        Cesium3DTilesSelection
        Cesium3DTilesContent
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
