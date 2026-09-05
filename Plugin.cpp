#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

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

struct HookRecord {
    void** vtable{};
    SubmitFn original_submit{};
    WaitGetPosesFn original_wait{};
    char version[32]{};
};

HookRecord hooks[16]{};
int hook_count = 0;

std::atomic<uint64_t> wait_id{0};
std::atomic<uint64_t> submit_id{0};
std::atomic<uint32_t> log_count{0};

constexpr uint32_t MAX_LOGS = 500;

HookRecord* find_record(void* self) {
    if (!self) {
        return nullptr;
    }

    auto*** p = reinterpret_cast<void***>(self);
    if (!p || !*p) {
        return nullptr;
    }

    void** vt = *p;

    for (int i = 0; i < hook_count; ++i) {
        if (hooks[i].vtable == vt) {
            return &hooks[i];
        }
    }

    return nullptr;
}

int32_t __fastcall submit_hook(
    void* self,
    int32_t eye,
    const VRTexture* texture,
    const VRBounds* bounds,
    uint32_t flags)
{
    HookRecord* rec = find_record(self);

    if (!rec || !rec->original_submit) {
        API::get()->log_error("[SHIPDIAG10] Submit hook could not find original function");
        return 1;
    }

    const uint64_t sid = submit_id.fetch_add(1) + 1;
    const uint64_t wid = wait_id.load();

    int32_t result = rec->original_submit(self, eye, texture, bounds, flags);

    const uint32_t n = log_count.fetch_add(1);
    if (n < MAX_LOGS) {
        const char* eye_name =
            eye == 0 ? "LEFT" :
            eye == 1 ? "RIGHT" :
            "OTHER";

        void* tex_handle = texture ? texture->handle : nullptr;
        int32_t tex_type = texture ? texture->type : -999;
        int32_t color_space = texture ? texture->color_space : -999;

        if (bounds) {
            API::get()->log_info(
                "[SHIPDIAG10] SUBMIT id=%llu wait=%llu iface=%s eye=%s(%d) tex=%p type=%d color=%d bounds=(%.4f %.4f %.4f %.4f) flags=0x%X result=%d",
                static_cast<unsigned long long>(sid),
                static_cast<unsigned long long>(wid),
                rec->version,
                eye_name,
                eye,
                tex_handle,
                tex_type,
                color_space,
                bounds->uMin,
                bounds->vMin,
                bounds->uMax,
                bounds->vMax,
                flags,
                result
            );
        } else {
            API::get()->log_info(
                "[SHIPDIAG10] SUBMIT id=%llu wait=%llu iface=%s eye=%s(%d) tex=%p type=%d color=%d bounds=NULL flags=0x%X result=%d",
                static_cast<unsigned long long>(sid),
                static_cast<unsigned long long>(wid),
                rec->version,
                eye_name,
                eye,
                tex_handle,
                tex_type,
                color_space,
                flags,
                result
            );
        }
    }

    return result;
}

int32_t __fastcall wait_hook(
    void* self,
    void* render_poses,
    uint32_t render_pose_count,
    void* game_poses,
    uint32_t game_pose_count)
{
    HookRecord* rec = find_record(self);

    if (!rec || !rec->original_wait) {
        API::get()->log_error("[SHIPDIAG10] WaitGetPoses hook could not find original function");
        return 1;
    }

    const uint64_t wid = wait_id.fetch_add(1) + 1;

    int32_t result = rec->original_wait(
        self,
        render_poses,
        render_pose_count,
        game_poses,
        game_pose_count
    );

    const uint32_t n = log_count.fetch_add(1);
    if (n < MAX_LOGS) {
        API::get()->log_info(
            "[SHIPDIAG10] WAIT id=%llu iface=%s renderCount=%u gameCount=%u result=%d",
            static_cast<unsigned long long>(wid),
            rec->version,
            render_pose_count,
            game_pose_count,
            result
        );
    }

    return result;
}

bool already_hooked_vtable(void** vt) {
    for (int i = 0; i < hook_count; ++i) {
        if (hooks[i].vtable == vt) {
            return true;
        }
    }
    return false;
}

bool patch_slot(void** slot, void* replacement, void** original_out) {
    if (!slot || !*slot || !replacement || !original_out) {
        return false;
    }

    DWORD old_protect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    *original_out = *slot;
    *slot = replacement;

    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    return true;
}

bool hook_interface(void* iface, const char* version) {
    if (!iface || hook_count >= 16) {
        return false;
    }

    auto*** p = reinterpret_cast<void***>(iface);
    if (!p || !*p) {
        return false;
    }

    void** vt = *p;

    if (already_hooked_vtable(vt)) {
        API::get()->log_info(
            "[SHIPDIAG10] %s shares already-hooked vtable=%p",
            version,
            vt
        );
        return true;
    }

    HookRecord rec{};
    rec.vtable = vt;
    std::snprintf(rec.version, sizeof(rec.version), "%s", version);

    void* original_wait{};
    void* original_submit{};

    // IVRCompositor vtable layout:
    // 0 SetTrackingSpace
    // 1 GetTrackingSpace
    // 2 WaitGetPoses
    // 3 GetLastPoses
    // 4 GetLastPoseForTrackedDeviceIndex
    // 5 Submit
    if (!patch_slot(
            &vt[2],
            reinterpret_cast<void*>(&wait_hook),
            &original_wait))
    {
        API::get()->log_error(
            "[SHIPDIAG10] failed to hook WaitGetPoses for %s",
            version
        );
        return false;
    }

    rec.original_wait = reinterpret_cast<WaitGetPosesFn>(original_wait);

    if (!patch_slot(
            &vt[5],
            reinterpret_cast<void*>(&submit_hook),
            &original_submit))
    {
        // Restore WaitGetPoses if Submit patch fails.
        DWORD old_protect{};
        if (VirtualProtect(&vt[2], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
            vt[2] = reinterpret_cast<void*>(rec.original_wait);
            DWORD ignored{};
            VirtualProtect(&vt[2], sizeof(void*), old_protect, &ignored);
        }

        API::get()->log_error(
            "[SHIPDIAG10] failed to hook Submit for %s",
            version
        );
        return false;
    }

    rec.original_submit = reinterpret_cast<SubmitFn>(original_submit);

    hooks[hook_count++] = rec;

    API::get()->log_info(
        "[SHIPDIAG10] hooked %s iface=%p vtable=%p WaitGetPoses=%p Submit=%p",
        version,
        iface,
        vt,
        original_wait,
        original_submit
    );

    return true;
}

void install_openvr_hooks() {
    HMODULE openvr = GetModuleHandleA("openvr_api.dll");

    if (!openvr) {
        API::get()->log_error("[SHIPDIAG10] openvr_api.dll not loaded");
        return;
    }

    auto get_iface = reinterpret_cast<GetGenericInterfaceFn>(
        GetProcAddress(openvr, "VR_GetGenericInterface")
    );

    if (!get_iface) {
        API::get()->log_error("[SHIPDIAG10] VR_GetGenericInterface export not found");
        return;
    }

    API::get()->log_info(
        "[SHIPDIAG10] openvr_api.dll=%p VR_GetGenericInterface=%p",
        openvr,
        reinterpret_cast<void*>(get_iface)
    );

    // Probe recent compositor interface versions because UEVR may have been
    // compiled against a different OpenVR header version than SteamVR itself.
    for (int version = 29; version >= 22; --version) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "IVRCompositor_%03d", version);

        int32_t error = 0;
        void* iface = get_iface(name, &error);

        if (iface && error == 0) {
            hook_interface(iface, name);
        } else {
            API::get()->log_info(
                "[SHIPDIAG10] interface unavailable %s iface=%p error=%d",
                name,
                iface,
                error
            );
        }
    }

    API::get()->log_info(
        "[SHIPDIAG10] install complete: %d distinct compositor vtables hooked",
        hook_count
    );
}

}

class ShipExplorerCompositorDiagV10 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info(
            "[SHIPDIAG10] loaded - raw OpenVR Submit/WaitGetPoses diagnostic; NO camera modification"
        );

        install_openvr_hooks();
    }
};

std::unique_ptr<ShipExplorerCompositorDiagV10> g_plugin{
    new ShipExplorerCompositorDiagV10()
};
