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
#include <cmath>

#include <avif/avif.h>

#include "hdr.h"

#include "color.h"
#include "imagefloat.h"
#include "labimage.h"
#include "procparams.h"
#include "rt_math.h"

namespace rtengine
{

namespace hdr
{

std::optional<TransferFunction> outputTransferFunction(const procparams::ColorManagementParams& icm)
{
    if (icm.outputProfile.empty() || icm.outputProfile == procparams::ColorManagementParams::NoICMString) {
        return {};
    }

    return unexpressibleTransferFunction(ICCStore::getInstance()->getProfile(icm.outputProfile));
}

bool isHdrOutput(const procparams::ColorManagementParams& icm)
{
    return static_cast<bool>(outputTransferFunction(icm));
}

double peakNits(TransferFunction transfer, double maxNits)
{
    switch (transfer) {
        case TransferFunction::PQ:
            return LIM(maxNits, 1.0, 10000.0);

        case TransferFunction::HLG:
            return Color::hlg_nominal_peak;
    }

    return 10000.0;
}

void decode(TransferFunction transfer, double paperwhiteNits, const double luma[3], double rgb[3])
{
    paperwhiteNits = LIM(paperwhiteNits, 1.0, 10000.0);

    switch (transfer) {
        case TransferFunction::PQ:
            for (int c = 0; c < 3; ++c) {
                rgb[c] = Color::pq_eotf(LIM(rgb[c], 0.0, 1.0)) * 10000.0 / paperwhiteNits;
            }

            break;

        case TransferFunction::HLG: {
            // Inverse OETF to scene light, then the BT.2100 OOTF: Fd = Lw * Ys^(gamma - 1) * E
            double scene[3];

            for (int c = 0; c < 3; ++c) {
                scene[c] = Color::hlg_inv_oetf(LIM(rgb[c], 0.0, 1.0));
            }

            const double ys = luma[0] * scene[0] + luma[1] * scene[1] + luma[2] * scene[2];
            const double gain = ys > 0.0 ? std::pow(ys, Color::hlg_system_gamma - 1.0) : 0.0;

            for (int c = 0; c < 3; ++c) {
                rgb[c] = Color::hlg_nominal_peak * gain * scene[c] / paperwhiteNits;
            }

            break;
        }
    }
}

namespace
{

inline void hlgEncode(double paperwhiteNits, const double luma[3], double rgb[3])
{
    // Display light normalized to the nominal peak, then the inverse OOTF on luminance,
    // E = Fd * Yd^((1 - gamma) / gamma), then the OETF.
    double display[3];

    for (int c = 0; c < 3; ++c) {
        display[c] = rtengine::max(rgb[c], 0.0) * paperwhiteNits / Color::hlg_nominal_peak;
    }

    const double yd = luma[0] * display[0] + luma[1] * display[1] + luma[2] * display[2];
    const double gain = yd > 0.0 ? std::pow(yd, (1.0 - Color::hlg_system_gamma) / Color::hlg_system_gamma) : 0.0;

    for (int c = 0; c < 3; ++c) {
        rgb[c] = Color::hlg_oetf(rtengine::min(display[c] * gain, 1.0));
    }
}

}

void encode(TransferFunction transfer, double paperwhiteNits, double maxNits, const double luma[3], double rgb[3])
{
    paperwhiteNits = LIM(paperwhiteNits, 1.0, 10000.0);

    switch (transfer) {
        case TransferFunction::PQ: {
            const double ceiling = peakNits(transfer, maxNits);

            for (int c = 0; c < 3; ++c) {
                rgb[c] = Color::pq_inv_eotf(LIM(rgb[c] * paperwhiteNits, 0.0, ceiling) / 10000.0);
            }

            break;
        }

        case TransferFunction::HLG:
            hlgEncode(paperwhiteNits, luma, rgb);
            break;
    }
}

void applyHlgEncoding(Imagefloat& image, double paperwhiteNits, const double luma[3], bool multiThread)
{
    paperwhiteNits = LIM(paperwhiteNits, 1.0, 10000.0);
    const int width = image.getWidth();
    const int height = image.getHeight();

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif

    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            double rgb[3] = {image.r(i, j), image.g(i, j), image.b(i, j)};
            hlgEncode(paperwhiteNits, luma, rgb);
            image.r(i, j) = rgb[0];
            image.g(i, j) = rgb[1];
            image.b(i, j) = rgb[2];
        }
    }
}

bool profileMatrix(cmsHPROFILE profile, Matrix3& rgbToXyz)
{
    if (!profile || cmsGetColorSpace(profile) != cmsSigRgbData || !cmsIsMatrixShaper(profile)) {
        return false;
    }

    const cmsCIEXYZ* const colorants[3] = {
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigRedColorantTag)),
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigGreenColorantTag)),
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigBlueColorantTag))
    };

    if (!colorants[0] || !colorants[1] || !colorants[2]) {
        return false;
    }

    for (int c = 0; c < 3; ++c) {
        rgbToXyz[0][c] = colorants[c]->X;
        rgbToXyz[1][c] = colorants[c]->Y;
        rgbToXyz[2][c] = colorants[c]->Z;
    }

    return true;
}

namespace
{
bool iccPrimariesXY(cmsHPROFILE profile, float primaries[8]);
}

std::array<double, 3> profileLuma(cmsHPROFILE profile)
{
    // The luma weights belong to the actual primaries and white point, not to the D50 adapted
    // colorants stored in the profile, so recover the chromaticities first. For BT.2020 this
    // gives exactly the BT.2100 weights (0.2627, 0.6780, 0.0593) the HLG OOTF is defined with.
    float xy[8];

    if (profile && iccPrimariesXY(profile, xy)) {
        Matrix3 primaries;

        for (int c = 0; c < 3; ++c) {
            const double x = xy[2 * c], y = xy[2 * c + 1];
            primaries[0][c] = x / y;
            primaries[1][c] = 1.0;
            primaries[2][c] = (1.0 - x - y) / y;
        }

        const double wx = xy[6], wy = xy[7];
        const std::array<double, 3> white = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
        Matrix3 inverse;

        if (invertMatrix(primaries, inverse)) {
            // Scale each primary so that they sum up to the white point; Y of each is its luma weight
            const std::array<double, 3> scale = dotProduct(inverse, white);
            return {scale[0], scale[1], scale[2]};
        }
    }

    return {0.2627, 0.6780, 0.0593};
}

void computeClipFlags(const LabImage& lab, int cx, int cy, int cw, int ch, const procparams::ColorManagementParams& icm, std::vector<uint8_t>& flags, bool multiThread)
{
    flags.clear();
    const std::optional<TransferFunction> transfer = outputTransferFunction(icm);

    if (!transfer || cw <= 0 || ch <= 0) {
        return;
    }

    const ICCStore* const store = ICCStore::getInstance();
    Matrix3 outToXyz, baseToXyz, xyzToOut, xyzToBase;

    if (
        !profileMatrix(store->getProfile(icm.outputProfile), outToXyz)
        || !invertMatrix(outToXyz, xyzToOut)
    ) {
        return;
    }

    // Without a usable base, judge the SDR gamut by the HDR output's own primaries
    const bool haveBase = profileMatrix(store->getProfile(icm.outputSdrBaseProfile), baseToXyz) && invertMatrix(baseToXyz, xyzToBase);

    if (!haveBase) {
        xyzToBase = xyzToOut;
    }

    const double paperwhite = LIM(icm.outputPaperwhiteNits, 1.0, 10000.0);
    // Relative to paper white, with a little tolerance so values that encode exactly at the
    // ceiling, or exactly at paper white, don't flicker in and out of the indicators
    const double peak = peakNits(*transfer, icm.outputMaxNits) / paperwhite * 0.999;
    // HLG clips each channel in scene light, after the inverse OOTF on luminance, so saturated colours clip
    // well below the nominal peak (a pure red at about 765 cd/m2): ask the encoding itself
    const bool hlg = *transfer == TransferFunction::HLG;
    const std::array<double, 3> outLuma = profileLuma(store->getProfile(icm.outputProfile));
    constexpr double sdrWhite = 1.0 + 1e-4;
    constexpr double gamutTolerance = 1e-4;

    flags.assign(static_cast<size_t>(cw) * ch, 0);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif

    for (int i = 0; i < ch; ++i) {
        const float* const rL = lab.L[cy + i];
        const float* const ra = lab.a[cy + i];
        const float* const rb = lab.b[cy + i];
        uint8_t* const row = flags.data() + static_cast<size_t>(i) * cw;

        for (int j = 0; j < cw; ++j) {
            float fx, fy, fz;
            Color::Lab2XYZ(rL[cx + j], ra[cx + j], rb[cx + j], fx, fy, fz);
            // Y = 1 is paper white
            const double xyz[3] = {fx / 65535.0, fy / 65535.0, fz / 65535.0};
            uint8_t f = 0;

            double out[3];

            for (int c = 0; c < 3; ++c) {
                out[c] = xyzToOut[c][0] * xyz[0] + xyzToOut[c][1] * xyz[1] + xyzToOut[c][2] * xyz[2];
            }

            bool abovePeak;

            if (hlg) {
                double encoded[3] = {out[0], out[1], out[2]};
                encode(*transfer, paperwhite, icm.outputMaxNits, outLuma.data(), encoded);
                abovePeak = rtengine::max(encoded[0], encoded[1], encoded[2]) >= 0.9999;
            } else {
                abovePeak = rtengine::max(out[0], out[1], out[2]) >= peak;
            }

            if (abovePeak) {
                f |= HDR_CLIP_ABOVE_PEAK;
            }

            if (xyz[1] > sdrWhite) {
                f |= HDR_CLIP_ABOVE_SDR;
            } else {
                for (int c = 0; c < 3; ++c) {
                    const double v = xyzToBase[c][0] * xyz[0] + xyzToBase[c][1] * xyz[1] + xyzToBase[c][2] * xyz[2];

                    if (v < -gamutTolerance || v > 1.0 + gamutTolerance) {
                        f |= HDR_CLIP_OUTSIDE_SDR_GAMUT;
                        break;
                    }
                }
            }

            row[j] = f;
        }
    }
}

namespace
{

// Linear BT.2020 (D65), the colour space the HDR preview is handed to the display in
cmsHPROFILE bt2020Profile()
{
    static const struct Bt2020 {
        cmsHPROFILE profile;

        Bt2020()
        {
            const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
            const cmsCIExyYTRIPLE primaries = {{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0}, {0.131, 0.046, 1.0}};
            cmsToneCurve* const linear = cmsBuildGamma(nullptr, 1.0);
            cmsToneCurve* const curves[3] = {linear, linear, linear};
            profile = cmsCreateRGBProfile(&d65, &primaries, curves);
            cmsFreeToneCurve(linear);
        }
    } instance;

    return instance.profile;
}

}

void computePreview(const LabImage& lab, int cx, int cy, int cw, int ch, const procparams::ColorManagementParams& icm, std::vector<float>& out, bool multiThread)
{
    out.clear();
    const std::optional<TransferFunction> transfer = outputTransferFunction(icm);

    if (!transfer || cw <= 0 || ch <= 0) {
        return;
    }

    const cmsHPROFILE outProfile = ICCStore::getInstance()->getProfile(icm.outputProfile);
    Matrix3 outToXyz, xyzToOut, outToBt2020;

    if (!profileMatrix(outProfile, outToXyz) || !invertMatrix(outToXyz, xyzToOut) || !conversionMatrix(outProfile, bt2020Profile(), outToBt2020)) {
        return;
    }

    const std::array<double, 3> luma = profileLuma(outProfile);
    const double paperwhite = icm.outputPaperwhiteNits;
    const double maxNits = icm.outputMaxNits;

    out.resize(static_cast<size_t>(cw) * ch * 3);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif

    for (int i = 0; i < ch; ++i) {
        const float* const rL = lab.L[cy + i];
        const float* const ra = lab.a[cy + i];
        const float* const rb = lab.b[cy + i];
        float* const row = out.data() + static_cast<size_t>(i) * cw * 3;

        for (int j = 0; j < cw; ++j) {
            float fx, fy, fz;
            Color::Lab2XYZ(rL[cx + j], ra[cx + j], rb[cx + j], fx, fy, fz);
            const double xyz[3] = {fx / 65535.0, fy / 65535.0, fz / 65535.0};
            double rgb[3];

            for (int c = 0; c < 3; ++c) {
                rgb[c] = xyzToOut[c][0] * xyz[0] + xyzToOut[c][1] * xyz[1] + xyzToOut[c][2] * xyz[2];
            }

            // Through the output's encoding and back, so the preview clips exactly where the export does
            encode(*transfer, paperwhite, maxNits, luma.data(), rgb);
            decode(*transfer, paperwhite, luma.data(), rgb);

            for (int c = 0; c < 3; ++c) {
                const double v = outToBt2020[c][0] * rgb[0] + outToBt2020[c][1] * rgb[1] + outToBt2020[c][2] * rgb[2];
                row[3 * j + c] = static_cast<float>(rtengine::max(v, 0.0));
            }
        }
    }
}

SdrRendering sdrRendering(TransferFunction transfer, double paperwhiteNits, double maxNits, procparams::ColorManagementParams::SdrFallback mode, cmsHPROFILE baseProfile)
{
    SdrRendering rendering;
    rendering.mode = mode;
    rendering.peak = rtengine::max(peakNits(transfer, maxNits) / LIM(paperwhiteNits, 1.0, 10000.0), 1.0);
    rendering.baseLuma = profileLuma(baseProfile);
    return rendering;
}

namespace
{

// Values up to this fraction of SDR white are left alone by the roll-off, so midtones look the same in SDR
// as in HDR. Above, the shoulder compresses everything up to the output's peak into the rest.
constexpr double ROLL_OFF_KNEE = 0.75;

// Extended Reinhard shoulder above the knee: continuous slope at the knee, and `peak` lands exactly on 1
double shoulder(double v, double peak)
{
    if (v <= ROLL_OFF_KNEE) {
        return v;
    }

    const double range = 1.0 - ROLL_OFF_KNEE;
    const double x = (v - ROLL_OFF_KNEE) / range;
    const double white = rtengine::max((peak - ROLL_OFF_KNEE) / range, 1e-6);
    return ROLL_OFF_KNEE + range * rtengine::min(x * (1.0 + x / (white * white)) / (1.0 + x), 1.0);
}

void rollOff(const SdrRendering& rendering, double rgb[3])
{
    // The shoulder acts on the largest channel, and all channels are scaled alike, which keeps hue and
    // saturation: bright saturated colours stay saturated, at the cost of some of their luminance. (Mapping
    // luminance instead would have to desaturate them to almost white, as saturated primaries can only
    // reach a fraction of SDR white's luminance.)
    const double maxRgb = rtengine::max(rgb[0], rgb[1], rgb[2]);

    if (!(maxRgb > 0.0)) {
        rgb[0] = rgb[1] = rgb[2] = 0.0;
        return;
    }

    const double scale = shoulder(maxRgb, rendering.peak) / maxRgb;

    for (int c = 0; c < 3; ++c) {
        rgb[c] *= scale;
    }

    // Colours outside the SDR gamut have negative channels left. Desaturate those towards grey of the same
    // luminance, rather than clipping per channel, which would shift their hue. Moving towards grey only
    // lowers the largest channel, so nothing goes above 1.
    const std::array<double, 3>& luma = rendering.baseLuma;
    const double y = rtengine::max(luma[0] * rgb[0] + luma[1] * rgb[1] + luma[2] * rgb[2], 0.0);
    double saturation = 1.0;

    for (int c = 0; c < 3; ++c) {
        if (rgb[c] < 0.0) {
            saturation = rtengine::min(saturation, y / (y - rgb[c]));
        }
    }

    for (int c = 0; c < 3; ++c) {
        rgb[c] = LIM(y + saturation * (rgb[c] - y), 0.0, 1.0);
    }
}

}

void sdrFromHdrLinear(const SdrRendering& rendering, const Matrix3& hdrToBase, const float in[3], float out[3])
{
    double rgb[3];

    for (int c = 0; c < 3; ++c) {
        rgb[c] = hdrToBase[c][0] * in[0] + hdrToBase[c][1] * in[1] + hdrToBase[c][2] * in[2];
    }

    switch (rendering.mode) {
        case procparams::ColorManagementParams::SdrFallback::CLIP:
            for (int c = 0; c < 3; ++c) {
                rgb[c] = LIM(rgb[c], 0.0, 1.0);
            }

            break;

        case procparams::ColorManagementParams::SdrFallback::ROLL_OFF:
            rollOff(rendering, rgb);
            break;
    }

    for (int c = 0; c < 3; ++c) {
        out[c] = rgb[c];
    }
}

bool needsSdrRendering(const procparams::ColorManagementParams& icm)
{
    return icm.outputSdrFallback != procparams::ColorManagementParams::SdrFallback::CLIP && isHdrOutput(icm);
}

void renderSdr(LabImage& lab, const procparams::ColorManagementParams& icm, bool multiThread)
{
    const std::optional<TransferFunction> transfer = outputTransferFunction(icm);

    if (!transfer) {
        return;
    }

    const ICCStore* const store = ICCStore::getInstance();
    const cmsHPROFILE outProfile = store->getProfile(icm.outputProfile);
    cmsHPROFILE baseProfile = store->getProfile(icm.outputSdrBaseProfile);
    Matrix3 outToXyz, xyzToOut, baseToXyz, outToBase;

    if (!profileMatrix(baseProfile, baseToXyz)) {
        // Without a usable base, render into the HDR output's own primaries
        baseProfile = outProfile;
    }

    if (
        !profileMatrix(outProfile, outToXyz)
        || !invertMatrix(outToXyz, xyzToOut)
        || !profileMatrix(baseProfile, baseToXyz)
        || !conversionMatrix(outProfile, baseProfile, outToBase)
    ) {
        return;
    }

    const std::array<double, 3> outLuma = profileLuma(outProfile);
    const SdrRendering rendering = sdrRendering(*transfer, icm.outputPaperwhiteNits, icm.outputMaxNits, icm.outputSdrFallback, baseProfile);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif

    for (int i = 0; i < lab.H; ++i) {
        for (int j = 0; j < lab.W; ++j) {
            float fx, fy, fz;
            Color::Lab2XYZ(lab.L[i][j], lab.a[i][j], lab.b[i][j], fx, fy, fz);
            const double xyz[3] = {fx / 65535.0, fy / 65535.0, fz / 65535.0};
            double rgb[3];

            for (int c = 0; c < 3; ++c) {
                rgb[c] = xyzToOut[c][0] * xyz[0] + xyzToOut[c][1] * xyz[1] + xyzToOut[c][2] * xyz[2];
            }

            // Through the output's encoding and back, like the HDR image the export renders the SDR one from
            encode(*transfer, icm.outputPaperwhiteNits, icm.outputMaxNits, outLuma.data(), rgb);
            decode(*transfer, icm.outputPaperwhiteNits, outLuma.data(), rgb);

            const float hdrRgb[3] = {static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2])};
            float sdr[3];
            sdrFromHdrLinear(rendering, outToBase, hdrRgb, sdr);

            double out[3];

            for (int c = 0; c < 3; ++c) {
                out[c] = 65535.0 * (baseToXyz[c][0] * sdr[0] + baseToXyz[c][1] * sdr[1] + baseToXyz[c][2] * sdr[2]);
            }

            Color::XYZ2Lab(out[0], out[1], out[2], lab.L[i][j], lab.a[i][j], lab.b[i][j]);
        }
    }
}

bool conversionMatrix(cmsHPROFILE from, cmsHPROFILE to, Matrix3& fromToTo)
{
    Matrix3 fromToXyz, toToXyz, xyzToTo;

    if (!profileMatrix(from, fromToXyz) || !profileMatrix(to, toToXyz) || !invertMatrix(toToXyz, xyzToTo)) {
        return false;
    }

    fromToTo = dotProduct(xyzToTo, fromToXyz);
    return true;
}

namespace
{

// Chromaticities of the primaries and white point, as avifColorPrimariesFind wants them. The
// colorants are stored adapted to D50, so this undoes the adaptation to get the actual primaries.
bool iccPrimariesXY(cmsHPROFILE profile, float primaries[8])
{
    if (cmsGetColorSpace(profile) != cmsSigRgbData || !cmsIsMatrixShaper(profile)) {
        return false;
    }

    const cmsCIEXYZ* const colorants[3] = {
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigRedMatrixColumnTag)),
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigGreenMatrixColumnTag)),
        static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigBlueMatrixColumnTag))
    };

    if (!colorants[0] || !colorants[1] || !colorants[2]) {
        return false;
    }

    const cmsCIEXYZ d50Xyz = {0.9642, 1.0, 0.8249};
    std::array<cmsCIEXYZ, 4> xyz = {*colorants[0], *colorants[1], *colorants[2], d50Xyz};

    if (const auto* const chad = static_cast<const cmsFloat64Number*>(cmsReadTag(profile, cmsSigChromaticAdaptationTag))) {
        std::array<std::array<double, 3>, 3> forward, inverse;

        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                forward[row][col] = chad[3 * row + col];
            }
        }

        if (!invertMatrix(forward, inverse)) {
            return false;
        }

        for (cmsCIEXYZ& value : xyz) {
            const cmsCIEXYZ in = value;
            value.X = inverse[0][0] * in.X + inverse[0][1] * in.Y + inverse[0][2] * in.Z;
            value.Y = inverse[1][0] * in.X + inverse[1][1] * in.Y + inverse[1][2] * in.Z;
            value.Z = inverse[2][0] * in.X + inverse[2][1] * in.Y + inverse[2][2] * in.Z;
        }
    } else {
        const auto* const whitePoint = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, cmsSigMediaWhitePointTag));

        if (!whitePoint) {
            return false;
        }

        xyz[3] = *whitePoint;

        for (int i = 0; i < 3; ++i) {
            cmsCIEXYZ adapted;

            if (!cmsAdaptToIlluminant(&adapted, &d50Xyz, &xyz[3], &xyz[i])) {
                return false;
            }

            xyz[i] = adapted;
        }
    }

    for (int i = 0; i < 4; ++i) {
        const double sum = xyz[i].X + xyz[i].Y + xyz[i].Z;

        if (!(std::abs(sum) > 1e-6)) {
            return false;
        }

        primaries[2 * i] = xyz[i].X / sum;
        primaries[2 * i + 1] = xyz[i].Y / sum;
    }

    return true;
}

}

int cicpPrimaries(cmsHPROFILE profile)
{
    float primaries[8];

    if (!profile || !iccPrimariesXY(profile, primaries)) {
        return AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    }

    const avifColorPrimaries found = avifColorPrimariesFind(primaries, nullptr);
    return found == AVIF_COLOR_PRIMARIES_UNKNOWN ? AVIF_COLOR_PRIMARIES_UNSPECIFIED : found;
}

int cicpTransferCharacteristics(cmsHPROFILE profile)
{
    if (!profile) {
        return AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    }

    const cmsTagSignature trcTags[3] = {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
    cmsToneCurve* curves[3];

    for (int i = 0; i < 3; ++i) {
        curves[i] = static_cast<cmsToneCurve*>(cmsReadTag(profile, trcTags[i]));

        if (!curves[i]) {
            return AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
        }
    }

    struct Candidate {
        avifTransferCharacteristics tc;
        double (*curve)(double);
    };

    const Candidate candidates[] = {
        {
            AVIF_TRANSFER_CHARACTERISTICS_SRGB,
            [](double x) { return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); }
        },
        {
            AVIF_TRANSFER_CHARACTERISTICS_BT709,
            [](double x) { return x < 0.081 ? x / 4.5 : std::pow((x + 0.099) / 1.099, 1.0 / 0.45); }
        },
        {AVIF_TRANSFER_CHARACTERISTICS_BT470M, [](double x) { return std::pow(x, 2.2); }},
        {AVIF_TRANSFER_CHARACTERISTICS_BT470BG, [](double x) { return std::pow(x, 2.8); }},
        {AVIF_TRANSFER_CHARACTERISTICS_LINEAR, [](double x) { return x; }},
        {AVIF_TRANSFER_CHARACTERISTICS_PQ, Color::pq_eotf},
        {AVIF_TRANSFER_CHARACTERISTICS_HLG, Color::hlg_inv_oetf}
    };

    constexpr int samples = 64;
    constexpr double tolerance = 1e-3;

    for (const Candidate& candidate : candidates) {
        bool match = true;

        for (int s = 0; s <= samples && match; ++s) {
            const double x = static_cast<double>(s) / samples;
            const double expected = candidate.curve(x);

            for (int channel = 0; channel < 3 && match; ++channel) {
                match = std::abs(cmsEvalToneCurveFloat(curves[channel], x) - expected) <= tolerance;
            }
        }

        if (match) {
            return candidate.tc;
        }
    }

    return AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
}

bool isUsableSdrBaseProfile(cmsHPROFILE profile)
{
    if (!profile || unexpressibleTransferFunction(profile)) {
        return false;
    }

    Matrix3 m;

    if (!profileMatrix(profile, m) || cicpPrimaries(profile) == AVIF_COLOR_PRIMARIES_UNSPECIFIED) {
        return false;
    }

    switch (cicpTransferCharacteristics(profile)) {
        case AVIF_TRANSFER_CHARACTERISTICS_SRGB:
        case AVIF_TRANSFER_CHARACTERISTICS_BT709:
        case AVIF_TRANSFER_CHARACTERISTICS_BT470M:
        case AVIF_TRANSFER_CHARACTERISTICS_BT470BG:
            return true;

        default:
            return false;
    }
}

}

}
