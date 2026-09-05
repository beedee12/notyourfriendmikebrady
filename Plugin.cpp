#include <windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

// Exact Ship Explorer build from supplied logs.
constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0A4C6000;
constexpr uintptr_t SLATE_DRAW_RVA = 0x02FC7D40;
constexpr size_t SNAPSHOT_SIZE = 64;

// 1 metre in Unreal units at w2m=100.
constexpr double TAG_Z_OFFSET = 100.0;

// Phase lengths in milliseconds:
// 0-4s baseline
// 4-8s tag view 1
// 8-12s baseline
// 12-16s tag view 2
// repeat
constexpr uint64_t PHASE_MS = 4000;
constexpr uint64_t CYCLE_MS = PHASE_MS * 4;

struct DVec3 {
    double x;
    double y;
    double z;
};

uint8_t* g_slate_draw = nullptr;
uint8_t g_original[SNAPSHOT_SIZE]{};
bool g_snapshot_ok = false;
bool g_slate_restored = false;
bool g_slate_change_seen = false;

uint32_t g_tick = 0;
uint64_t g_start_ms = 0;
int g_last_phase = -1;

uint32_t get_image_size(uint8_t* module) {
    if (!module) {
        return 0;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    return nt->OptionalHeader.SizeOfImage;
}

bool restore_slate_bytes() {
    if (!g_snapshot_ok || !g_slate_draw) {
        return false;
    }

    DWORD old{};
    if (!VirtualProtect(
            g_slate_draw,
            SNAPSHOT_SIZE,
            PAGE_EXECUTE_READWRITE,
            &old))
    {
        API::get()->log_error("[SHIPTAG13] VirtualProtect failed restoring Slate");
        return false;
    }

    std::memcpy(g_slate_draw, g_original, SNAPSHOT_SIZE);

    DWORD ignored{};
    VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_slate_draw, SNAPSHOT_SIZE);

    return std::memcmp(g_slate_draw, g_original, SNAPSHOT_SIZE) == 0;
}

int current_phase() {
    const uint64_t now = GetTickCount64();
    const uint64_t elapsed = now - g_start_ms;
    return static_cast<int>((elapsed % CYCLE_MS) / PHASE_MS);
}

const char* phase_name(int phase) {
    switch (phase) {
        case 0: return "BASELINE";
        case 1: return "TAG_VIEW_1_UP_1M";
        case 2: return "BASELINE";
        case 3: return "TAG_VIEW_2_UP_1M";
        default: return "UNKNOWN";
    }
}

void announce_phase_if_needed() {
    const int phase = current_phase();

    if (phase != g_last_phase) {
        g_last_phase = phase;

        API::get()->log_info(
            "[SHIPTAG13] PHASE=%d %s",
            phase,
            phase_name(phase)
        );
    }
}

void pre_engine_tick(
    UEVR_UGameEngineHandle,
    float)
{
    ++g_tick;
    announce_phase_if_needed();

    // Preserve the successful V11 anti-Slate behavior.
    if (g_snapshot_ok && !g_slate_restored) {
        if (std::memcmp(
                g_slate_draw,
                g_original,
                SNAPSHOT_SIZE
            ) != 0)
        {
            if (!g_slate_change_seen) {
                g_slate_change_seen = true;

                API::get()->log_info(
                    "[SHIPTAG13] DETECTED Slate DrawWindow hook at tick=%u target=%p",
                    g_tick,
                    g_slate_draw
                );
            }

            if (restore_slate_bytes()) {
                g_slate_restored = true;

                API::get()->log_info(
                    "[SHIPTAG13] RESTORED original Slate DrawWindow bytes at tick=%u",
                    g_tick
                );
            } else {
                API::get()->log_error(
                    "[SHIPTAG13] Slate restore failed at tick=%u",
                    g_tick
                );
            }
        }
    }
}

void post_stereo_cb(
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

    const int phase = current_phase();

    bool tag = false;

    if (phase == 1 && view_index == 1) {
        tag = true;
    } else if (phase == 3 && view_index == 2) {
        tag = true;
    }

    if (!tag) {
        return;
    }

    auto* p = reinterpret_cast<DVec3*>(position);

    // Deliberately alter ONLY vertical position.
    // Rotation/headtracking is untouched.
    p->z += TAG_Z_OFFSET;

    static uint32_t logs = 0;
    if (logs++ < 80) {
        API::get()->log_info(
            "[SHIPTAG13] tagged view=%d +Z=%.1f w2m=%.1f resultingZ=%.6f",
            view_index,
            TAG_Z_OFFSET,
            world_to_meters,
            p->z
        );
    }
}

}

class ShipExplorerViewTaggerV13 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPTAG13] loaded - V11 anti-Slate + stereo view identity tagger"
        );

        g_start_ms = GetTickCount64();

        uint8_t* module = reinterpret_cast<uint8_t*>(
            GetModuleHandleA(nullptr)
        );

        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPTAG13] game module=%p imageSize=0x%X expected=0x%X",
            module,
            image_size,
            EXPECTED_IMAGE_SIZE
        );

        if (!module || image_size != EXPECTED_IMAGE_SIZE) {
            API::get()->log_error(
                "[SHIPTAG13] GAME BUILD MISMATCH - Slate restore disabled"
            );
        } else {
            g_slate_draw = module + SLATE_DRAW_RVA;
            std::memcpy(g_original, g_slate_draw, SNAPSHOT_SIZE);
            g_snapshot_ok = true;

            API::get()->log_info(
                "[SHIPTAG13] snapshotted original Slate bytes at %p",
                g_slate_draw
            );
        }

        auto* callbacks = API::get()->param()->sdk->callbacks;

        callbacks->on_pre_engine_tick(&pre_engine_tick);
        callbacks->on_post_calculate_stereo_view_offset(&post_stereo_cb);

        API::get()->log_info(
            "[SHIPTAG13] cycle: 4s baseline -> 4s VIEW1 +1m Z -> 4s baseline -> 4s VIEW2 +1m Z"
        );
        API::get()->log_info(
            "[SHIPTAG13] rotation untouched; temporal settings untouched"
        );
    }
};

std::unique_ptr<ShipExplorerViewTaggerV13> g_plugin{
    new ShipExplorerViewTaggerV13()
};
