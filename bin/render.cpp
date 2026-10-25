#include <mln/map/map.hpp>
#include <mln/map/map_options.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <mln/gfx/backend.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/style/style.hpp>
#include <mln/style/layers/custom_layer.hpp>
#if MLN_WITH_CESIUM_TILES
#include <mln/cesium/cesium_tiles_host.hpp>
#endif

#include <args.hxx>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <fstream>
#include <thread>

namespace {

// Lets the tool add runtime layers (the Cesium tiles layer) once the style has loaded.
class StyleObserver : public mln::MapObserver {
public:
    std::function<void()> onStyleLoaded;
    void onDidFinishLoadingStyle() override {
        if (onStyleLoaded) onStyleLoaded();
    }
};

} // namespace

int main(int argc, char* argv[]) {
    args::ArgumentParser argumentParser("MapLibre Native render tool");
    args::HelpFlag helpFlag(argumentParser, "help", "Display this help menu", {"help"});

    args::ValueFlag<std::string> backendValue(argumentParser, "Backend", "Rendering backend", {"backend"});
    args::ValueFlag<std::string> apikeyValue(argumentParser, "key", "API key", {'t', "apikey"});
    args::ValueFlag<std::string> styleValue(
        argumentParser, "URL|file|-", "Map stylesheet (use '-' for stdin)", {'s', "style"});
    args::ValueFlag<std::string> outputValue(
        argumentParser, "file|-", "Output file name (use '-' for stdout)", {'o', "output"});
    args::ValueFlag<std::string> cacheValue(argumentParser, "file", "Cache database file name", {'c', "cache"});
    args::ValueFlag<std::string> assetsValue(
        argumentParser, "file", "Directory to which asset:// URLs will resolve", {'a', "assets"});

    args::Flag debugFlag(argumentParser, "debug", "Debug mode", {"debug"});

    args::ValueFlag<double> pixelRatioValue(argumentParser, "number", "Image scale factor", {'r', "ratio"});

    // grouping ensures either bounds or center based position is used
    args::Group boundsOrCenterZoom(argumentParser, "Position (either one):", args::Group::Validators::AtMostOne);

    args::NargsValueFlag<double> boundsValue(
        boundsOrCenterZoom, "degrees: north west south east", "Bounds of rendered map", {"bounds"}, 4);

    args::Group centerGroup(boundsOrCenterZoom, "Center:", args::Group::Validators::AtLeastOne);
    args::ValueFlag<double> zoomValue(centerGroup, "number", "Zoom level", {'z', "zoom"});
    args::ValueFlag<double> lonValue(centerGroup, "degrees", "Longitude", {'x', "lon"});
    args::ValueFlag<double> latValue(centerGroup, "degrees", "Latitude", {'y', "lat"});
    args::ValueFlag<double> altValue(argumentParser, "degrees", "Altitude", {'A', "alt"});
    args::ValueFlag<double> fovValue(argumentParser, "degrees", "FOV", {'f', "fov"});
    args::ValueFlag<double> bearingValue(argumentParser, "degrees", "Bearing", {'b', "bearing"});
    args::ValueFlag<double> pitchValue(argumentParser, "degrees", "Pitch", {'p', "pitch"});
    args::ValueFlag<double> rollValue(argumentParser, "degrees", "Roll", {'R', "roll"});
    args::ValueFlag<uint32_t> widthValue(argumentParser, "pixels", "Image width", {'w', "width"});
    args::ValueFlag<uint32_t> heightValue(argumentParser, "pixels", "Image height", {'h', "height"});

    args::ValueFlag<std::string> mapModeValue(
        argumentParser, "MapMode", "Map mode (e.g. 'static', 'tile', 'continuous')", {'m', "mode"});
    args::ValueFlag<std::string> tilesetValue(
        argumentParser,
        "URL",
        "OGC 3D Tiles tileset.json drawn as a Cesium layer; frames are rendered until its tiles have loaded",
        {"tileset"});

    try {
        argumentParser.ParseCLI(argc, argv);
    } catch (const args::Help&) {
        std::cout << argumentParser;
        exit(0);
    } catch (const args::ParseError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(1);
    } catch (const args::ValidationError& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << argumentParser;
        exit(2);
    }

    const double lat = latValue ? args::get(latValue) : 0;
    const double lon = lonValue ? args::get(lonValue) : 0;
    const double alt = altValue ? args::get(altValue) : 0;
    const double zoom = zoomValue ? args::get(zoomValue) : 0;
    const double fov = fovValue ? args::get(fovValue) : mln::util::rad2deg(mln::util::DEFAULT_FOV);
    const double bearing = bearingValue ? args::get(bearingValue) : 0;
    const double pitch = pitchValue ? args::get(pitchValue) : 0;
    const double roll = rollValue ? args::get(rollValue) : 0;
    const double pixelRatio = pixelRatioValue ? args::get(pixelRatioValue) : 1;

    const uint32_t width = widthValue ? args::get(widthValue) : 512;
    const uint32_t height = heightValue ? args::get(heightValue) : 512;
    const std::string output = outputValue ? args::get(outputValue) : "out.png";
    const std::string cache_file = cacheValue ? args::get(cacheValue) : "cache.sqlite";
    const std::string asset_root = assetsValue ? args::get(assetsValue) : ".";

    // Try to load the apikey from the environment.
    const char* apikeyEnv = getenv("MLN_API_KEY");
    const std::string apikey = apikeyValue ? args::get(apikeyValue) : (apikeyEnv ? apikeyEnv : std::string());

    const bool debug = debugFlag ? args::get(debugFlag) : false;

    using namespace mln;

    auto mapTilerConfiguration = mln::TileServerOptions::MapTilerConfiguration();

    std::string json;
    std::string style = styleValue ? args::get(styleValue) : mapTilerConfiguration.defaultStyles().at(0).getUrl();

    util::RunLoop loop;

    MapMode mapMode = MapMode::Static;
    if (mapModeValue) {
        const auto modeStr = args::get(mapModeValue);
        if (modeStr == "tile") {
            mapMode = MapMode::Tile;
        } else if (modeStr == "continuous") {
            mapMode = MapMode::Continuous;
        }
    }

    HeadlessFrontend frontend({width, height}, static_cast<float>(pixelRatio));
    StyleObserver observer;
    Map map(
        frontend,
        observer,
        MapOptions().withMapMode(mapMode).withSize(frontend.getSize()).withPixelRatio(static_cast<float>(pixelRatio)),
        ResourceOptions()
            .withCachePath(cache_file)
            .withAssetPath(asset_root)
            .withApiKey(apikey)
            .withTileServerOptions(mapTilerConfiguration));

    if (style == "-") {
        json = std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        map.getStyle().loadJSON(json);
    } else {
        if (style.find("://") == std::string::npos) {
            style = std::string("file://") + style;
        }

        map.getStyle().loadURL(style);
    }
    std::vector<double> bounds = args::get(boundsValue);
    if (bounds.size() == 4) {
        LatLngBounds boundingBox = LatLngBounds::hull(LatLng(bounds[0], bounds[1]), LatLng(bounds[2], bounds[3]));
        map.jumpTo(map.cameraForLatLngBounds(boundingBox, EdgeInsets(), bearing, pitch));
    } else {
        map.jumpTo(CameraOptions()
                       .withCenter(LatLng{lat, lon})
                       .withCenterAltitude(alt)
                       .withZoom(zoom)
                       .withBearing(bearing)
                       .withPitch(pitch)
                       .withRoll(roll)
                       .withFov(fov));
    }

    if (debug) {
        map.setDebug(debug ? mln::MapDebugOptions::TileBorders | mln::MapDebugOptions::ParseStatus
                           : mln::MapDebugOptions::NoDebug);
    }

#if MLN_WITH_CESIUM_TILES
    mln::cesium::CesiumTilesHost* tilesHost = nullptr;
    if (tilesetValue) {
        const std::string tilesetUrl = args::get(tilesetValue);
        observer.onStyleLoaded = [&map, &tilesHost, tilesetUrl] {
            if (map.getStyle().getLayer("cesium-tiles")) return;
            auto host = std::make_unique<mln::cesium::CesiumTilesHost>(tilesetUrl);
            tilesHost = host.get();
            map.getStyle().addLayer(std::make_unique<mln::style::CustomLayer>("cesium-tiles", std::move(host)));
        };
    }
#else
    if (tilesetValue) {
        std::cout << "--tileset needs a build with MLN_WITH_CESIUM_TILES=ON" << std::endl;
        exit(1);
    }
#endif

    try {
        auto rendered = frontend.render(map);
#if MLN_WITH_CESIUM_TILES
        if (tilesetValue) {
            // The tiles stream in over many frames: keep rendering until the layer reports the
            // view's tiles loaded (and a couple of frames past it, so the last uploads draw), or
            // give up after a while and write what there is.
            int settled = 0;
            for (int i = 0; i < 400 && settled < 3; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                rendered = frontend.render(map);
                const float progress = tilesHost ? tilesHost->loadProgress() : 0.0f;
                settled = progress >= 100.0f ? settled + 1 : 0;
                if (i % 20 == 0) std::cerr << "tiles: " << progress << "% loaded" << std::endl;
            }
        }
#endif
        std::string png = encodePNG(rendered.image);
        if (output == "-") {
            std::cout.write(png.data(), png.size());
        } else {
            std::ofstream out(output, std::ios::binary);
            out << png;
            out.close();
        }
    } catch (std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        exit(1);
    }

    return 0;
}
