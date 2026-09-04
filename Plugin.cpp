#include <cmath>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

class ShipExplorerStereoFix : public uevr::Plugin {
public:
    void on_dllmain() override {}

    void on_initialize() override {
        API::get()->log_info("[SHIPFIX] loaded");
    }

    void on_pre_calculate_stereo_view_offset(
        UEVR_StereoRenderingDeviceHandle,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f* position,
        UEVR_Rotatorf* rotation,
        bool is_double) override
    {
        if (!is_double || (view_index != 1 && view_index != 2)) {
            return;
        }

        auto* pos = reinterpret_cast<UEVR_Vector3d*>(position);
        auto* rot = reinterpret_cast<UEVR_Rotatord*>(rotation);

        // HMD pose from UEVR runtime.
        UEVR_Vector3f hmd_pos{};
        UEVR_Quaternionf hmd_q{};
        auto vr = API::get()->param()->vr;
        vr->get_pose(vr->get_hmd_index(), &hmd_pos, &hmd_q);

        const auto hmd_rot = quat_to_rotator(hmd_q);

        // Restore headset orientation in Ship Explorer's SamePass-OFF two-view state.
        rot->pitch += hmd_rot.pitch;
        rot->yaw   += hmd_rot.yaw;
        rot->roll  += hmd_rot.roll;

        // Ship Explorer reports eyes as view 1 / view 2.
        // Replace its broken stereo separation with UEVR runtime eye offsets.
        UEVR_Vector3f eye_offset{};
        vr->get_eye_offset(view_index - 1, &eye_offset);

        // OpenVR eye offset is metres. Unreal units use world_to_meters.
        // UEVR/UE coordinate convention maps runtime X separation onto UE Y.
        pos->y += static_cast<double>(eye_offset.x * world_to_meters);

        if (m_logs < 30) {
            ++m_logs;
            API::get()->log_info(
                "[SHIPFIX] view=%d double=1 eye=(%.6f,%.6f,%.6f) pos=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f,%.3f)",
                view_index,
                eye_offset.x, eye_offset.y, eye_offset.z,
                pos->x, pos->y, pos->z,
                rot->pitch, rot->yaw, rot->roll
            );
        }
    }

private:
    struct RotD {
        double pitch;
        double yaw;
        double roll;
    };

    static constexpr double RAD2DEG = 57.2957795130823208768;

    static double norm_axis(double a) {
        a = std::fmod(a, 360.0);
        if (a < 0.0) a += 360.0;
        if (a > 180.0) a -= 360.0;
        return a;
    }

    static RotD quat_to_rotator(const UEVR_Quaternionf& q) {
        const double X = q.x;
        const double Y = q.y;
        const double Z = q.z;
        const double W = q.w;

        const double singularity = Z * X - W * Y;
        const double yawY = 2.0 * (W * Z + X * Y);
        const double yawX = 1.0 - 2.0 * (Y * Y + Z * Z);
        constexpr double limit = 0.4999995;

        RotD r{};

        if (singularity < -limit) {
            r.pitch = -90.0;
            r.yaw = std::atan2(yawY, yawX) * RAD2DEG;
            r.roll = norm_axis(-r.yaw - (2.0 * std::atan2(X, W) * RAD2DEG));
        } else if (singularity > limit) {
            r.pitch = 90.0;
            r.yaw = std::atan2(yawY, yawX) * RAD2DEG;
            r.roll = norm_axis(r.yaw - (2.0 * std::atan2(X, W) * RAD2DEG));
        } else {
            r.pitch = std::asin(2.0 * singularity) * RAD2DEG;
            r.yaw = std::atan2(yawY, yawX) * RAD2DEG;
            r.roll = std::atan2(
                -2.0 * (W * X + Y * Z),
                1.0 - 2.0 * (X * X + Y * Y)
            ) * RAD2DEG;
        }

        return r;
    }

    int m_logs{};
};

std::unique_ptr<ShipExplorerStereoFix> g_plugin{new ShipExplorerStereoFix()};
