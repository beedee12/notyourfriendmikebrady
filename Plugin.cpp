#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

// -----------------------------------------------------------------------------
// Exact Ship Explorer executable build from supplied logs.
// Keep the proven anti-Slate repair as a secondary safeguard.
// -----------------------------------------------------------------------------

constexpr uint32_t EXPECTED_IMAGE_SIZE = 0x0A4C6000;
constexpr uintptr_t SLATE_DRAW_RVA = 0x02FC7D40;
constexpr size_t SNAPSHOT_SIZE = 64;

uint8_t* g_slate_draw = nullptr;
uint8_t g_original_slate[SNAPSHOT_SIZE]{};
bool g_slate_snapshot_ok = false;
bool g_slate_restored = false;
bool g_slate_change_seen = false;
uint32_t g_tick = 0;

// -----------------------------------------------------------------------------
// OpenVR compositor gate.
//
// Core idea:
// OpenVR error 108 (AlreadySubmitted) causes UEVR to do:
//   got_first_poses = false;
//   needs_pose_update = true;
//
// That is exactly the wrong thing for Ship Explorer if synchronized sequential
// is already struggling to keep both eyes on the same HMD pose.
//
// V15 therefore lets the FIRST submit for each eye through after each
// WaitGetPoses, but suppresses any duplicate submit for that same eye until
// the next WaitGetPoses. Suppressed duplicates return success (0) to UEVR.
//
// This is not faking a displayed frame: the duplicate texture was going to be
// rejected by SteamVR with 108 anyway.
// -----------------------------------------------------------------------------

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

std::atomic<bool> g_seen_left{false};
std::atomic<bool> g_seen_right{false};

std::atomic<uint64_t> g_suppressed_left{0};
std::atomic<uint64_t> g_suppressed_right{0};

std::atomic<uint32_t> g_logs{0};
constexpr uint32_t MAX_LOGS = 500;

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

    const int32_t result = g_wait_original(
        self,
        render_poses,
        render_pose_count,
        game_poses,
        game_pose_count
    );

    const uint64_t wid = g_wait_id.fetch_add(1) + 1;

    // New compositor frame boundary.
    g_seen_left.store(false);
    g_seen_right.store(false);

    if (g_logs.fetch_add(1) < MAX_LOGS) {
        API::get()->log_info(
            "[SHIPFIX15] WAIT id=%llu result=%d suppressedTotal{L=%llu R=%llu}",
            static_cast<unsigned long long>(wid),
            result,
            static_cast<unsigned long long>(g_suppressed_left.load()),
            static_cast<unsigned long long>(g_suppressed_right.load())
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

    const uint64_t sid = g_submit_id.fetch_add(1) + 1;
    const uint64_t wid = g_wait_id.load();

    bool duplicate = false;

    if (eye == 0) {
        duplicate = g_seen_left.exchange(true);
    } else if (eye == 1) {
        duplicate = g_seen_right.exchange(true);
    }

    if (duplicate) {
        if (eye == 0) {
            g_suppressed_left.fetch_add(1);
        } else if (eye == 1) {
            g_suppressed_right.fetch_add(1);
        }

        if (g_logs.fetch_add(1) < MAX_LOGS) {
            API::get()->log_info(
                "[SHIPFIX15] SUPPRESS duplicate SUBMIT id=%llu wait=%llu eye=%s(%d) tex=%p -> returning success",
                static_cast<unsigned long long>(sid),
                static_cast<unsigned long long>(wid),
                eye == 0 ? "LEFT" : (eye == 1 ? "RIGHT" : "OTHER"),
                eye,
                texture ? texture->handle : nullptr
            );
        }

        // VRCompositorError_None.
        return 0;
    }

    const int32_t result = g_submit_original(
        self,
        eye,
        texture,
        bounds,
        flags
    );

    if (g_logs.fetch_add(1) < MAX_LOGS) {
        API::get()->log_info(
            "[SHIPFIX15] PASS SUBMIT id=%llu wait=%llu eye=%s(%d) tex=%p result=%d",
            static_cast<unsigned long long>(sid),
            static_cast<unsigned long long>(wid),
            eye == 0 ? "LEFT" : (eye == 1 ? "RIGHT" : "OTHER"),
            eye,
            texture ? texture->handle : nullptr,
            result
        );
    }

    return result;
}

void hook_openvr_027() {
    HMODULE openvr = GetModuleHandleA("openvr_api.dll");

    if (!openvr) {
        API::get()->log_error("[SHIPFIX15] openvr_api.dll not loaded");
        return;
    }

    auto get_iface = reinterpret_cast<GetGenericInterfaceFn>(
        GetProcAddress(openvr, "VR_GetGenericInterface")
    );

    if (!get_iface) {
        API::get()->log_error("[SHIPFIX15] VR_GetGenericInterface unavailable");
        return;
    }

    int32_t error = 0;
    void* iface = get_iface("IVRCompositor_027", &error);

    if (!iface || error != 0) {
        API::get()->log_error(
            "[SHIPFIX15] IVRCompositor_027 unavailable iface=%p error=%d",
            iface,
            error
        );
        return;
    }

    auto*** p = reinterpret_cast<void***>(iface);
    if (!p || !*p) {
        API::get()->log_error("[SHIPFIX15] invalid IVRCompositor_027 vtable");
        return;
    }

    g_comp_vtable = *p;

    void* original_wait{};
    void* original_submit{};

    // IVRCompositor:
    //   index 2 = WaitGetPoses
    //   index 5 = Submit
    if (!patch_ptr(
            &g_comp_vtable[2],
            reinterpret_cast<void*>(&wait_hook),
            &original_wait))
    {
        API::get()->log_error("[SHIPFIX15] failed to hook WaitGetPoses");
        return;
    }

    g_wait_original = reinterpret_cast<WaitGetPosesFn>(original_wait);

    if (!patch_ptr(
            &g_comp_vtable[5],
            reinterpret_cast<void*>(&submit_hook),
            &original_submit))
    {
        API::get()->log_error("[SHIPFIX15] failed to hook Submit");
        return;
    }

    g_submit_original = reinterpret_cast<SubmitFn>(original_submit);

    API::get()->log_info(
        "[SHIPFIX15] hooked IVRCompositor_027 iface=%p vtable=%p",
        iface,
        g_comp_vtable
    );
}

// -----------------------------------------------------------------------------
// Anti-Slate safeguard from V11/V14.
// -----------------------------------------------------------------------------

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
    if (!g_slate_snapshot_ok || !g_slate_draw) {
        return false;
    }

    DWORD old{};
    if (!VirtualProtect(
            g_slate_draw,
            SNAPSHOT_SIZE,
            PAGE_EXECUTE_READWRITE,
            &old))
    {
        return false;
    }

    std::memcpy(g_slate_draw, g_original_slate, SNAPSHOT_SIZE);

    DWORD ignored{};
    VirtualProtect(g_slate_draw, SNAPSHOT_SIZE, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_slate_draw, SNAPSHOT_SIZE);

    return std::memcmp(
        g_slate_draw,
        g_original_slate,
        SNAPSHOT_SIZE
    ) == 0;
}

void pre_engine_tick(
    UEVR_UGameEngineHandle,
    float)
{
    ++g_tick;

    if (!g_slate_snapshot_ok || g_slate_restored) {
        return;
    }

    if (std::memcmp(
            g_slate_draw,
            g_original_slate,
            SNAPSHOT_SIZE
        ) == 0)
    {
        return;
    }

    if (!g_slate_change_seen) {
        g_slate_change_seen = true;

        API::get()->log_info(
            "[SHIPFIX15] DETECTED Slate DrawWindow hook at tick=%u",
            g_tick
        );
    }

    if (restore_slate_bytes()) {
        g_slate_restored = true;

        API::get()->log_info(
            "[SHIPFIX15] RESTORED original Slate DrawWindow bytes at tick=%u",
            g_tick
        );
    } else {
        API::get()->log_error(
            "[SHIPFIX15] Slate restore failed at tick=%u",
            g_tick
        );
    }
}

}

class ShipExplorerPosePairV15 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPFIX15] loaded - duplicate-submit gate + anti-Slate; NO camera transforms"
        );

        uint8_t* module = reinterpret_cast<uint8_t*>(
            GetModuleHandleA(nullptr)
        );

        const uint32_t image_size = get_image_size(module);

        API::get()->log_info(
            "[SHIPFIX15] game module=%p imageSize=0x%X expected=0x%X",
            module,
            image_size,
            EXPECTED_IMAGE_SIZE
        );

        if (module && image_size == EXPECTED_IMAGE_SIZE) {
            g_slate_draw = module + SLATE_DRAW_RVA;
            std::memcpy(
                g_original_slate,
                g_slate_draw,
                SNAPSHOT_SIZE
            );
            g_slate_snapshot_ok = true;

            API::get()->param()->sdk->callbacks->on_pre_engine_tick(
                &pre_engine_tick
            );
        } else {
            API::get()->log_error(
                "[SHIPFIX15] game build mismatch - anti-Slate disabled"
            );
        }

        hook_openvr_027();

        API::get()->log_info(
            "[SHIPFIX15] initialized - duplicate eye submissions will be suppressed until next WaitGetPoses"
        );
    }
};

std::unique_ptr<ShipExplorerPosePairV15> g_plugin{
    new ShipExplorerPosePairV15()
};
