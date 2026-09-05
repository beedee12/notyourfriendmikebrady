#include <windows.h>
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
uint8_t g_original[SNAPSHOT_SIZE]{};

bool g_snapshot_ok = false;
bool g_slate_restored = false;
bool g_change_seen = false;
uint32_t g_tick = 0;

uint32_t get_image_size(uint8_t* module) {
    if (!module) return 0;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    return nt->OptionalHeader.SizeOfImage;
}

bool restore_slate_bytes() {
    if (!g_snapshot_ok || !g_slate_draw) return false;

    DWORD old{};
    if (!VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, PAGE_EXECUTE_READWRITE, &old)) {
        API::get()->log_error("[SHIPCTRL14] VirtualProtect failed restoring Slate");
        return false;
    }

    std::memcpy(g_slate_draw, g_original, SNAPSHOT_SIZE);

    DWORD ignored{};
    VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_slate_draw, SNAPSHOT_SIZE);

    return std::memcmp(g_slate_draw, g_original, SNAPSHOT_SIZE) == 0;
}

void pre_engine_tick(UEVR_UGameEngineHandle, float) {
    ++g_tick;

    if (!g_snapshot_ok || g_slate_restored) return;

    if (std::memcmp(g_slate_draw, g_original, SNAPSHOT_SIZE) == 0) return;

    if (!g_change_seen) {
        g_change_seen = true;
        API::get()->log_info(
            "[SHIPCTRL14] DETECTED Slate DrawWindow hook at tick=%u target=%p",
            g_tick,
            g_slate_draw
        );
    }

    if (restore_slate_bytes()) {
        g_slate_restored = true;
        API::get()->log_info(
            "[SHIPCTRL14] RESTORED original Slate DrawWindow bytes at tick=%u",
            g_tick
        );
    } else {
        API::get()->log_error(
            "[SHIPCTRL14] Slate restore failed at tick=%u",
            g_tick
        );
    }
}

}

class ShipExplorerControlV14 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPCTRL14] loaded - MINIMAL CONTROL: anti-Slate only"
        );

        uint8_t* module = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPCTRL14] game module=%p imageSize=0x%X expected=0x%X",
            module,
            image_size,
            EXPECTED_IMAGE_SIZE
        );

        if (!module || image_size != EXPECTED_IMAGE_SIZE) {
            API::get()->log_error(
                "[SHIPCTRL14] GAME BUILD MISMATCH - disabled"
            );
            return;
        }

        g_slate_draw = module + SLATE_DRAW_RVA;
        std::memcpy(g_original, g_slate_draw, SNAPSHOT_SIZE);
        g_snapshot_ok = true;

        API::get()->log_info(
            "[SHIPCTRL14] snapshotted original Slate bytes at %p",
            g_slate_draw
        );

        API::get()->param()->sdk->callbacks->on_pre_engine_tick(&pre_engine_tick);

        API::get()->log_info(
            "[SHIPCTRL14] initialized - NO stereo callback, NO OpenVR hook, NO camera changes"
        );
    }
};

std::unique_ptr<ShipExplorerControlV14> g_plugin{
    new ShipExplorerControlV14()
};
