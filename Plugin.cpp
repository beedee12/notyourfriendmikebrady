#include <memory>
#include <cmath>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
    struct DVec3 { double x, y, z; };

    DVec3 last_raw_1{};
    DVec3 last_raw_2{};
    bool have_1 = false;
    bool have_2 = false;

    // V8 deliberately collapses ONLY UEVR's per-eye positional separation.
    // Rotation is untouched, so existing head rotation/tracking remains UEVR's.
    //
    // This is a controlled test: if the two misaligned/flickering images line up,
    // we've proven the bad stereo offset itself is the culprit. V9 can then
    // re-introduce a sane IPD instead of the game's enormous/swapped separation.
    constexpr double STEREO_SCALE = 0.0;

    int logs = 0;

    static DVec3 add(DVec3 a, DVec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
    static DVec3 sub(DVec3 a, DVec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
    static DVec3 mul(DVec3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }

    void post_cb(
        UEVR_StereoRenderingDeviceHandle,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f* position,
        UEVR_Rotatorf*,
        bool is_double)
    {
        if (!position || !is_double || (view_index != 1 && view_index != 2)) {
            return;
        }

        auto* p = reinterpret_cast<DVec3*>(position);
        DVec3 raw{p->x, p->y, p->z};

        // First collect genuine UEVR outputs. Never derive the separation from
        // already-corrected coordinates.
        if (view_index == 1) {
            last_raw_1 = raw;
            have_1 = true;
        } else {
            last_raw_2 = raw;
            have_2 = true;
        }

        if (!(have_1 && have_2)) {
            return;
        }

        // Half of UEVR's measured eye-to-eye displacement, in Unreal units.
        const DVec3 half_sep = mul(sub(last_raw_2, last_raw_1), 0.5);

        // Preserve the current view's live head/camera translation, while scaling
        // only the eye-specific displacement around the common center.
        DVec3 corrected = raw;
        if (view_index == 1) {
            // raw1 = center - half_sep
            corrected = add(raw, mul(half_sep, 1.0 - STEREO_SCALE));
        } else {
            // raw2 = center + half_sep
            corrected = sub(raw, mul(half_sep, 1.0 - STEREO_SCALE));
        }

        p->x = corrected.x;
        p->y = corrected.y;
        p->z = corrected.z;

        if (logs++ < 80) {
            const double sep_len = std::sqrt(
                half_sep.x*half_sep.x +
                half_sep.y*half_sep.y +
                half_sep.z*half_sep.z
            ) * 2.0;

            API::get()->log_info(
                "[SHIPFIX8] view=%d double=1 w2m=%.3f raw=(%.6f %.6f %.6f) corrected=(%.6f %.6f %.6f) measuredEyeDistance=%.6f",
                view_index, world_to_meters,
                raw.x, raw.y, raw.z,
                corrected.x, corrected.y, corrected.z,
                sep_len
            );
        }
    }
}

class ShipExplorerStereoFixV8 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info("[SHIPFIX8] loaded - positional stereo collapse test, rotations untouched");
        API::get()->param()->sdk->callbacks->on_post_calculate_stereo_view_offset(&post_cb);
        API::get()->log_info("[SHIPFIX8] POST callback registered");
    }
};

std::unique_ptr<ShipExplorerStereoFixV8> g_plugin{new ShipExplorerStereoFixV8()};
