#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

// Ship Explorer build seen in the supplied logs:
// Game Module Size = 0x0A4C6000
// FSlateRHIRenderer::DrawWindow_RenderThread = module + 0x02FC7D40
constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0A4C6000;
constexpr uintptr_t SLATE_DRAW_RVA = 0x02FC7D40;
constexpr size_t SNAPSHOT_SIZE = 64;

uint8_t* g_slate_draw = nullptr;
uint8_t g_original[SNAPSHOT_SIZE]{};
bool g_snapshot_ok = false;
bool g_restored = false;
bool g_change_seen = false;
uint32_t g_tick_count = 0;

// -------------------------------------------------------------------------
// Minimal OpenVR compositor diagnostic.
// Unlike V10, hook ONLY IVRCompositor_027 so wrapper forwarding is not
// double/triple-counted.
// -------------------------------------------------------------------------

struct VRTexture {
    void* handle;
    int32_t type;
    int32_t color_space;
};

struct VRBounds {
    float uMin;
    float vMin;
    float uMax;
    float vMax;
};

using SubmitFn = int32_t(__fastcall*)(
    void* self,
    int32_t eye,
    const VRTexture* texture,
    const VRBounds* bounds,
    uint32_t flags
);

using WaitGetPosesFn = int32_t(__fastcall*)(
    void* self,
    void* render_poses,
    uint32_t render_pose_count,
    void* game_poses,
    uint32_t game_pose_count
);

using GetGenericInterfaceFn = void* (__cdecl*)(
    const char* interface_version,
    int32_t* error
);

SubmitFn g_submit_original = nullptr;
WaitGetPosesFn g_wait_original = nullptr;
void** g_comp_vtable = nullptr;

std::atomic<uint64_t> g_wait_id{0};
std::atomic<uint64_t> g_submit_id{0};
std::atomic<uint32_t> g_comp_logs{0};

constexpr uint32_t MAX_COMP_LOGS = 320;

int32_t __fastcall wait_hook(
    void* self,
    void* render_poses,
    uint32_t render_pose_count,
    void* game_poses,
    uint32_t game_pose_count)
{
    if (!g_wait_original) {
        return 1;
    }

    int32_t result = g_wait_original(
        self,
        render_poses,
        render_pose_count,
        game_poses,
        game_pose_count
    );

    const uint64_t id = g_wait_id.fetch_add(1) + 1;

    if (g_comp_logs.fetch_add(1) < MAX_COMP_LOGS) {
        API::get()->log_info(
            "[SHIPFIX11] WAIT id=%llu result=%d slateRestored=%d",
            static_cast<unsigned long long>(id),
            result,
            g_restored ? 1 : 0
        );
    }

    return result;
}

int32_t __fastcall submit_hook(
    void* self,
    int32_t eye,
    const VRTexture* texture,
    const VRBounds* bounds,
    uint32_t flags)
{
    if (!g_submit_original) {
        return 1;
    }

    const int32_t result = g_submit_original(
        self,
        eye,
        texture,
        bounds,
        flags
    );

    const uint64_t id = g_submit_id.fetch_add(1) + 1;
    const uint64_t wait = g_wait_id.load();

    if (g_comp_logs.fetch_add(1) < MAX_COMP_LOGS) {
        API::get()->log_info(
            "[SHIPFIX11] SUBMIT id=%llu wait=%llu eye=%s(%d) tex=%p result=%d slateRestored=%d",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(wait),
            eye == 0 ? "LEFT" : (eye == 1 ? "RIGHT" : "OTHER"),
            eye,
            texture ? texture->handle : nullptr,
            result,
            g_restored ? 1 : 0
        );
    }

    return result;
}

bool patch_ptr(void** slot, void* replacement, void** original_out) {
    if (!slot || !*slot || !replacement || !original_out) {
        return false;
    }

    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }

    *original_out = *slot;
    *slot = replacement;

    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    return true;
}

void hook_openvr_027() {
    HMODULE openvr = GetModuleHandleA("openvr_api.dll");

    if (!openvr) {
        API::get()->log_error("[SHIPFIX11] openvr_api.dll not loaded");
        return;
    }

    auto get_iface = reinterpret_cast<GetGenericInterfaceFn>(
        GetProcAddress(openvr, "VR_GetGenericInterface")
    );

    if (!get_iface) {
        API::get()->log_error("[SHIPFIX11] VR_GetGenericInterface unavailable");
        return;
    }

    int32_t error = 0;
    void* iface = get_iface("IVRCompositor_027", &error);

    if (!iface || error != 0) {
        API::get()->log_error(
            "[SHIPFIX11] IVRCompositor_027 unavailable iface=%p error=%d",
            iface,
            error
        );
        return;
    }

    auto*** p = reinterpret_cast<void***>(iface);
    if (!p || !*p) {
        API::get()->log_error("[SHIPFIX11] invalid compositor vtable");
        return;
    }

    g_comp_vtable = *p;

    void* original_wait{};
    void* original_submit{};

    // IVRCompositor: index 2 = WaitGetPoses, index 5 = Submit.
    if (!patch_ptr(
            &g_comp_vtable[2],
            reinterpret_cast<void*>(&wait_hook),
            &original_wait))
    {
        API::get()->log_error("[SHIPFIX11] failed to hook WaitGetPoses");
        return;
    }

    g_wait_original = reinterpret_cast<WaitGetPosesFn>(original_wait);

    if (!patch_ptr(
            &g_comp_vtable[5],
            reinterpret_cast<void*>(&submit_hook),
            &original_submit))
    {
        API::get()->log_error("[SHIPFIX11] failed to hook Submit");
        return;
    }

    g_submit_original = reinterpret_cast<SubmitFn>(original_submit);

    API::get()->log_info(
        "[SHIPFIX11] hooked IVRCompositor_027 only iface=%p vtable=%p",
        iface,
        g_comp_vtable
    );
}

// -------------------------------------------------------------------------
// Slate anti-hook.
// -------------------------------------------------------------------------

uint32_t get_image_size(uint8_t* module) {
    if (!module) {
        return 0;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        module + dos->e_lfanew
    );

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
        API::get()->log_error("[SHIPFIX11] VirtualProtect failed restoring Slate");
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

void pre_engine_tick(
    UEVR_UGameEngineHandle,
    float)
{
    ++g_tick_count;

    if (!g_snapshot_ok || g_restored) {
        return;
    }

    if (std::memcmp(
            g_slate_draw,
            g_original,
            SNAPSHOT_SIZE
        ) == 0)
    {
        return;
    }

    if (!g_change_seen) {
        g_change_seen = true;

        API::get()->log_info(
            "[SHIPFIX11] DETECTED Slate DrawWindow hook at tick=%u target=%p",
            g_tick_count,
            g_slate_draw
        );
    }

    if (restore_slate_bytes()) {
        g_restored = true;

        API::get()->log_info(
            "[SHIPFIX11] RESTORED original Slate DrawWindow bytes at tick=%u",
            g_tick_count
        );
    } else {
        API::get()->log_error(
            "[SHIPFIX11] Slate restore attempt failed at tick=%u",
            g_tick_count
        );
    }
}

}

class ShipExplorerAntiSlateV11 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPFIX11] loaded - targeted anti-Slate-hook test"
        );

        uint8_t* module = reinterpret_cast<uint8_t*>(
            GetModuleHandleA(nullptr)
        );

        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPFIX11] game module=%p imageSize=0x%X expected=0x%X",
            module,
            image_size,
            EXPECTED_IMAGE_SIZE
        );

        if (!module || image_size != EXPECTED_IMAGE_SIZE) {
            API::get()->log_error(
                "[SHIPFIX11] GAME BUILD MISMATCH - Slate restore disabled"
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
                "[SHIPFIX11] snapshotted %zu original bytes at Slate target=%p RVA=0x%llX",
                SNAPSHOT_SIZE,
                g_slate_draw,
                static_cast<unsigned long long>(SLATE_DRAW_RVA)
            );

            // Engine tick is used only as a safe-ish place to notice that UEVR
            // has modified the function and restore the original bytes.
            API::get()->param()->sdk->callbacks->on_pre_engine_tick(
                &pre_engine_tick
            );
        }

        hook_openvr_027();

        API::get()->log_info(
            "[SHIPFIX11] initialization complete"
        );
    }
};

std::unique_ptr<ShipExplorerAntiSlateV11> g_plugin{
    new ShipExplorerAntiSlateV11()
};
