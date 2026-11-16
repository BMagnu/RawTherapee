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

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <lcms2.h>

#include "iccstore.h"

// Shared helpers for HDR output (PQ / HLG), used by export, the gain map writer and, in the future,
// by the clipping indicators and the HDR preview. All "linear" values here are display light
// relative to diffuse (paper) white, i.e. 1.0 is paper white, and values above 1 are HDR highlights.

namespace rtengine
{

class Imagefloat;
class LabImage;

namespace procparams
{
struct ColorManagementParams;
}

namespace hdr
{

using Matrix3 = std::array<std::array<double, 3>, 3>;

// The transfer function of the output profile, if it is an HDR one, nothing otherwise.
std::optional<TransferFunction> outputTransferFunction(const procparams::ColorManagementParams& icm);
bool isHdrOutput(const procparams::ColorManagementParams& icm);

// Brightest light level, in cd/m2, that the given transfer function can represent with the given settings.
// PQ is limited by the chosen maximum nits, HLG by its nominal reference display peak.
double peakNits(TransferFunction transfer, double maxNits);

// Conversions between an encoded signal [0 ; 1] and linear light relative to paper white.
// `luma` is the Y row of the RGB to XYZ matrix of the primaries, which the HLG OOTF needs.
void decode(TransferFunction transfer, double paperwhiteNits, const double luma[3], double rgb[3]);
void encode(TransferFunction transfer, double paperwhiteNits, double maxNits, const double luma[3], double rgb[3]);

// Encodes, in place, an image holding linear light relative to paper white (as lcms produces it
// with a linear version of an HLG profile, i.e. [0 ; 1] float scale) into HLG, applying the
// BT.2100 inverse OOTF on luminance. Values are left on the same [0 ; 1] scale.
void applyHlgEncoding(Imagefloat& image, double paperwhiteNits, const double luma[3], bool multiThread);

// RGB to XYZ (D50 adapted, as stored in the profile) of a matrix-shaper RGB profile.
bool profileMatrix(cmsHPROFILE profile, Matrix3& rgbToXyz);

// The Y row of a matrix-shaper RGB profile's RGB to XYZ matrix, falling back to BT.2020.
std::array<double, 3> profileLuma(cmsHPROFILE profile);

// Matrix converting linear RGB in the `from` profile's primaries to linear RGB in the `to` profile's
// primaries. Both must be matrix-shaper RGB profiles.
bool conversionMatrix(cmsHPROFILE from, cmsHPROFILE to, Matrix3& fromToTo);

// The single definition of the SDR fallback rendering: linear light relative to paper white in
// the HDR primaries, to linear light in the SDR base primaries.
// Currently a hard clip to [0 ; 1] per channel.
// TODO: make this configurable (e.g. a highlight roll-off / gamut compression). Should it become
// configurable, the SDR view of the (planned) HDR preview must follow the same setting.
inline void sdrFromHdrLinear(const Matrix3& hdrToBase, const float in[3], float out[3])
{
    for (int c = 0; c < 3; ++c) {
        const double v = hdrToBase[c][0] * in[0] + hdrToBase[c][1] * in[1] + hdrToBase[c][2] * in[2];
        out[c] = v < 0.0 ? 0.f : (v > 1.0 ? 1.f : static_cast<float>(v));
    }
}

// Flags classifying each pixel of HDR output relative to SDR, as shown by the clipping indicators.
enum HdrClipFlag : uint8_t {
    HDR_CLIP_ABOVE_PEAK = 1,       // brighter than the HDR output can encode (max nits, HLG peak): clipped in HDR too
    HDR_CLIP_ABOVE_SDR = 2,        // luminance above paper white: can't be shown in SDR, even desaturated
    HDR_CLIP_OUTSIDE_SDR_GAMUT = 4 // within SDR luminance, but outside the SDR base gamut
};

// Classifies the pixels of a region of a Lab image (as the pipeline produces it, L = 32768 for paper
// white) for HDR output with the given colour management settings, one HdrClipFlag set per pixel,
// row by row. Leaves `flags` empty if the output isn't HDR.
void computeClipFlags(const LabImage& lab, int cx, int cy, int cw, int ch, const procparams::ColorManagementParams& icm, std::vector<uint8_t>& flags, bool multiThread);

// H.273 / CICP codes describing a profile, 2 (unspecified) when not matched exactly.
// Primaries are matched on their chromaticities, the transfer curve by sampling.
int cicpPrimaries(cmsHPROFILE profile);
int cicpTransferCharacteristics(cmsHPROFILE profile);

// Whether a profile is usable as the SDR base of a gain map: an RGB matrix-shaper profile
// with CICP matchable primaries and an SDR transfer curve.
bool isUsableSdrBaseProfile(cmsHPROFILE profile);

}

}
