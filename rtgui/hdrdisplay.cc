/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "hdrdisplay.h"

#include <gtkmm/container.h>
#include <gtkmm/widget.h>

#ifdef RT_WAYLAND_HDR

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gdk/gdkwayland.h>
#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"

namespace
{

// The Wayland objects shared by all HDR surfaces. Bound on GTK's wl_display, but with an event queue
// of our own, so their events are never dispatched by, nor interfere with, GTK.
struct WaylandGlobals {
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wp_color_manager_v1* colorManager = nullptr;

    bool shmXbgr2101010 = false;
    bool parametric = false;
    bool setLuminances = false;
    bool mastering = false;
    bool perceptual = false;
    bool pq = false;
    bool bt2020 = false;

    bool usable() const
    {
        return compositor && subcompositor && shm && colorManager && shmXbgr2101010 && parametric && setLuminances && perceptual && pq && bt2020;
    }
};

void onShmFormat(void* data, wl_shm*, uint32_t format)
{
    if (format == WL_SHM_FORMAT_XBGR2101010) {
        static_cast<WaylandGlobals*>(data)->shmXbgr2101010 = true;
    }
}

const wl_shm_listener shmListener = {onShmFormat};

void onSupportedIntent(void* data, wp_color_manager_v1*, uint32_t intent)
{
    if (intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
        static_cast<WaylandGlobals*>(data)->perceptual = true;
    }
}

void onSupportedFeature(void* data, wp_color_manager_v1*, uint32_t feature)
{
    WaylandGlobals* const globals = static_cast<WaylandGlobals*>(data);

    switch (feature) {
        case WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC:
            globals->parametric = true;
            break;

        case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES:
            globals->setLuminances = true;
            break;

        case WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES:
            globals->mastering = true;
            break;

        default:
            break;
    }
}

void onSupportedTf(void* data, wp_color_manager_v1*, uint32_t tf)
{
    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) {
        static_cast<WaylandGlobals*>(data)->pq = true;
    }
}

void onSupportedPrimaries(void* data, wp_color_manager_v1*, uint32_t primaries)
{
    if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) {
        static_cast<WaylandGlobals*>(data)->bt2020 = true;
    }
}

void onColorManagerDone(void*, wp_color_manager_v1*)
{
}

const wp_color_manager_v1_listener colorManagerListener = {
    onSupportedIntent,
    onSupportedFeature,
    onSupportedTf,
    onSupportedPrimaries,
    onColorManagerDone
};

void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    WaylandGlobals* const globals = static_cast<WaylandGlobals*>(data);

    if (!std::strcmp(interface, wl_compositor_interface.name)) {
        globals->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min<uint32_t>(version, 4)));
    } else if (!std::strcmp(interface, wl_subcompositor_interface.name)) {
        globals->subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (!std::strcmp(interface, wl_shm_interface.name)) {
        globals->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(globals->shm, &shmListener, globals);
    } else if (!std::strcmp(interface, wp_color_manager_v1_interface.name)) {
        globals->colorManager = static_cast<wp_color_manager_v1*>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min<uint32_t>(version, 2)));
        wp_color_manager_v1_add_listener(globals->colorManager, &colorManagerListener, globals);
    }
}

void onGlobalRemove(void*, wl_registry*, uint32_t)
{
}

const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};

WaylandGlobals* createGlobals()
{
    GdkDisplay* const gdkDisplay = gdk_display_get_default();

    if (!gdkDisplay || !GDK_IS_WAYLAND_DISPLAY(gdkDisplay)) {
        return nullptr;
    }

    WaylandGlobals* const globals = new WaylandGlobals;
    globals->display = gdk_wayland_display_get_wl_display(gdkDisplay);
    globals->queue = wl_display_create_queue(globals->display);

    // A wrapper of the display on our queue, so the registry, and everything bound from it, uses that queue
    wl_display* const wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(globals->display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), globals->queue);
    globals->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(globals->registry, &registryListener, globals);

    // Once for the globals, once for the events sent right after binding them
    wl_display_roundtrip_queue(globals->display, globals->queue);
    wl_display_roundtrip_queue(globals->display, globals->queue);

    return globals;
}

WaylandGlobals* getGlobals()
{
    // Lives as long as the display connection, i.e. the application
    static WaylandGlobals* const globals = createGlobals();
    return globals;
}

// SMPTE ST 2084 inverse EOTF, for luminance relative to 10000 cd/m2
inline float pqEncode(float y)
{
    constexpr float m1 = 2610.f / 16384.f;
    constexpr float m2 = 2523.f / 32.f;
    constexpr float c1 = 3424.f / 4096.f;
    constexpr float c2 = 2413.f / 128.f;
    constexpr float c3 = 2392.f / 128.f;
    const float p = std::pow(std::min(std::max(y, 0.f), 1.f), m1);
    return std::pow((c1 + c2 * p) / (1.f + c3 * p), m2);
}

struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    bool busy = false;

    ~ShmBuffer()
    {
        if (buffer) {
            wl_buffer_destroy(buffer);
        }

        if (pixels) {
            munmap(pixels, size);
        }
    }
};

void onBufferRelease(void* data, wl_buffer*)
{
    static_cast<ShmBuffer*>(data)->busy = false;
}

const wl_buffer_listener bufferListener = {onBufferRelease};

std::unique_ptr<ShmBuffer> createBuffer(WaylandGlobals* globals, int width, int height)
{
    auto buffer = std::make_unique<ShmBuffer>();
    buffer->width = width;
    buffer->height = height;
    buffer->size = static_cast<size_t>(width) * height * 4;

    const int fd = memfd_create("rawtherapee-hdr", MFD_CLOEXEC);

    if (fd < 0) {
        return nullptr;
    }

    if (ftruncate(fd, buffer->size) < 0) {
        close(fd);
        return nullptr;
    }

    void* const pixels = mmap(nullptr, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (pixels == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    buffer->pixels = static_cast<uint32_t*>(pixels);
    wl_shm_pool* const pool = wl_shm_create_pool(globals->shm, fd, buffer->size);
    buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XBGR2101010);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer->buffer, &bufferListener, buffer.get());
    return buffer;
}

enum class DescriptionState {
    PENDING,
    READY,
    FAILED
};

void onDescriptionFailed(void* data, wp_image_description_v1*, uint32_t, const char* msg)
{
    std::cerr << "HDR preview: the compositor rejected the image description: " << (msg ? msg : "") << std::endl;
    *static_cast<DescriptionState*>(data) = DescriptionState::FAILED;
}

void onDescriptionReady(void* data, wp_image_description_v1*, uint32_t)
{
    *static_cast<DescriptionState*>(data) = DescriptionState::READY;
}

void onDescriptionReady2(void* data, wp_image_description_v1*, uint32_t, uint32_t)
{
    *static_cast<DescriptionState*>(data) = DescriptionState::READY;
}

const wp_image_description_v1_listener descriptionListener = {
    onDescriptionFailed,
    onDescriptionReady,
    onDescriptionReady2
};

}

struct HdrDisplay::Implementation {
    Gtk::Widget& anchor;
    WaylandGlobals* const globals;

    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wl_surface* parent = nullptr;
    wp_color_management_surface_v1* colorSurface = nullptr;
    wp_image_description_v1* description = nullptr;
    DescriptionState descriptionState = DescriptionState::PENDING;
    bool descriptionApplied = false;
    double descriptionPaperwhite = 0.0;
    double descriptionMaxNits = 0.0;
    std::vector<std::unique_ptr<ShmBuffer>> buffers;
    bool mapped = false;

    // sRGB to linear, for the 8 bit overlay
    std::array<float, 256> srgbToLinear;

    explicit Implementation(Gtk::Widget& widget) :
        anchor(widget),
        globals(getGlobals())
    {
        for (int i = 0; i < 256; ++i) {
            const double v = i / 255.0;
            srgbToLinear[i] = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        }
    }

    ~Implementation()
    {
        destroySurface();
    }

    void destroySurface()
    {
        buffers.clear();

        if (description) {
            wp_image_description_v1_destroy(description);
            description = nullptr;
        }

        descriptionApplied = false;

        if (colorSurface) {
            wp_color_management_surface_v1_destroy(colorSurface);
            colorSurface = nullptr;
        }

        if (subsurface) {
            wl_subsurface_destroy(subsurface);
            subsurface = nullptr;
        }

        if (surface) {
            wl_surface_destroy(surface);
            surface = nullptr;
        }

        parent = nullptr;
        mapped = false;

        if (globals) {
            wl_display_flush(globals->display);
        }
    }

    wl_surface* toplevelSurface() const
    {
        Gtk::Widget* const toplevel = anchor.get_toplevel();

        if (!toplevel || !toplevel->get_realized()) {
            return nullptr;
        }

        GdkWindow* const window = toplevel->get_window() ? toplevel->get_window()->gobj() : nullptr;
        return window && GDK_IS_WAYLAND_WINDOW(window) ? gdk_wayland_window_get_wl_surface(window) : nullptr;
    }

    bool ensureSurface()
    {
        wl_surface* const top = toplevelSurface();

        if (!top) {
            return false;
        }

        if (surface && parent == top) {
            return true;
        }

        // First use, or the toplevel's surface was recreated
        destroySurface();
        parent = top;
        surface = wl_compositor_create_surface(globals->compositor);
        subsurface = wl_subcompositor_get_subsurface(globals->subcompositor, surface, parent);
        // Synchronized with the toplevel: our updates show together with the GTK frame they belong to

        // Pointer and touch input must keep reaching GTK underneath
        wl_region* const empty = wl_compositor_create_region(globals->compositor);
        wl_surface_set_input_region(surface, empty);
        wl_region_destroy(empty);

        colorSurface = wp_color_manager_v1_get_surface(globals->colorManager, surface);
        return true;
    }

    bool ensureDescription(double paperwhite, double maxNits)
    {
        if (description && descriptionPaperwhite == paperwhite && descriptionMaxNits == maxNits) {
            // It may have become ready only after we stopped waiting for it
            return applyDescription();
        }

        if (description) {
            wp_image_description_v1_destroy(description);
        }

        wp_image_description_creator_params_v1* const creator = wp_color_manager_v1_create_parametric_creator(globals->colorManager);
        wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
        wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);

        // PQ black is 0, but the protocol needs a minimum above it. Paper white is the reference white,
        // which the compositor anchors to the display's SDR white.
        constexpr uint32_t minLum = 5; // 0.0005 cd/m2
        const uint32_t reference = std::max<uint32_t>(std::lround(paperwhite), 1);
        wp_image_description_creator_params_v1_set_luminances(creator, minLum, 10000, reference);

        if (globals->mastering) {
            // The content never exceeds the output's maximum luminance, which lets the compositor
            // tone map only as much as the display needs.
            wp_image_description_creator_params_v1_set_mastering_display_primaries(creator, 708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000);
            wp_image_description_creator_params_v1_set_mastering_luminance(creator, minLum, std::max<uint32_t>(std::lround(maxNits), reference + 1));
        }

        description = wp_image_description_creator_params_v1_create(creator);
        descriptionState = DescriptionState::PENDING;
        descriptionApplied = false;
        descriptionPaperwhite = paperwhite;
        descriptionMaxNits = maxNits;
        wp_image_description_v1_add_listener(description, &descriptionListener, &descriptionState);

        // The compositor answers right away in practice, but don't wait forever
        for (int i = 0; i < 5 && descriptionState == DescriptionState::PENDING; ++i) {
            if (wl_display_roundtrip_queue(globals->display, globals->queue) < 0) {
                break;
            }
        }

        return applyDescription();
    }

    bool applyDescription()
    {
        if (descriptionState != DescriptionState::READY) {
            return false;
        }

        if (!descriptionApplied) {
            wp_color_management_surface_v1_set_image_description(colorSurface, description, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
            descriptionApplied = true;
        }

        return true;
    }

    ShmBuffer* freeBuffer(int width, int height)
    {
        // Drop buffers of another size that the compositor is done with
        buffers.erase(
            std::remove_if(
                buffers.begin(),
                buffers.end(),
                [width, height](const std::unique_ptr<ShmBuffer>& buffer)
                {
                    return !buffer->busy && (buffer->width != width || buffer->height != height);
                }
            ),
            buffers.end()
        );

        for (const auto& buffer : buffers) {
            if (!buffer->busy) {
                return buffer.get();
            }
        }

        if (buffers.size() >= 4) {
            // The compositor holds on to all of them; skip this frame rather than pile up memory
            return nullptr;
        }

        std::unique_ptr<ShmBuffer> buffer = createBuffer(globals, width, height);

        if (!buffer) {
            return nullptr;
        }

        buffers.push_back(std::move(buffer));
        return buffers.back().get();
    }

    // Offset of the anchor widget in the toplevel's surface coordinates
    bool anchorOffset(int& x, int& y) const
    {
        Glib::RefPtr<Gdk::Window> window = anchor.get_window();

        if (!window) {
            return false;
        }

        int wx = 0, wy = 0, tx = 0, ty = 0;
        window->get_origin(wx, wy);
        window->get_toplevel()->get_origin(tx, ty);
        x = wx - tx;
        y = wy - ty;

        if (!anchor.get_has_window()) {
            const Gtk::Allocation allocation = anchor.get_allocation();
            x += allocation.get_x();
            y += allocation.get_y();
        }

        return true;
    }
};

bool HdrDisplay::available()
{
    const WaylandGlobals* const globals = getGlobals();
    return globals && globals->usable();
}

HdrDisplay::HdrDisplay(Gtk::Widget& anchor) :
    implementation(new Implementation(anchor))
{
}

HdrDisplay::~HdrDisplay() = default;

void HdrDisplay::present(
    int x,
    int y,
    int width,
    int height,
    int deviceScale,
    const float* hdr,
    int hdrStride,
    const Cairo::RefPtr<Cairo::ImageSurface>& overlay,
    double paperwhiteNits,
    double maxNits
)
{
    Implementation& impl = *implementation;

    if (!impl.globals || !impl.globals->usable() || width <= 0 || height <= 0 || !hdr) {
        hide();
        return;
    }

    deviceScale = std::max(deviceScale, 1);

    // The buffer size must be a multiple of the buffer scale, or the compositor kills the connection
    // (wl_surface error invalid_size). Drop the odd device pixel at the edge rather than that.
    width -= width % deviceScale;
    height -= height % deviceScale;

    if (width <= 0 || height <= 0) {
        hide();
        return;
    }

    wl_display_dispatch_queue_pending(impl.globals->display, impl.globals->queue);

    int offsetX = 0, offsetY = 0;

    if (!impl.ensureSurface() || !impl.anchorOffset(offsetX, offsetY) || !impl.ensureDescription(paperwhiteNits, maxNits)) {
        hide();
        return;
    }

    ShmBuffer* const buffer = impl.freeBuffer(width, height);

    if (!buffer) {
        return;
    }

    // The overlay covers the whole widget; locate the image within it
    const int overlayX = x * deviceScale;
    const int overlayY = y * deviceScale;
    const unsigned char* overlayData = nullptr;
    int overlayStride = 0;
    int overlayWidth = 0;
    int overlayHeight = 0;

    if (overlay) {
        overlay->flush();
        overlayData = overlay->get_data();
        overlayStride = overlay->get_stride();
        overlayWidth = overlay->get_width();
        overlayHeight = overlay->get_height();
    }

    const float paperwhite = paperwhiteNits / 10000.0;
    const std::array<float, 256>& srgbToLinear = impl.srgbToLinear;
    // BT.709 to BT.2020, linear
    constexpr float m709to2020[3][3] = {{0.6274f, 0.3293f, 0.0433f}, {0.0691f, 0.9195f, 0.0114f}, {0.0164f, 0.0880f, 0.8956f}};

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif

    for (int row = 0; row < height; ++row) {
        const float* const in = hdr + static_cast<size_t>(row) * hdrStride;
        uint32_t* const out = buffer->pixels + static_cast<size_t>(row) * width;
        const int oy = overlayY + row;
        const uint32_t* const overlayRow =
            overlayData && oy >= 0 && oy < overlayHeight
            ? reinterpret_cast<const uint32_t*>(overlayData + static_cast<size_t>(oy) * overlayStride)
            : nullptr;

        for (int col = 0; col < width; ++col) {
            float rgb[3] = {in[3 * col], in[3 * col + 1], in[3 * col + 2]};
            const int ox = overlayX + col;

            if (overlayRow && ox >= 0 && ox < overlayWidth) {
                // Cairo's ARGB32: premultiplied, native endian 0xAARRGGBB
                const uint32_t pixel = overlayRow[ox];
                const unsigned alpha = pixel >> 24;

                if (alpha) {
                    const float a = alpha / 255.f;
                    float sdr[3];

                    for (int c = 0; c < 3; ++c) {
                        const unsigned premultiplied = (pixel >> (16 - 8 * c)) & 0xff;
                        sdr[c] = srgbToLinear[std::min(255u, (premultiplied * 255 + alpha / 2) / alpha)];
                    }

                    for (int c = 0; c < 3; ++c) {
                        const float v = m709to2020[c][0] * sdr[0] + m709to2020[c][1] * sdr[1] + m709to2020[c][2] * sdr[2];
                        rgb[c] = v * a + rgb[c] * (1.f - a);
                    }
                }
            }

            uint32_t encoded[3];

            for (int c = 0; c < 3; ++c) {
                encoded[c] = static_cast<uint32_t>(pqEncode(rgb[c] * paperwhite) * 1023.f + 0.5f);
            }

            // XBGR2101010: red in the lowest bits
            out[col] = encoded[0] | (encoded[1] << 10) | (encoded[2] << 20) | (3u << 30);
        }
    }

    buffer->busy = true;
    wl_surface_attach(impl.surface, buffer->buffer, 0, 0);
    wl_surface_set_buffer_scale(impl.surface, deviceScale);
    wl_surface_damage_buffer(impl.surface, 0, 0, width, height);
    wl_subsurface_set_position(impl.subsurface, offsetX + x, offsetY + y);
    wl_surface_commit(impl.surface);
    wl_display_flush(impl.globals->display);
    impl.mapped = true;
}

void HdrDisplay::hide()
{
    Implementation& impl = *implementation;

    if (!impl.mapped || !impl.surface) {
        return;
    }

    wl_surface_attach(impl.surface, nullptr, 0, 0);
    wl_surface_commit(impl.surface);
    wl_display_flush(impl.globals->display);
    impl.mapped = false;
}

void HdrDisplay::release()
{
    // Destroying the subsurface takes it off screen right away, without waiting for a parent commit
    implementation->destroySurface();
}

#else // RT_WAYLAND_HDR

struct HdrDisplay::Implementation {
};

bool HdrDisplay::available()
{
    return false;
}

HdrDisplay::HdrDisplay(Gtk::Widget&)
{
}

HdrDisplay::~HdrDisplay() = default;

void HdrDisplay::present(int, int, int, int, int, const float*, int, const Cairo::RefPtr<Cairo::ImageSurface>&, double, double)
{
}

void HdrDisplay::hide()
{
}

void HdrDisplay::release()
{
}

#endif // RT_WAYLAND_HDR
