#include <mln/cesium/cesium_tiles_host.hpp>

#include <mln/style/layers/custom_layer_render_parameters.hpp>
#include <mln/util/logging.hpp>

#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#define MLN_CESIUM_GLSL_PRELUDE "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#define MLN_CESIUM_GLSL_PRELUDE "#version 330 core\n"
#else
#include <GLES3/gl3.h>
#define MLN_CESIUM_GLSL_PRELUDE "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#endif

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/GlobeTransforms.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/Material.h>
#include <CesiumGltf/MeshPrimitive.h>
#include <CesiumGltf/Model.h>
#include <CesiumGltf/Texture.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumImage/ImageAsset.h>
#include <CesiumUtility/CreditSystem.h>
#include <CesiumUtility/IntrusivePointer.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>
#include <variant>
#include <vector>

namespace mln {
namespace cesium {

namespace {

// MapLibre's tile size: the world is tileSize * 2^zoom pixels wide at the current zoom.
constexpr double tileSize = 512.0;
// Vertex offsets from a tile's mercator origin are stored scaled by this, so that a
// float keeps centimetre precision over a tile a few kilometres across.
constexpr double offsetScale = 1048576.0; // 2^20
constexpr double earthRadius = 6378137.0;

// ---------------------------------------------------------------- cesium-native plumbing

class ThreadTaskProcessor final : public CesiumAsync::ITaskProcessor {
public:
    void startTask(std::function<void()> f) override { std::thread(std::move(f)).detach(); }
};

// What the load thread produces for a tile: geometry already in web mercator, relative
// to one origin per tile, plus the images it samples. No GL here.
struct CpuPrimitive {
    std::vector<float> vertices; // x, y (scaled mercator offsets), z (metres), u, v
    std::vector<uint32_t> indices;
    int32_t imageIndex = -1;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct LoadThreadResult {
    double originX = 0.0; // normalized web mercator, [0, 1]
    double originY = 0.0;
    std::vector<CpuPrimitive> primitives;
    std::map<int32_t, CesiumUtility::IntrusivePointer<CesiumImage::ImageAsset>> images;
};

struct GpuPrimitive {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
    GLuint texture = 0; // 0 = untextured, draw the colour
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct GpuTile {
    double originX = 0.0;
    double originY = 0.0;
    std::vector<GpuPrimitive> primitives;
    std::map<int32_t, GLuint> textures;
};

glm::dvec3 toMercator(const CesiumGeospatial::Cartographic& c) {
    const double x = c.longitude / glm::two_pi<double>() + 0.5;
    const double y = 0.5 - std::log(std::tan(glm::quarter_pi<double>() + c.latitude / 2.0)) / glm::two_pi<double>();
    return {x, y, c.height};
}

template <typename T>
void readIndices(const CesiumGltf::Model& model, int32_t accessor, std::vector<uint32_t>& out) {
    CesiumGltf::AccessorView<T> view(model, accessor);
    if (view.status() != CesiumGltf::AccessorViewStatus::Valid) return;
    out.reserve(static_cast<size_t>(view.size()));
    for (int64_t i = 0; i < view.size(); ++i) {
        out.push_back(static_cast<uint32_t>(view[i]));
    }
}

// Turns a loaded glTF into mercator geometry. Runs on a load thread.
std::unique_ptr<LoadThreadResult> buildTile(const CesiumGltf::Model& model, const glm::dmat4& tileTransform) {
    auto out = std::make_unique<LoadThreadResult>();
    // The tile transform takes glTF content to ECEF; glTF is y-up and 3D Tiles are z-up,
    // and the loader records which applies in the model's extras.
    const glm::dmat4 root = CesiumGltfContent::GltfUtilities::applyGltfUpAxisTransform(model, tileTransform);
    const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;

    struct Pending {
        CpuPrimitive primitive;
        std::vector<glm::dvec3> mercator;
        std::vector<glm::vec2> uv;
    };
    std::vector<Pending> pending;
    glm::dvec2 sum{0.0, 0.0};
    size_t count = 0;

    model.forEachPrimitiveInScene(
        -1,
        [&](const CesiumGltf::Model& m,
            const CesiumGltf::Node&,
            const CesiumGltf::Mesh&,
            const CesiumGltf::MeshPrimitive& primitive,
            const glm::dmat4& nodeTransform) {
            if (primitive.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES) return;
            const auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) return;
            CesiumGltf::AccessorView<glm::vec3> positions(m, posIt->second);
            if (positions.status() != CesiumGltf::AccessorViewStatus::Valid || positions.size() == 0) return;

            Pending p;
            const glm::dmat4 toEcef = root * nodeTransform;
            p.mercator.reserve(static_cast<size_t>(positions.size()));
            for (int64_t i = 0; i < positions.size(); ++i) {
                const glm::dvec3 ecef = glm::dvec3(toEcef * glm::dvec4(glm::dvec3(positions[i]), 1.0));
                const auto carto = ellipsoid.cartesianToCartographic(ecef);
                const glm::dvec3 merc = carto ? toMercator(*carto) : glm::dvec3{0.0, 0.0, 0.0};
                p.mercator.push_back(merc);
                sum += glm::dvec2(merc.x, merc.y);
            }
            count += static_cast<size_t>(positions.size());

            const auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end()) {
                CesiumGltf::AccessorView<glm::vec2> uvs(m, uvIt->second);
                if (uvs.status() == CesiumGltf::AccessorViewStatus::Valid && uvs.size() == positions.size()) {
                    p.uv.reserve(static_cast<size_t>(uvs.size()));
                    for (int64_t i = 0; i < uvs.size(); ++i) p.uv.push_back(uvs[i]);
                }
            }

            if (primitive.indices >= 0 && primitive.indices < static_cast<int32_t>(m.accessors.size())) {
                switch (m.accessors[static_cast<size_t>(primitive.indices)].componentType) {
                    case CesiumGltf::Accessor::ComponentType::UNSIGNED_BYTE:
                        readIndices<uint8_t>(m, primitive.indices, p.primitive.indices);
                        break;
                    case CesiumGltf::Accessor::ComponentType::UNSIGNED_SHORT:
                        readIndices<uint16_t>(m, primitive.indices, p.primitive.indices);
                        break;
                    case CesiumGltf::Accessor::ComponentType::UNSIGNED_INT:
                        readIndices<uint32_t>(m, primitive.indices, p.primitive.indices);
                        break;
                    default:
                        break;
                }
            } else {
                p.primitive.indices.resize(static_cast<size_t>(positions.size()));
                for (size_t i = 0; i < p.primitive.indices.size(); ++i) p.primitive.indices[i] = static_cast<uint32_t>(i);
            }
            if (p.primitive.indices.empty()) return;

            if (primitive.material >= 0 && primitive.material < static_cast<int32_t>(m.materials.size())) {
                const auto& material = m.materials[static_cast<size_t>(primitive.material)];
                if (material.pbrMetallicRoughness) {
                    const auto& pbr = *material.pbrMetallicRoughness;
                    if (pbr.baseColorFactor.size() == 4) {
                        for (size_t i = 0; i < 4; ++i) p.primitive.color[i] = static_cast<float>(pbr.baseColorFactor[i]);
                    }
                    if (pbr.baseColorTexture && pbr.baseColorTexture->index >= 0 &&
                        pbr.baseColorTexture->index < static_cast<int32_t>(m.textures.size())) {
                        const int32_t source = m.textures[static_cast<size_t>(pbr.baseColorTexture->index)].source;
                        if (source >= 0 && source < static_cast<int32_t>(m.images.size())) {
                            const auto& asset = m.images[static_cast<size_t>(source)].pAsset;
                            if (asset && asset->compressedPixelFormat == CesiumImage::GpuCompressedPixelFormat::NONE &&
                                asset->bytesPerChannel == 1 && !asset->pixelData.empty() && !p.uv.empty()) {
                                p.primitive.imageIndex = source;
                                out->images[source] = asset;
                            }
                        }
                    }
                }
            }
            pending.push_back(std::move(p));
        });

    if (count == 0) return nullptr;
    out->originX = sum.x / static_cast<double>(count);
    out->originY = sum.y / static_cast<double>(count);

    for (auto& p : pending) {
        auto& v = p.primitive.vertices;
        v.reserve(p.mercator.size() * 5);
        for (size_t i = 0; i < p.mercator.size(); ++i) {
            const glm::dvec3& m = p.mercator[i];
            v.push_back(static_cast<float>((m.x - out->originX) * offsetScale));
            v.push_back(static_cast<float>((m.y - out->originY) * offsetScale));
            v.push_back(static_cast<float>(m.z));
            const glm::vec2 uv = i < p.uv.size() ? p.uv[i] : glm::vec2{0.0f, 0.0f};
            v.push_back(uv.x);
            v.push_back(uv.y);
        }
        out->primitives.push_back(std::move(p.primitive));
    }
    return out;
}

// Holds the GL side of the tiles. Everything here runs on the render thread, which is
// also the thread that drives cesium-native (its "main thread").
class RendererResources final : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
    bool contextAlive = true;

    CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources> prepareInLoadThread(
        const CesiumAsync::AsyncSystem& asyncSystem,
        Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
        const glm::dmat4& transform,
        const std::any&) override {
        void* result = nullptr;
        if (const auto* model = std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind)) {
            result = buildTile(*model, transform).release();
        }
        return asyncSystem.createResolvedFuture(
            Cesium3DTilesSelection::TileLoadResultAndRenderResources{std::move(tileLoadResult), result});
    }

    void* prepareInMainThread(Cesium3DTilesSelection::Tile&, void* pLoadThreadResult) override {
        std::unique_ptr<LoadThreadResult> loaded(static_cast<LoadThreadResult*>(pLoadThreadResult));
        if (!loaded || !contextAlive) return nullptr;

        auto tile = std::make_unique<GpuTile>();
        tile->originX = loaded->originX;
        tile->originY = loaded->originY;

        for (const auto& [index, asset] : loaded->images) {
            GLenum format = GL_RGBA;
            switch (asset->channels) {
                case 1:
                    format = GL_RED;
                    break;
                case 2:
                    format = GL_RG;
                    break;
                case 3:
                    format = GL_RGB;
                    break;
                default:
                    format = GL_RGBA;
                    break;
            }
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         static_cast<GLint>(format),
                         asset->width,
                         asset->height,
                         0,
                         format,
                         GL_UNSIGNED_BYTE,
                         asset->pixelData.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            tile->textures[index] = texture;
        }

        for (auto& cpu : loaded->primitives) {
            GpuPrimitive gpu;
            gpu.color = cpu.color;
            if (cpu.imageIndex >= 0) {
                if (const auto it = tile->textures.find(cpu.imageIndex); it != tile->textures.end()) {
                    gpu.texture = it->second;
                }
            }
            glGenVertexArrays(1, &gpu.vao);
            glGenBuffers(1, &gpu.vbo);
            glGenBuffers(1, &gpu.ebo);
            glBindVertexArray(gpu.vao);
            glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(cpu.vertices.size() * sizeof(float)),
                         cpu.vertices.data(),
                         GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(cpu.indices.size() * sizeof(uint32_t)),
                         cpu.indices.data(),
                         GL_STATIC_DRAW);
            constexpr GLsizei stride = 5 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
            glBindVertexArray(0);
            gpu.indexCount = static_cast<GLsizei>(cpu.indices.size());
            tile->primitives.push_back(gpu);
        }
        return tile.release();
    }

    void free(Cesium3DTilesSelection::Tile&, void* pLoadThreadResult, void* pMainThreadResult) noexcept override {
        delete static_cast<LoadThreadResult*>(pLoadThreadResult);
        std::unique_ptr<GpuTile> tile(static_cast<GpuTile*>(pMainThreadResult));
        if (!tile || !contextAlive) return;
        for (auto& p : tile->primitives) {
            if (p.vao) glDeleteVertexArrays(1, &p.vao);
            if (p.vbo) glDeleteBuffers(1, &p.vbo);
            if (p.ebo) glDeleteBuffers(1, &p.ebo);
        }
        for (auto& [index, texture] : tile->textures) {
            if (texture) glDeleteTextures(1, &texture);
        }
    }

    void* prepareRasterInLoadThread(CesiumImage::ImageAsset&, const std::any&) override { return nullptr; }
    void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile&, void*) override { return nullptr; }
    void freeRaster(const CesiumRasterOverlays::RasterOverlayTile&, void*, void*) noexcept override {}
    void attachRasterInMainThread(const Cesium3DTilesSelection::Tile&,
                                  int32_t,
                                  const CesiumRasterOverlays::RasterOverlayTile&,
                                  void*,
                                  const glm::dvec2&,
                                  const glm::dvec2&) override {}
    void detachRasterInMainThread(const Cesium3DTilesSelection::Tile&,
                                  int32_t,
                                  const CesiumRasterOverlays::RasterOverlayTile&,
                                  void*) noexcept override {}
};

// ---------------------------------------------------------------- GL program

constexpr const char* vertexSource = MLN_CESIUM_GLSL_PRELUDE
    "uniform mat4 u_matrix;\n"
    "uniform float u_scale;\n"
    "layout (location = 0) in vec3 a_pos;\n"
    "layout (location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_matrix * vec4(a_pos.xy * u_scale, a_pos.z, 1.0);\n"
    "}\n";

constexpr const char* fragmentSource = MLN_CESIUM_GLSL_PRELUDE
    "uniform sampler2D u_texture;\n"
    "uniform int u_textured;\n"
    "uniform vec4 u_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = u_textured == 1 ? texture(u_texture, v_uv) * u_color : u_color;\n"
    "}\n";

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        Log::Error(Event::Render, "Cesium tiles shader failed to compile: " + log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// ---------------------------------------------------------------- the host

class CesiumTilesHost::Impl {
public:
    Impl(std::string url_, double sse) : url(std::move(url_)), maximumScreenSpaceError(sse) {}

    std::string url;
    double maximumScreenSpaceError;

    std::shared_ptr<RendererResources> resources;
    std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset;
    std::optional<CesiumAsync::AsyncSystem> asyncSystem;

    GLuint program = 0;
    GLint uMatrix = -1;
    GLint uScale = -1;
    GLint uTexture = -1;
    GLint uTextured = -1;
    GLint uColor = -1;

    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    void createProgram() {
        const GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (!vs || !fs) return;
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok == GL_FALSE) {
            Log::Error(Event::Render, "Cesium tiles program failed to link");
            glDeleteProgram(program);
            program = 0;
            return;
        }
        uMatrix = glGetUniformLocation(program, "u_matrix");
        uScale = glGetUniformLocation(program, "u_scale");
        uTexture = glGetUniformLocation(program, "u_texture");
        uTextured = glGetUniformLocation(program, "u_textured");
        uColor = glGetUniformLocation(program, "u_color");
    }

    void createTileset() {
        resources = std::make_shared<RendererResources>();
        asyncSystem.emplace(std::make_shared<ThreadTaskProcessor>());
        Cesium3DTilesSelection::TilesetExternals externals{
            std::make_shared<CesiumCurl::CurlAssetAccessor>(),
            resources,
            *asyncSystem,
            std::make_shared<CesiumUtility::CreditSystem>()};
        Cesium3DTilesSelection::TilesetOptions options;
        options.maximumScreenSpaceError = maximumScreenSpaceError;
        options.maximumCachedBytes = 256LL * 1024 * 1024;
        tileset = std::make_unique<Cesium3DTilesSelection::Tileset>(externals, url, options);
        Log::Info(Event::Render, "Cesium tiles: loading " + url);
    }

    // The map camera as cesium-native wants it: an ECEF position, direction and up.
    Cesium3DTilesSelection::ViewState viewFor(const style::CustomLayerRenderParameters& p) const {
        const double fov = p.fieldOfView > 3.2 ? glm::radians(p.fieldOfView) : p.fieldOfView; // radians
        const double pitch = p.pitch > 3.2 ? glm::radians(p.pitch) : p.pitch;
        const double bearing = glm::radians(p.bearing);
        const double lat = glm::radians(p.latitude);
        const double lon = glm::radians(p.longitude);

        const double worldSize = tileSize * std::exp2(p.zoom);
        const double metresPerPixel = glm::two_pi<double>() * earthRadius * std::cos(lat) / worldSize;
        const double distancePx = 0.5 * p.height / std::tan(fov / 2.0);
        const double up = distancePx * std::cos(pitch) * metresPerPixel;
        const double back = distancePx * std::sin(pitch) * metresPerPixel;

        // East-north-up around the map centre: the camera sits behind the centre, opposite
        // the bearing, and looks at it.
        const glm::dvec3 cameraEnu{-std::sin(bearing) * back, -std::cos(bearing) * back, up};
        const glm::dvec3 direction = glm::normalize(-cameraEnu);
        glm::dvec3 cameraUp;
        if (std::abs(direction.z) > 0.999) {
            cameraUp = {std::sin(bearing), std::cos(bearing), 0.0};
        } else {
            const glm::dvec3 zenith{0.0, 0.0, 1.0};
            cameraUp = glm::normalize(glm::cross(glm::cross(direction, zenith), direction));
        }

        const auto& ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
        const glm::dvec3 centreEcef = ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic(lon, lat, 0.0));
        const glm::dmat4 enuToEcef = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(centreEcef, ellipsoid);
        const glm::dmat3 rotate(enuToEcef);
        const glm::dvec3 position = glm::dvec3(enuToEcef * glm::dvec4(cameraEnu, 1.0));

        const double hfov = 2.0 * std::atan(std::tan(fov / 2.0) * p.width / p.height);
        return Cesium3DTilesSelection::ViewState(
            position, rotate * direction, rotate * cameraUp, glm::dvec2(p.width, p.height), hfov, fov, ellipsoid);
    }

    void render(const style::CustomLayerRenderParameters& p) {
        if (!program) return;
        if (!tileset) createTileset();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        asyncSystem->dispatchMainThreadTasks();
        const auto& result = tileset->updateViewGroup(tileset->getDefaultViewGroup(), {viewFor(p)}, dt);
        tileset->loadTiles();

        if (++frame % 120 == 1) {
            Log::Info(Event::Render,
                      "Cesium tiles: " + std::to_string(result.tilesToRenderThisFrame.size()) + " tiles to render, " +
                          std::to_string(result.workerThreadTileLoadQueueLength + result.mainThreadTileLoadQueueLength) +
                          " queued to load");
        }

        const double worldSize = tileSize * std::exp2(p.zoom);
        const auto& proj = p.projectionMatrix; // column-major

        glUseProgram(program);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(uTexture, 0);
        glUniform1f(uScale, static_cast<float>(worldSize / offsetScale));

        for (const auto& tile : result.tilesToRenderThisFrame) {
            const auto* content = tile->getContent().getRenderContent();
            if (!content) continue;
            const auto* gpu = static_cast<const GpuTile*>(content->getRenderResources());
            if (!gpu) continue;

            // proj * translate(origin): fold the tile origin into the matrix in double, so
            // the vertices only carry small offsets.
            const double tx = gpu->originX * worldSize;
            const double ty = gpu->originY * worldSize;
            std::array<float, 16> matrix;
            for (size_t i = 0; i < 16; ++i) matrix[i] = static_cast<float>(proj[i]);
            for (size_t i = 0; i < 4; ++i) {
                matrix[12 + i] = static_cast<float>(proj[i] * tx + proj[4 + i] * ty + proj[12 + i]);
            }
            glUniformMatrix4fv(uMatrix, 1, GL_FALSE, matrix.data());

            for (const auto& prim : gpu->primitives) {
                glUniform4fv(uColor, 1, prim.color.data());
                glUniform1i(uTextured, prim.texture ? 1 : 0);
                if (prim.texture) glBindTexture(GL_TEXTURE_2D, prim.texture);
                glBindVertexArray(prim.vao);
                glDrawElements(GL_TRIANGLES, prim.indexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    void destroy(bool contextAlive) {
        if (resources) resources->contextAlive = contextAlive;
        tileset.reset();
        asyncSystem.reset();
        resources.reset();
        if (program && contextAlive) glDeleteProgram(program);
        program = 0;
    }
};

CesiumTilesHost::CesiumTilesHost(std::string tilesetUrl, double maximumScreenSpaceError)
    : impl(std::make_unique<Impl>(std::move(tilesetUrl), maximumScreenSpaceError)) {}

CesiumTilesHost::~CesiumTilesHost() = default;

void CesiumTilesHost::initialize(const style::CustomLayerInitParameters&) {
    impl->createProgram();
}

void CesiumTilesHost::render(const style::CustomLayerRenderParameters& parameters) {
    impl->render(parameters);
}

void CesiumTilesHost::contextLost() {
    impl->destroy(/*contextAlive=*/false);
}

void CesiumTilesHost::deinitialize() {
    impl->destroy(/*contextAlive=*/true);
}

float CesiumTilesHost::loadProgress() {
    return impl->tileset ? impl->tileset->computeLoadProgress() : 0.0f;
}

} // namespace cesium
} // namespace mln
