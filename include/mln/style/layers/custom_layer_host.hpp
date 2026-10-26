#pragma once

#include <mln/style/layers/custom_layer_init_parameters.hpp>
#include <mln/style/layers/custom_layer_render_parameters.hpp>

namespace mln {

namespace gfx {
class Context;
} // namespace gfx

namespace style {

class CustomLayerHost {
public:
    virtual ~CustomLayerHost() = default;
    /**
     * Initialize any GL/Metal/Vulkan state needed by the custom layer. This method
     * is called once, from the main thread, at a point when the graphics context
     * is active but before rendering for the first time.
     *
     * The parameters argument provides backend-specific device handles:
     *   - Vulkan: cast to mln::style::vulkan::CustomLayerInitParameters for
     *     access to vk::Device, vk::PhysicalDevice, and the dispatcher.
     *   - Metal/GL: base CustomLayerInitParameters (no extra handles currently).
     *
     * Resources that are acquired in this method must be released in the
     * `deinitialize` function.
     */
    virtual void initialize(const CustomLayerInitParameters&) = 0;

    /**
     * Whether the layer draws true 3D geometry. With terrain on, a custom layer is
     * normally rendered into the terrain's drape texture, like every other 2D layer,
     * so it follows the ground. A 3D layer (meshes, models) must instead draw in the
     * main pass, against the terrain's depth, so it is never draped. Without terrain
     * both answers draw the same way.
     */
    virtual bool rendersIn3D() const { return false; }

    /// Where a 3D host's models are, as web mercator rectangles {x0, y0, x1, y1} in world
    /// units (0..1). With terrain on, the map renders its draped fills and lines for that
    /// area into overlay textures and passes them in CustomLayerRenderParameters, so the
    /// host can paint them onto its models. Empty (the default) asks for none.
    virtual std::vector<std::array<double, 4>> modelOverlayBounds() const { return {}; }

    /**
     * Called right before the layers start rendering.
     */
    virtual void preRender(const mln::gfx::Context&, const mln::style::CustomLayerRenderParameters&) {};

    /**
     * Render the layer. This method is called once per frame. The
     * implementation should not make any assumptions about the GL/Metal state (other
     * than that the correct context is active). It may make changes to the
     * state, and is not required to reset values such as the depth mask,
     * stencil mask, and corresponding test flags to their original values. Make
     * sure that you are drawing your fragments with a z value of 1 to take
     * advantage of the opaque fragment culling in case there are opaque layers
     * above your custom layer.
     */
    virtual void render(const mln::style::CustomLayerRenderParameters&) = 0;

    /**
     * Called when the system has destroyed the underlying GL/Metal context. The
     * `deinitialize` function will not be called in this case, however
     * `initialize` will be called instead to prepare for a new render.
     *
     */
    virtual void contextLost() = 0;

    /**
     * Destroy any GL/Metal state needed by the custom layer, and deallocate context,
     * if necessary. This method is called once, from the main thread, at a
     * point when the GL/Metal context is active.
     *
     * Note that it may be called even when the `initialize` function has not
     * been called.
     */
    virtual void deinitialize() = 0;
};

} // namespace style
} // namespace mln
