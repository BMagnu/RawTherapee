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
#pragma once

#include <memory>

#include <cairomm/surface.h>

namespace Gtk
{
class Widget;
}

// Shows HDR image content on top of a widget, at its actual luminance, on displays and compositors that
// can. GTK 3 can only draw 8 bit sRGB-ish content, so this uses a separate surface the compositor is told
// holds PQ encoded BT.2020 content, with the paper white of the output as reference white.
//
// Currently implemented for Wayland, through a subsurface described with wp_color_management_v1.
// TODO: Windows (DXGI scRGB / HDR10 swap chain) and macOS (EDR through CAMetalLayer).
class HdrDisplay final
{
public:
    // Whether HDR content can be shown at all: Wayland, and a compositor supporting parametric PQ / BT.2020
    // image descriptions. Cheap after the first call. Must be called from the GUI thread.
    static bool available();

    explicit HdrDisplay(Gtk::Widget& anchor);
    ~HdrDisplay();

    HdrDisplay(const HdrDisplay&) = delete;
    HdrDisplay& operator=(const HdrDisplay&) = delete;

    // Shows `width` x `height` device pixels of HDR content at (`x`, `y`), in logical coordinates of the
    // anchor widget. `hdr` is linear BT.2020 light relative to paper white, 3 floats per pixel, with rows
    // `hdrStride` floats apart. `overlay`, when given, is an ARGB32 rendering of the whole anchor widget
    // (in device pixels) composited over the HDR content, with its white at paper white, so everything
    // GTK draws on top of the image stays visible.
    void present(
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
    );

    // Stops showing anything, leaving the GTK rendering underneath visible.
    void hide();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};
