# Cesium 3D Tiles layer

A custom layer that streams an OGC 3D Tiles tileset (photogrammery meshes and other glTF
content) through [cesium-native](https://github.com/CesiumGS/cesium-native) and draws it with
OpenGL ES 3 inside MapLibre's projection, so it sits on the 3D terrain and is occluded by it.

- `include/mln/cesium/cesium_tiles_host.hpp`: the `CustomLayerHost`. Add it as
  `CustomLayer("cesium-tiles", std::make_unique<CesiumTilesHost>(tilesetUrl))`.
- `CustomLayerHost::rendersIn3D()`: custom layers are normally drawn into the terrain drape;
  a 3D host draws in the main pass with the 3D depth mode instead (the core change in
  `render_custom_layer.cpp` and `drawable_custom_layer_host_tweaker.cpp`).
- `include/mln/cesium/file_source_asset_accessor.hpp`: tileset and tile requests go through the
  map's own network `FileSource`, so they share its HTTP client (OkHttp on Android, with the
  app's TLS, proxy and auth interceptors), its retries for timeouts and 5xx answers, and its
  cache. A tileset therefore loads from wherever the map's tiles can.
- `CustomLayerRenderParameters` carries `pixelRatio`, `centerAltitude`, `terrainEnabled` and
  `terrainExaggeration`: the host orbits the same point as MapLibre's camera, measures screen
  space error in device pixels, and on a flat map sets the tileset's base on the ground plane.
- Camera gates: `minimumZoom` and `minimumPitchDegrees` (constructor, Android `CesiumTilesLayer`)
  fade the layer in over the zoom level and the 10 degrees of pitch below them; below either it is
  idle, with no tile selection, no requests and no drawing. `setEnabled(false)` makes it idle too,
  e.g. while its server is offline. The harness reads `MLN_CESIUM_MIN_ZOOM` / `MLN_CESIUM_MIN_PITCH`.
- Info layers on the mesh: with terrain on, the host reports the footprint it drew
  (`CustomLayerHost::modelOverlayBounds`). Under it the map renders the draped fills and lines
  alone (layer groups opt in with `setDrapesOntoModels`, set by fill and line layers) into
  transparent 512 px overlay tiles of their own (`RendererImpl::modelOverlayPool`), not the
  terrain's: terrain tiles stop at the DEM's zoom (15 for the Israel DTM), far too coarse for a
  close view of a building. Starting from the terrain tiles under the model, a tile is split
  while its texels would show larger than a device pixel, up to zoom 19 and 24 tiles. Each is a
  drape target in overlay-only mode (`RenderTarget::setModelOverlayOnly`) with the drape cache.
  They reach the host in `CustomLayerRenderParameters::modelOverlays`, with a version that changes
  whenever one re-renders. The host redraws the mesh at the same depth (`invariant gl_Position`),
  sampling them mipmapped and anisotropic, faded only on near-vertical faces. `NO_OVERLAY` turns
  the pass off; `OVERLAY_DEBUG` tints each overlay tile by zoom. Not done for a flat map or
  backends other than OpenGL. Zoom 20+ overlay targets came out empty on Android and are capped
  until that is understood.
- Memory: the tile cache is capped at 128 MB and `trimMemory()` evicts everything the view does not
  show (everything, while hidden) on the next frame, without starting downloads.
- Tiles load only while frames are drawn. `CesiumTilesHost::isLoading()` (Android:
  `CesiumTilesLayer.isLoading`) tells the app to keep repainting until the view is refined.
- `MLN_CESIUM_HEIGHT_OFFSET=<metres>`: lifts or lowers a tileset whose vertical datum is off.
  Cesium's public AGI HQ sample sits at about −187 m against AWS Terrarium terrain.
- Diagnostics switches, as `MLN_CESIUM_<NAME>=1` in the environment or, on Android, the system
  property `debug.mln.cesium.<name>` (`adb shell setprop debug.mln.cesium.no_draw 1`, read every
  frame, so no rebuild): `NO_TILESET` (never load), `NO_DRAW` (load, never draw), `NO_DEPTH`,
  `NO_CULL`, `SOLID` (paint the mesh magenta), `NO_RESTORE` (skip the GL state guard).
- The layer restores every piece of GL state it touches before returning (`GlStateGuard`).
  Without it, imagery tiles intermittently came out black for the whole session once a mesh
  had drawn (about 4 cold starts in 10 on the emulator): MapLibre caches GL state, and the
  leftover state reached its later raster work.

## Building

cesium-native is a sibling checkout, built and installed first (its vcpkg bootstrap takes a
while the first time):

```sh
cd ../cesium-native
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCESIUM_TESTS_ENABLED=OFF -DCESIUM_DISABLE_SPZ=ON
cmake --build build && cmake --install build --prefix "$PWD/install"
```

Then MapLibre with `-DMLN_WITH_CESIUM_TILES=ON` and `CMAKE_PREFIX_PATH` holding the install
prefix and cesium-native's vcpkg `installed/<triplet>` directory (see `VCPKG_INSTALLED_DIR`
in its CMakeCache). Both `mbgl-glfw` (`--tileset <url>`) and `mbgl-render` (`--tileset <url>`,
renders frames until the tiles have loaded) pick the layer up.

## Rendering the OpenGL path on a Mac

macOS cannot run MapLibre's OpenGL ES backend, so the GL path (the one Android uses) is
built and rendered in a Linux container with Mesa:

```sh
docker build -t maplibre-linux-gl -f platform/cesium/docker/Dockerfile.linux-gl platform/cesium/docker
docker run --rm -v "$PWD/..:/work" -v maplibre-ezvcpkg:/root/.ezvcpkg maplibre-linux-gl
# inside: build cesium-native with -DVCPKG_TARGET_TRIPLET=arm64-linux -DVCPKG_HOST_TRIPLET=arm64-linux,
# then MapLibre with `cmake --preset linux-opengl -DMLN_WITH_CESIUM_TILES=ON ...`, and
EGL_PLATFORM=surfaceless build-linux-opengl/bin/mbgl-render --style platform/cesium/styles/imagery-terrain.json \
  -x -75.59671 -y 40.0388 -z 16.5 -p 60 -b 20 --tileset https://pelican-public.s3.amazonaws.com/3dtiles/agi-hq/tileset.json --output out.png
```

The Metal build on the Mac compiles and links the layer too, which is the quick compile check,
but cannot draw it.
