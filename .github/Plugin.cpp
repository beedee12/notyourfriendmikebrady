#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0A4C6000;
constexpr uintptr_t SLATE_DRAW_RVA = 0x02FC7D40;
constexpr size_t SNAPSHOT_SIZE = 64;

uint8_t* g_slate_draw = nullptr;
uint8_t g_original_slate[SNAPSHOT_SIZE]{};
bool g_slate_snapshot_ok = false;
bool g_slate_restored = false;
bool g_slate_change_seen = false;
uint32_t g_tick = 0;

struct DVec3 { double x, y, z; };
struct DRotator { double pitch, yaw, roll; };

struct PairAnchor {
    bool valid{};
    int view{};
    DVec3 pos{};
    DRotator rot{};
    float w2m{};
    uint64_t time_ms{};
};

PairAnchor g_anchor{};

constexpr double IPD_METERS = 0.0657775;
constexpr uint64_t MAX_PAIR_AGE_MS = 80;
uint32_t g_pair_logs = 0;
constexpr uint32_t MAX_PAIR_LOGS = 140;

double deg_to_rad(double d) {
    return d * 3.14159265358979323846 / 180.0;
}

DVec3 right_axis_from_rotator(const DRotator& r) {
    const double pitch = deg_to_rad(r.pitch);
    const double yaw   = deg_to_rad(r.yaw);
    const double roll  = deg_to_rad(r.roll);

    const double SP = std::sin(pitch);
    const double CP = std::cos(pitch);
    const double SY = std::sin(yaw);
    const double CY = std::cos(yaw);
    const double SR = std::sin(roll);
    const double CR = std::cos(roll);

    DVec3 out{};
    out.x = SR * SP * CY - CR * SY;
    out.y = SR * SP * SY + CR * CY;
    out.z = -SR * CP;

    const double len = std::sqrt(out.x*out.x + out.y*out.y + out.z*out.z);
    if (len > 1e-9) {
        out.x /= len;
        out.y /= len;
        out.z /= len;
    }
    return out;
}

double distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

double rotation_delta(const DRotator& a, const DRotator& b) {
    auto shortest = [](double x) {
        while (x > 180.0) x -= 360.0;
        while (x < -180.0) x += 360.0;
        return x;
    };

    const double dp = shortest(a.pitch - b.pitch);
    const double dy = shortest(a.yaw - b.yaw);
    const double dr = shortest(a.roll - b.roll);
    return std::sqrt(dp*dp + dy*dy + dr*dr);
}

void pair_stereo_view(
    UEVR_StereoRenderingDeviceHandle,
    int view_index,
    float world_to_meters,
    UEVR_Vector3f* position,
    UEVR_Rotatorf* rotation,
    bool is_double)
{
    if (!position || !rotation || !is_double) return;
    if (view_index != 1 && view_index != 2) return;

    auto* pos = reinterpret_cast<DVec3*>(position);
    auto* rot = reinterpret_cast<DRotator*>(rotation);
    const uint64_t now = GetTickCount64();

    const bool expired = !g_anchor.valid || (now - g_anchor.time_ms) > MAX_PAIR_AGE_MS;
    const bool same_eye_again = g_anchor.valid && g_anchor.view == view_index;

    if (expired || same_eye_again) {
        g_anchor.valid = true;
        g_anchor.view = view_index;
        g_anchor.pos = *pos;
        g_anchor.rot = *rot;
        g_anchor.w2m = world_to_meters;
        g_anchor.time_ms = now;

        if (g_pair_logs++ < MAX_PAIR_LOGS) {
            API::get()->log_info(
                "[SHIPFIX16] ANCHOR view=%d pos=(%.6f %.6f %.6f) rot=(%.4f %.4f %.4f) w2m=%.2f",
                view_index, pos->x, pos->y, pos->z,
                rot->pitch, rot->yaw, rot->roll, world_to_meters
            );
        }
        return;
    }

    const DVec3 raw_pos = *pos;
    const DRotator raw_rot = *rot;

    const double w2m = g_anchor.w2m > 0.0f
        ? static_cast<double>(g_anchor.w2m)
        : static_cast<double>(world_to_meters);

    const double ipd_units = IPD_METERS * w2m;
    const DVec3 right = right_axis_from_rotator(g_anchor.rot);

    const double direction =
        (g_anchor.view == 1 && view_index == 2) ? +1.0 :
        (g_anchor.view == 2 && view_index == 1) ? -1.0 :
        0.0;

    if (direction == 0.0) {
        g_anchor.valid = false;
        return;
    }

    pos->x = g_anchor.pos.x + direction * right.x * ipd_units;
    pos->y = g_anchor.pos.y + direction * right.y * ipd_units;
    pos->z = g_anchor.pos.z + direction * right.z * ipd_units;
    *rot = g_anchor.rot;

    if (g_pair_logs++ < MAX_PAIR_LOGS) {
        API::get()->log_info(
            "[SHIPFIX16] PAIRED anchor=%d target=%d age=%llums rawSep=%.6f fixedSep=%.6f rawRotDelta=%.6f ipdUnits=%.6f",
            g_anchor.view,
            view_index,
            static_cast<unsigned long long>(now - g_anchor.time_ms),
            distance(g_anchor.pos, raw_pos),
            distance(g_anchor.pos, *pos),
            rotation_delta(g_anchor.rot, raw_rot),
            ipd_units
        );
    }

    g_anchor.valid = false;
}

uint32_t get_image_size(uint8_t* module) {
    if (!module) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

bool restore_slate_bytes() {
    if (!g_slate_snapshot_ok || !g_slate_draw) return false;

    DWORD old{};
    if (!VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, PAGE_EXECUTE_READWRITE, &old)) return false;

    std::memcpy(g_slate_draw, g_original_slate, SNAPSHOT_SIZE);

    DWORD ignored{};
    VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_slate_draw, SNAPSHOT_SIZE);

    return std::memcmp(g_slate_draw, g_original_slate, SNAPSHOT_SIZE) == 0;
}

void pre_engine_tick(UEVR_UGameEngineHandle, float) {
    ++g_tick;
    if (!g_slate_snapshot_ok || g_slate_restored) return;
    if (std::memcmp(g_slate_draw, g_original_slate, SNAPSHOT_SIZE) == 0) return;

    if (!g_slate_change_seen) {
        g_slate_change_seen = true;
        API::get()->log_info(
            "[SHIPFIX16] DETECTED Slate DrawWindow hook at tick=%u",
            g_tick
        );
    }

    if (restore_slate_bytes()) {
        g_slate_restored = true;
        API::get()->log_info(
            "[SHIPFIX16] RESTORED original Slate DrawWindow bytes at tick=%u",
            g_tick
        );
    } else {
        API::get()->log_error(
            "[SHIPFIX16] Slate restore failed at tick=%u",
            g_tick
        );
    }
}

}

class ShipExplorerPosePairerV16 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPFIX16] loaded - shared-pose stereo pairer + anti-Slate"
        );

        uint8_t* module = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPFIX16] game module=%p imageSize=0x%X expected=0x%X",
            module, image_size, EXPECTED_IMAGE_SIZE
        );

        if (module && image_size == EXPECTED_IMAGE_SIZE) {
            g_slate_draw = module + SLATE_DRAW_RVA;
            std::memcpy(g_original_slate, g_slate_draw, SNAPSHOT_SIZE);
            g_slate_snapshot_ok = true;

            API::get()->param()->sdk->callbacks->on_pre_engine_tick(
                &pre_engine_tick
            );
        } else {
            API::get()->log_error(
                "[SHIPFIX16] game build mismatch - anti-Slate disabled"
            );
        }

        API::get()->param()->sdk->callbacks->on_post_calculate_stereo_view_offset(
            &pair_stereo_view
        );

        API::get()->log_info(
            "[SHIPFIX16] initialized - pair window=%llums IPD=%.4fmm; NO extra HMD transform",
            static_cast<unsigned long long>(MAX_PAIR_AGE_MS),
            IPD_METERS * 1000.0
        );
    }
};

std::unique_ptr<ShipExplorerPosePairerV16> g_plugin{
    new ShipExplorerPosePairerV16()
};
