#include <mln/cesium/cesium_tiles_host.hpp>

#include <mln/cesium/file_source_asset_accessor.hpp>
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

#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumGeospatial/BoundingRegion.h>
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
#include <spdlog/spdlog.h>
#if defined(__ANDROID__)
#include <spdlog/sinks/android_sink.h>
#include <sys/system_properties.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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

// Diagnostics switches: MLN_CESIUM_<NAME> in the environment, or on Android the system
// property debug.mln.cesium.<name> (adb shell setprop debug.mln.cesium.no_draw 1).
bool debugFlag(const std::string& name) {
    if (std::getenv(("MLN_CESIUM_" + name).c_str())) return true;
#if defined(__ANDROID__)
    std::string property = "debug.mln.cesium.";
    for (const char c : name) property += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(property.c_str(), value) > 0 && value[0] == '1') return true;
#endif
    return false;
}

// The switches above, read about once a second rather than every frame.
struct DebugFlags {
    bool noTileset = false;
    bool noDraw = false;
    bool noDepth = false;
    bool solid = false;
    bool noRestore = false;
    bool noOverlay = false;

    static DebugFlags read() {
        return {debugFlag("NO_TILESET"),
                debugFlag("NO_DRAW"),
                debugFlag("NO_DEPTH"),
                debugFlag("SOLID"),
                debugFlag("NO_RESTORE"),
                debugFlag("NO_OVERLAY")};
    }
};

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
    size_t vertexCount = 0;
    double minZ = 0.0;
    double maxZ = 0.0;
    // Footprint in normalized web mercator, for the draped-vector overlays.
    std::array<double, 4> bounds{0.0, 0.0, 0.0, 0.0};
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
    size_t vertexCount = 0;
    double minZ = 0.0;
    double maxZ = 0.0;
    std::array<double, 4> bounds{0.0, 0.0, 0.0, 0.0};
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

// Turns a loaded glTF into mercator geometry. Runs on a load thread. `heightOffset` lifts
// or lowers the whole tileset (metres), for data whose vertical datum is off.
std::unique_ptr<LoadThreadResult> buildTile(const CesiumGltf::Model& model,
                                            const glm::dmat4& tileTransform,
                                            double heightOffset) {
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
    out->vertexCount = count;
    out->minZ = std::numeric_limits<double>::max();
    out->maxZ = std::numeric_limits<double>::lowest();
    out->bounds = {std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::lowest(),
                   std::numeric_limits<double>::lowest()};

    for (auto& p : pending) {
        auto& v = p.primitive.vertices;
        v.reserve(p.mercator.size() * 5);
        for (size_t i = 0; i < p.mercator.size(); ++i) {
            const glm::dvec3& m = p.mercator[i];
            out->minZ = std::min(out->minZ, m.z + heightOffset);
            out->maxZ = std::max(out->maxZ, m.z + heightOffset);
            out->bounds = {std::min(out->bounds[0], m.x),
                           std::min(out->bounds[1], m.y),
                           std::max(out->bounds[2], m.x),
                           std::max(out->bounds[3], m.y)};
            v.push_back(static_cast<float>((m.x - out->originX) * offsetScale));
            v.push_back(static_cast<float>((m.y - out->originY) * offsetScale));
            v.push_back(static_cast<float>(m.z + heightOffset));
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
    double heightOffset = 0.0;

    CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources> prepareInLoadThread(
        const CesiumAsync::AsyncSystem& asyncSystem,
        Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
        const glm::dmat4& transform,
        const std::any&) override {
        void* result = nullptr;
        if (const auto* model = std::get_if<CesiumGltf::Model>(&tileLoadResult.contentKind)) {
            result = buildTile(*model, transform, heightOffset).release();
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
        tile->vertexCount = loaded->vertexCount;
        tile->minZ = loaded->minZ;
        tile->maxZ = loaded->maxZ;
        tile->bounds = loaded->bounds;

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

// Puts back the GL state the layer touches. MapLibre caches GL state and only resets its
// cache after a custom layer, so anything left changed can reach a later draw or upload
// through a stale cache entry; leaving GL exactly as found keeps the layer neutral.
class GlStateGuard {
public:
    GlStateGuard() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &activeUnitTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0Texture);
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        blend = glIsEnabled(GL_BLEND);
        cullFace = glIsEnabled(GL_CULL_FACE);
        stencilTest = glIsEnabled(GL_STENCIL_TEST);
    }

    ~GlStateGuard() {
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit0Texture));
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(activeUnitTexture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
        glDepthFunc(static_cast<GLenum>(depthFunc));
        glDepthMask(depthMask);
        glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb),
                            static_cast<GLenum>(blendDstRgb),
                            static_cast<GLenum>(blendSrcAlpha),
                            static_cast<GLenum>(blendDstAlpha));
        setEnabled(GL_DEPTH_TEST, depthTest);
        setEnabled(GL_BLEND, blend);
        setEnabled(GL_CULL_FACE, cullFace);
        setEnabled(GL_STENCIL_TEST, stencilTest);
    }

    GlStateGuard(const GlStateGuard&) = delete;
    GlStateGuard& operator=(const GlStateGuard&) = delete;

private:
    static void setEnabled(GLenum capability, GLboolean enabled) {
        if (enabled) {
            glEnable(capability);
        } else {
            glDisable(capability);
        }
    }

    GLint program = 0;
    GLint vertexArray = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint activeUnitTexture = 0;
    GLint unit0Texture = 0;
    GLint unpackAlignment = 4;
    GLint depthFunc = GL_LESS;
    GLint blendSrcRgb = GL_ONE;
    GLint blendDstRgb = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;
    GLboolean depthMask = GL_TRUE;
    GLboolean depthTest = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean cullFace = GL_FALSE;
    GLboolean stencilTest = GL_FALSE;
};

// ---------------------------------------------------------------- GL program

constexpr const char* vertexSource = MLN_CESIUM_GLSL_PRELUDE
    "invariant gl_Position;\n"
    "uniform mat4 u_matrix;\n"
    "uniform float u_scale;\n"
    "uniform float u_z_scale;\n"
    "uniform float u_z_offset;\n"
    "layout (location = 0) in vec3 a_pos;\n"
    "layout (location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_matrix * vec4(a_pos.xy * u_scale, a_pos.z * u_z_scale + u_z_offset, 1.0);\n"
    "}\n";

constexpr const char* fragmentSource = MLN_CESIUM_GLSL_PRELUDE
    "uniform sampler2D u_texture;\n"
    "uniform int u_textured;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_opacity;\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 color = u_textured == 1 ? texture(u_texture, v_uv) * u_color : u_color;\n"
    "    fragColor = vec4(color.rgb, 1.0) * u_opacity;\n"
    "}\n";

// The draped fills and lines painted onto the mesh: the same geometry drawn again at the
// same depth (both programs keep gl_Position invariant), sampling the overlay texture of a
// terrain tile at the fragment's map position. Fades out on steep faces, where a top-down
// projection would smear down the walls.
constexpr const char* overlayVertexSource = MLN_CESIUM_GLSL_PRELUDE
    "invariant gl_Position;\n"
    "uniform mat4 u_matrix;\n"
    "uniform float u_scale;\n"
    "uniform float u_z_scale;\n"
    "uniform float u_z_offset;\n"
    "uniform vec2 u_overlay_offset;\n"
    "uniform float u_overlay_scale;\n"
    "uniform float u_pixels_per_metre;\n"
    "layout (location = 0) in vec3 a_pos;\n"
    "out vec2 v_overlay;\n"
    "out vec3 v_world;\n"
    "void main() {\n"
    "    float z = a_pos.z * u_z_scale + u_z_offset;\n"
    "    v_overlay = u_overlay_offset + a_pos.xy * u_overlay_scale;\n"
    "    v_world = vec3(a_pos.xy * u_scale, z * u_pixels_per_metre);\n"
    "    gl_Position = u_matrix * vec4(a_pos.xy * u_scale, z, 1.0);\n"
    "}\n";

constexpr const char* overlayFragmentSource = MLN_CESIUM_GLSL_PRELUDE
    "uniform sampler2D u_overlay;\n"
    "uniform float u_opacity;\n"
    "in vec2 v_overlay;\n"
    "in vec3 v_world;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    if (any(lessThan(v_overlay, vec2(0.0))) || any(greaterThan(v_overlay, vec2(1.0)))) discard;\n"
    "    vec3 normal = normalize(cross(dFdx(v_world), dFdy(v_world)));\n"
    "    float facingUp = smoothstep(0.35, 0.6, abs(normal.z));\n"
    "    vec4 color = texture(u_overlay, vec2(v_overlay.x, 1.0 - v_overlay.y)) * (facingUp * u_opacity);\n"
    "    if (color.a < 0.004) discard;\n"
    "    fragColor = color;\n"
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

GLuint linkProgram(const char* vertex, const char* fragment) {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
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
        return 0;
    }
    return program;
}

} // namespace

// ---------------------------------------------------------------- the host

class CesiumTilesHost::Impl {
public:
    Impl(std::string url_, double sse, double heightOffset_, double minimumZoom_, double minimumPitchDegrees)
        : url(std::move(url_)),
          maximumScreenSpaceError(sse),
          heightOffset(heightOffset_),
          minimumZoom(minimumZoom_),
          minimumPitch(glm::radians(minimumPitchDegrees)) {}

    std::string url;
    double maximumScreenSpaceError;
    // Metres added to every vertex's height. cesium-native culls and picks detail in the
    // data's own frame, so the camera it sees is moved the other way (viewFor).
    double heightOffset;
    double minimumZoom;
    double minimumPitch;

    // Written by the render thread, read from any thread.
    std::atomic<bool> enabled{true};
    std::atomic<bool> loading{false};
    std::atomic<float> progress{0.0f};
    std::atomic<bool> trimRequested{false};

    std::shared_ptr<RendererResources> resources;
    std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset;
    std::optional<CesiumAsync::AsyncSystem> asyncSystem;

    GLuint program = 0;
    GLint uMatrix = -1;
    GLint uScale = -1;
    GLint uZScale = -1;
    GLint uZOffset = -1;
    GLint uTexture = -1;
    GLint uTextured = -1;
    GLint uColor = -1;
    GLint uOpacity = -1;

    GLuint overlayProgram = 0;
    GLint oMatrix = -1;
    GLint oScale = -1;
    GLint oZScale = -1;
    GLint oZOffset = -1;
    GLint oOverlayOffset = -1;
    GLint oOverlayScale = -1;
    GLint oPixelsPerMetre = -1;
    GLint oOverlay = -1;
    GLint oOpacity = -1;

    // The mesh's footprint last drawn, which the map renders draped fills and lines for
    // (modelOverlayBounds); empty while hidden. Read on the render thread only.
    std::vector<std::array<double, 4>> overlayBounds;

    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    uint64_t frame = 0;
    size_t tilesRenderedLastFrame = 0;
    size_t tilesLoggedLastTime = 0;
    size_t tilesQueuedLastFrame = 0;
    // The tileset's lowest point (drawn height), for the flat map.
    double flatBase = 0.0;
    bool flatBaseKnown = false;
    DebugFlags flags;
    bool hiddenLastFrame = false;

    // How visible the camera lets the layer be, 0..1: it fades in over the zoom level below
    // minimumZoom and over the 10 degrees of pitch below minimumPitch.
    double visibilityFor(const style::CustomLayerRenderParameters& p) const {
        constexpr double pitchFade = glm::radians(10.0);
        const double pitch = p.pitch > 3.2 ? glm::radians(p.pitch) : p.pitch;
        double visibility = 1.0;
        if (minimumZoom > 0.0) {
            visibility *= glm::clamp(p.zoom - (minimumZoom - 1.0), 0.0, 1.0);
        }
        if (minimumPitch > 0.0) {
            visibility *= glm::smoothstep(minimumPitch - pitchFade, minimumPitch, pitch);
        }
        return visibility;
    }

    // Evicts every cached tile not in the current selection, starting no new loads. While
    // hidden the selection is dropped first, so everything evictable goes.
    void evictCachedTiles(bool hidden) {
        auto& options = tileset->getOptions();
        const auto cachedBytes = options.maximumCachedBytes;
        const auto simultaneousLoads = options.maximumSimultaneousTileLoads;
        options.maximumCachedBytes = 0;
        options.maximumSimultaneousTileLoads = 0;
        if (hidden) tileset->updateViewGroup(tileset->getDefaultViewGroup(), {}, 0.0f);
        tileset->loadTiles();
        options.maximumCachedBytes = cachedBytes;
        options.maximumSimultaneousTileLoads = simultaneousLoads;
        Log::Info(Event::Render,
                  "Cesium tiles: trimmed to " + std::to_string(tileset->getTotalDataBytes() / (1024 * 1024)) + " MB");
    }

    void createProgram() {
        overlayProgram = linkProgram(overlayVertexSource, overlayFragmentSource);
        if (overlayProgram) {
            oMatrix = glGetUniformLocation(overlayProgram, "u_matrix");
            oScale = glGetUniformLocation(overlayProgram, "u_scale");
            oZScale = glGetUniformLocation(overlayProgram, "u_z_scale");
            oZOffset = glGetUniformLocation(overlayProgram, "u_z_offset");
            oOverlayOffset = glGetUniformLocation(overlayProgram, "u_overlay_offset");
            oOverlayScale = glGetUniformLocation(overlayProgram, "u_overlay_scale");
            oPixelsPerMetre = glGetUniformLocation(overlayProgram, "u_pixels_per_metre");
            oOverlay = glGetUniformLocation(overlayProgram, "u_overlay");
            oOpacity = glGetUniformLocation(overlayProgram, "u_opacity");
        }
        program = linkProgram(vertexSource, fragmentSource);
        if (!program) return;
        uMatrix = glGetUniformLocation(program, "u_matrix");
        uScale = glGetUniformLocation(program, "u_scale");
        uZScale = glGetUniformLocation(program, "u_z_scale");
        uZOffset = glGetUniformLocation(program, "u_z_offset");
        uTexture = glGetUniformLocation(program, "u_texture");
        uTextured = glGetUniformLocation(program, "u_textured");
        uColor = glGetUniformLocation(program, "u_color");
        uOpacity = glGetUniformLocation(program, "u_opacity");
    }

    void createTileset() {
        // Teaches cesium-native the tile formats (b3dm, i3dm, pnts, cmpt, glb); without this
        // every tile is taken for an external tileset.json.
        Cesium3DTilesContent::registerAllTileContentTypes();
        if (const char* offset = std::getenv("MLN_CESIUM_HEIGHT_OFFSET")) heightOffset = std::atof(offset);
        resources = std::make_shared<RendererResources>();
        resources->heightOffset = heightOffset;
        asyncSystem.emplace(std::make_shared<ThreadTaskProcessor>());
        Cesium3DTilesSelection::TilesetExternals externals{std::make_shared<FileSourceAssetAccessor>(),
                                                           resources,
                                                           *asyncSystem,
                                                           std::make_shared<CesiumUtility::CreditSystem>()};
#if defined(__ANDROID__)
        // cesium-native's own messages (failed requests, bad content) reach logcat instead of a
        // stdout nobody reads.
        static std::shared_ptr<spdlog::logger> androidLogger = spdlog::android_logger_mt("cesium-native", "CesiumNative");
        externals.pLogger = androidLogger;
#endif
        Cesium3DTilesSelection::TilesetOptions options;
        options.maximumScreenSpaceError = maximumScreenSpaceError;
        // Phone-sized: imagery, terrain and feature layers share the device with it.
        options.maximumCachedBytes = 128LL * 1024 * 1024;
        options.maximumSimultaneousTileLoads = 12;
        if (debugFlag("NO_CULL")) options.enableFrustumCulling = false; // diagnostics
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

        // East-north-up around the point the camera orbits: the map centre at its altitude
        // (the ground under it when the centre is clamped to terrain). The camera sits
        // behind it, opposite the bearing, and looks at it. Drawn heights are metres above
        // sea level on terrain and relative to the flat plane otherwise, so in the data's
        // frame the orbit point is that much higher or lower.
        const double orbitAltitude = (p.terrainEnabled ? p.centerAltitude / p.terrainExaggeration
                                                       : p.centerAltitude + flatBase) -
                                     heightOffset;
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
        const glm::dvec3 centreEcef =
            ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic(lon, lat, orbitAltitude));
        const glm::dmat4 enuToEcef = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(centreEcef, ellipsoid);
        const glm::dmat3 rotate(enuToEcef);
        const glm::dvec3 position = glm::dvec3(enuToEcef * glm::dvec4(cameraEnu, 1.0));

        const double hfov = 2.0 * std::atan(std::tan(fov / 2.0) * p.width / p.height);
        // Screen-space error is judged in device pixels; the map's size is logical.
        const double ratio = p.pixelRatio > 0.0 ? p.pixelRatio : 1.0;
        return Cesium3DTilesSelection::ViewState(position,
                                                 rotate * direction,
                                                 rotate * cameraUp,
                                                 glm::dvec2(p.width * ratio, p.height * ratio),
                                                 hfov,
                                                 fov,
                                                 ellipsoid);
    }

    void render(const style::CustomLayerRenderParameters& p) {
        if (frame++ % 60 == 0) flags = DebugFlags::read();
        if (!program || flags.noTileset) return;
        std::optional<GlStateGuard> stateGuard;
        if (!flags.noRestore) stateGuard.emplace();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        const double opacity = enabled ? visibilityFor(p) : 0.0;
        if (opacity <= 0.0) {
            // Idle: no selection and no loadTiles, so nothing new is requested. Loads already
            // in flight still finish into the cache.
            if (tileset) {
                asyncSystem->dispatchMainThreadTasks();
                if (trimRequested.exchange(false)) evictCachedTiles(/*hidden=*/true);
            }
            if (!hiddenLastFrame) Log::Info(Event::Render, "Cesium tiles: hidden");
            hiddenLastFrame = true;
            loading = !overlayBounds.empty();
            overlayBounds.clear();
            return;
        }
        if (hiddenLastFrame) Log::Info(Event::Render, "Cesium tiles: shown");
        hiddenLastFrame = false;

        if (!tileset) createTileset();
        asyncSystem->dispatchMainThreadTasks();
        if (!flatBaseKnown) {
            if (const auto* root = tileset->getRootTile()) {
                // Off terrain the map is a flat plane: the tileset's lowest point lands on it.
                const auto region = Cesium3DTilesSelection::getBoundingRegionFromBoundingVolume(root->getBoundingVolume());
                flatBase = region ? region->getMinimumHeight() + heightOffset : 0.0;
                flatBaseKnown = true;
            }
        }
        const auto view = viewFor(p);
        const auto& result = tileset->updateViewGroup(tileset->getDefaultViewGroup(), {view}, dt);
        tileset->loadTiles();
        if (trimRequested.exchange(false)) evictCachedTiles(/*hidden=*/false);
        tilesRenderedLastFrame = result.tilesToRenderThisFrame.size();
        tilesQueuedLastFrame = static_cast<size_t>(result.workerThreadTileLoadQueueLength + result.mainThreadTileLoadQueueLength);
        // cesium-native reports 100% whenever nothing is queued, which is also the case before
        // the tileset.json has arrived or before the first tile was selected: not loaded yet.
        progress = tileset->getRootTile() && tilesRenderedLastFrame > 0 ? tileset->computeLoadProgress() : 0.0f;
        loading = tilesQueuedLastFrame > 0 || progress < 100.0f;

        // The footprint drawn this frame: the map renders draped fills and lines for it, ready
        // the next frame, so a changed footprint asks for one more frame.
        std::array<double, 4> footprint{std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::lowest(),
                                        std::numeric_limits<double>::lowest()};
        for (const auto& tile : result.tilesToRenderThisFrame) {
            const auto* content = tile->getContent().getRenderContent();
            const auto* gpu = content ? static_cast<const GpuTile*>(content->getRenderResources()) : nullptr;
            if (!gpu) continue;
            footprint = {std::min(footprint[0], gpu->bounds[0]),
                         std::min(footprint[1], gpu->bounds[1]),
                         std::max(footprint[2], gpu->bounds[2]),
                         std::max(footprint[3], gpu->bounds[3])};
        }
        std::vector<std::array<double, 4>> newBounds;
        if (footprint[0] <= footprint[2]) newBounds.push_back(footprint);
        if (newBounds != overlayBounds) {
            overlayBounds = std::move(newBounds);
            loading = true;
        }

        if (frame % 120 == 1 || result.tilesToRenderThisFrame.size() != tilesLoggedLastTime) {
            tilesLoggedLastTime = result.tilesToRenderThisFrame.size();
            const auto carto = CesiumGeospatial::Ellipsoid::WGS84.cartesianToCartographic(view.getPosition());
            std::string camera = "camera off the ellipsoid";
            if (carto) {
                camera = "camera lon " + std::to_string(glm::degrees(carto->longitude)) + " lat " +
                         std::to_string(glm::degrees(carto->latitude)) + " h " + std::to_string(carto->height) + " m";
            }
            Log::Info(Event::Render,
                      "Cesium tiles: " + std::to_string(result.tilesToRenderThisFrame.size()) + " to render, " +
                          std::to_string(result.workerThreadTileLoadQueueLength + result.mainThreadTileLoadQueueLength) +
                          " queued, " + std::to_string(result.tilesVisited) + " visited, " +
                          std::to_string(result.tilesCulled) + " culled, root " +
                          (tileset->getRootTile() ? "ready" : "missing") + "; " + camera);
            for (const auto& tile : result.tilesToRenderThisFrame) {
                const auto* content = tile->getContent().getRenderContent();
                const auto* gpu = content ? static_cast<const GpuTile*>(content->getRenderResources()) : nullptr;
                if (!gpu) {
                    Log::Info(Event::Render, "  tile without GPU resources");
                    continue;
                }
                const double lon = (gpu->originX - 0.5) * 360.0;
                const double lat = glm::degrees(2.0 * std::atan(std::exp((0.5 - gpu->originY) * glm::two_pi<double>())) -
                                                glm::half_pi<double>());
                Log::Info(Event::Render,
                          "  tile at lon " + std::to_string(lon) + " lat " + std::to_string(lat) + ", " +
                              std::to_string(gpu->vertexCount) + " vertices, z " + std::to_string(gpu->minZ) + ".." +
                              std::to_string(gpu->maxZ) + " m, " + std::to_string(gpu->primitives.size()) +
                              " primitives, " + std::to_string(gpu->textures.size()) + " textures");
            }
        }

        if (flags.noDraw) return;

        const double worldSize = tileSize * std::exp2(p.zoom);
        const auto& proj = p.projectionMatrix; // column-major

        glUseProgram(program);
        if (flags.noDepth) {
            glDisable(GL_DEPTH_TEST);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
        }
        // Fading in or out it blends over what is under it, premultiplied like the map.
        if (opacity < 1.0) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(uTexture, 0);
        glUniform1f(uOpacity, static_cast<float>(opacity));
        glUniform1f(uScale, static_cast<float>(worldSize / offsetScale));
        // On terrain, heights are drawn like the terrain's own: metres times its exaggeration.
        // Off terrain the plane is at sea level, so the tileset comes down to its base.
        glUniform1f(uZScale, p.terrainEnabled ? static_cast<float>(p.terrainExaggeration) : 1.0f);
        glUniform1f(uZOffset, p.terrainEnabled ? 0.0f : static_cast<float>(-flatBase));

        const bool solid = flags.solid;
        constexpr std::array<float, 4> solidColor{1.0f, 0.0f, 1.0f, 1.0f};
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
                glUniform4fv(uColor, 1, solid ? solidColor.data() : prim.color.data());
                glUniform1i(uTextured, prim.texture && !solid ? 1 : 0);
                if (prim.texture) glBindTexture(GL_TEXTURE_2D, prim.texture);
                glBindVertexArray(prim.vao);
                glDrawElements(GL_TRIANGLES, prim.indexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
        if (overlayProgram && !p.modelOverlays.empty() && !flags.solid && !flags.noOverlay) {
            drawOverlays(p, result, worldSize, opacity);
        }

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    // The draped fills and lines on the mesh: each tile drawn again for every overlay tile it
    // overlaps, at the depth just written, blended over it.
    void drawOverlays(const style::CustomLayerRenderParameters& p,
                      const Cesium3DTilesSelection::ViewUpdateResult& result,
                      double worldSize,
                      double opacity) {
        const auto& proj = p.projectionMatrix;
        const double metresPerPixel = glm::two_pi<double>() * earthRadius * std::cos(glm::radians(p.latitude)) /
                                      worldSize;
        glUseProgram(overlayProgram);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(oOverlay, 0);
        glUniform1f(oScale, static_cast<float>(worldSize / offsetScale));
        glUniform1f(oZScale, p.terrainEnabled ? static_cast<float>(p.terrainExaggeration) : 1.0f);
        glUniform1f(oZOffset, p.terrainEnabled ? 0.0f : static_cast<float>(-flatBase));
        glUniform1f(oPixelsPerMetre, static_cast<float>(1.0 / metresPerPixel));
        glUniform1f(oOpacity, static_cast<float>(opacity));

        for (const auto& overlay : p.modelOverlays) {
            bool bound = false;
            for (const auto& tile : result.tilesToRenderThisFrame) {
                const auto* content = tile->getContent().getRenderContent();
                const auto* gpu = content ? static_cast<const GpuTile*>(content->getRenderResources()) : nullptr;
                if (!gpu) continue;
                const auto& b = gpu->bounds;
                if (b[2] < overlay.x0 || b[0] > overlay.x0 + overlay.size || b[3] < overlay.y0 ||
                    b[1] > overlay.y0 + overlay.size) {
                    continue;
                }
                if (!bound) {
                    glBindTexture(GL_TEXTURE_2D, overlay.texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    bound = true;
                }
                const double tx = gpu->originX * worldSize;
                const double ty = gpu->originY * worldSize;
                std::array<float, 16> matrix;
                for (size_t i = 0; i < 16; ++i) matrix[i] = static_cast<float>(proj[i]);
                for (size_t i = 0; i < 4; ++i) {
                    matrix[12 + i] = static_cast<float>(proj[i] * tx + proj[4 + i] * ty + proj[12 + i]);
                }
                glUniformMatrix4fv(oMatrix, 1, GL_FALSE, matrix.data());
                // Tile-relative overlay position, from the tile origin in double so the
                // shader only adds small offsets.
                glUniform2f(oOverlayOffset,
                            static_cast<float>((gpu->originX - overlay.x0) / overlay.size),
                            static_cast<float>((gpu->originY - overlay.y0) / overlay.size));
                glUniform1f(oOverlayScale, static_cast<float>(1.0 / (offsetScale * overlay.size)));
                for (const auto& prim : gpu->primitives) {
                    glBindVertexArray(prim.vao);
                    glDrawElements(GL_TRIANGLES, prim.indexCount, GL_UNSIGNED_INT, nullptr);
                }
            }
        }
    }

    void destroy(bool contextAlive) {
        loading = false;
        if (resources) resources->contextAlive = contextAlive;
        tileset.reset();
        asyncSystem.reset();
        resources.reset();
        if (program && contextAlive) glDeleteProgram(program);
        if (overlayProgram && contextAlive) glDeleteProgram(overlayProgram);
        program = 0;
        overlayProgram = 0;
        overlayBounds.clear();
    }
};

namespace {
// The hosts alive right now, so a handle held outside (the Android layer's pointer) can be
// checked before use: the style owns the host and may have deleted it.
std::mutex liveHostsMutex;
std::set<const CesiumTilesHost*> liveHosts;
} // namespace

CesiumTilesHost::CesiumTilesHost(std::string tilesetUrl,
                                 double maximumScreenSpaceError,
                                 double heightOffsetMeters,
                                 double minimumZoom,
                                 double minimumPitchDegrees)
    : impl(std::make_unique<Impl>(
          std::move(tilesetUrl), maximumScreenSpaceError, heightOffsetMeters, minimumZoom, minimumPitchDegrees)) {
    std::lock_guard<std::mutex> lock(liveHostsMutex);
    liveHosts.insert(this);
}

CesiumTilesHost::~CesiumTilesHost() {
    std::lock_guard<std::mutex> lock(liveHostsMutex);
    liveHosts.erase(this);
}

bool CesiumTilesHost::withLiveHost(const CesiumTilesHost* host, const std::function<void(CesiumTilesHost&)>& action) {
    std::lock_guard<std::mutex> lock(liveHostsMutex);
    if (liveHosts.count(host) == 0) return false;
    action(*const_cast<CesiumTilesHost*>(host));
    return true;
}

bool CesiumTilesHost::isLoading() const {
    return impl->loading;
}

float CesiumTilesHost::loadProgress() const {
    return impl->progress;
}

std::vector<std::array<double, 4>> CesiumTilesHost::modelOverlayBounds() const {
    return impl->overlayBounds;
}

void CesiumTilesHost::setEnabled(bool enabled) {
    impl->enabled = enabled;
}

void CesiumTilesHost::trimMemory() {
    impl->trimRequested = true;
}

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

} // namespace cesium
} // namespace mln
