#include <windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

// Exact Ship Explorer executable build from the supplied logs.
constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0A4C6000;
constexpr uintptr_t SLATE_DRAW_RVA = 0x02FC7D40;
constexpr size_t SNAPSHOT_SIZE = 64;

uint8_t* g_slate_draw = nullptr;
uint8_t g_original[SNAPSHOT_SIZE]{};
bool g_snapshot_ok = false;
bool g_slate_restored = false;
bool g_slate_change_seen = false;

uint32_t g_tick = 0;
bool g_temporal_test_applied = false;
uint32_t g_temporal_attempts = 0;

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
        API::get()->log_error("[SHIPFIX12] VirtualProtect failed restoring Slate");
        return false;
    }

    std::memcpy(g_slate_draw, g_original, SNAPSHOT_SIZE);

    DWORD ignored{};
    VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, old, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        g_slate_draw,
        SNAPSHOT_SIZE
    );

    return std::memcmp(
        g_slate_draw,
        g_original,
        SNAPSHOT_SIZE
    ) == 0;
}

bool set_cvar(const wchar_t* name, int value) {
    auto* cm = API::get()->get_console_manager();

    if (!cm) {
        return false;
    }

    auto* var = cm->find_variable(name);

    if (!var) {
        API::get()->log_warn(
            "[SHIPFIX12] cvar not found: %ls",
            name
        );
        return false;
    }

    const int before = var->get_int();
    var->set(value);
    const int after = var->get_int();

    API::get()->log_info(
        "[SHIPFIX12] cvar %ls: %d -> %d",
        name,
        before,
        after
    );

    return after == value;
}

void try_apply_temporal_test() {
    if (g_temporal_test_applied) {
        return;
    }

    ++g_temporal_attempts;

    auto* cm = API::get()->get_console_manager();
    if (!cm) {
        return;
    }

    API::get()->log_info(
        "[SHIPFIX12] applying TEMPORAL-OFF diagnostic at tick=%u attempt=%u",
        g_tick,
        g_temporal_attempts
    );

    int found = 0;

    // Global AA path: kills TAA/TSR history for the diagnostic run.
    found += set_cvar(L"r.AntiAliasingMethod", 0) ? 1 : 0;
    found += set_cvar(L"r.PostProcessAAQuality", 0) ? 1 : 0;
    found += set_cvar(L"r.TemporalAA.Upsampling", 0) ? 1 : 0;

    // Motion blur is explicitly warned about for synchronized sequential.
    found += set_cvar(L"r.MotionBlurQuality", 0) ? 1 : 0;
    found += set_cvar(L"r.DefaultFeature.MotionBlur", 0) ? 1 : 0;
    found += set_cvar(L"r.MotionBlur.Max", 0) ? 1 : 0;

    API::get()->log_info(
        "[SHIPFIX12] temporal-off diagnostic applied; %d/6 requested cvars confirmed",
        found
    );

    // Once console manager is alive, don't keep hammering values every frame.
    g_temporal_test_applied = true;
}

void pre_engine_tick(
    UEVR_UGameEngineHandle,
    float)
{
    ++g_tick;

    // Keep the V11 Slate repair.
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
                    "[SHIPFIX12] DETECTED Slate DrawWindow hook at tick=%u target=%p",
                    g_tick,
                    g_slate_draw
                );
            }

            if (restore_slate_bytes()) {
                g_slate_restored = true;

                API::get()->log_info(
                    "[SHIPFIX12] RESTORED original Slate DrawWindow bytes at tick=%u",
                    g_tick
                );
            } else {
                API::get()->log_error(
                    "[SHIPFIX12] Slate restore failed at tick=%u",
                    g_tick
                );
            }
        }
    }

    // Wait until the engine has actually started ticking before probing console vars.
    if (!g_temporal_test_applied && g_tick >= 10) {
        try_apply_temporal_test();
    }
}

}

class ShipExplorerTemporalTestV12 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPFIX12] loaded - V11 anti-Slate + TEMPORAL-OFF diagnostic"
        );

        uint8_t* module = reinterpret_cast<uint8_t*>(
            GetModuleHandleA(nullptr)
        );

        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPFIX12] game module=%p imageSize=0x%X expected=0x%X",
            module,
            image_size,
            EXPECTED_IMAGE_SIZE
        );

        if (!module || image_size != EXPECTED_IMAGE_SIZE) {
            API::get()->log_error(
                "[SHIPFIX12] GAME BUILD MISMATCH - Slate restore disabled"
            );
        } else {
            g_slate_draw = module + SLATE_DRAW_RVA;

            std::memcpy(
                g_original,
                g_slate_draw,
                SNAPSHOT_SIZE
            );

            g_snapshot_ok = true;

            API::get()->log_info(
                "[SHIPFIX12] snapshotted original Slate bytes at %p",
                g_slate_draw
            );
        }

        API::get()->param()->sdk->callbacks->on_pre_engine_tick(
            &pre_engine_tick
        );

        API::get()->log_info(
            "[SHIPFIX12] initialization complete; NO camera/eye/compositor modification"
        );
    }
};

std::unique_ptr<ShipExplorerTemporalTestV12> g_plugin{
    new ShipExplorerTemporalTestV12()
};
