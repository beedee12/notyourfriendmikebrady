#include <windows.h>
#include <cstdint>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
    using GetViewPassFn = int32_t(__fastcall*)(void* self, bool stereo_requested, uint32_t view_index);

    GetViewPassFn g_original_get_view_pass = nullptr;
    void** g_vtable = nullptr;
    bool g_hooked = false;
    int g_log_count = 0;

    int32_t __fastcall hook_get_view_pass(void* self, bool stereo_requested, uint32_t view_index) {
        int32_t original_result = 0;
        if (g_original_get_view_pass != nullptr) {
            original_result = g_original_get_view_pass(self, stereo_requested, view_index);
        }

        if (!stereo_requested) {
            return original_result;
        }

        // Force exactly one canonical Unreal stereo pass per requested eye.
        // EStereoscopicPass: FULL=0, LEFT=1, RIGHT=2.
        int32_t forced = original_result;
        if (view_index == 0) forced = 1;
        else if (view_index == 1) forced = 2;
        else forced = 0;

        if (g_log_count < 80) {
            ++g_log_count;
            API::get()->log_info(
                "[SHIPFIX3] GetViewPass stereo=%d index=%u original=%d forced=%d",
                stereo_requested ? 1 : 0, view_index, original_result, forced
            );
        }

        return forced;
    }

    void install_get_view_pass_hook(UEVR_StereoRenderingDeviceHandle device) {
        if (g_hooked || device == nullptr) return;

        auto*** obj = reinterpret_cast<void***>(device);
        if (obj == nullptr || *obj == nullptr) return;

        g_vtable = *obj;

        // Ship Explorer / this UEVR build identified GetViewPassForIndex at vtable index 5.
        void* target = g_vtable[5];
        if (target == nullptr) return;

        DWORD old_protect{};
        if (!VirtualProtect(&g_vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
            API::get()->log_error("[SHIPFIX3] VirtualProtect failed; GetViewPass hook not installed");
            return;
        }

        g_original_get_view_pass = reinterpret_cast<GetViewPassFn>(target);
        g_vtable[5] = reinterpret_cast<void*>(&hook_get_view_pass);

        DWORD dummy{};
        VirtualProtect(&g_vtable[5], sizeof(void*), old_protect, &dummy);
        FlushInstructionCache(GetCurrentProcess(), &g_vtable[5], sizeof(void*));

        g_hooked = true;
        API::get()->log_info("[SHIPFIX3] hooked GetViewPassForIndex at vtable index 5");
    }

    void on_early_stereo_offset(
        UEVR_StereoRenderingDeviceHandle device,
        int,
        float,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool)
    {
        install_get_view_pass_hook(device);
    }
}

class ShipExplorerStereoFixV3 final : public uevr::Plugin {
public:
    void on_initialize() override {
        auto param = API::get()->param();

        // Register the SDK callback directly. It gives us the actual stereo device pointer.
        param->sdk->callbacks->on_early_calculate_stereo_view_offset(&on_early_stereo_offset);

        API::get()->log_info("[SHIPFIX3] loaded; waiting for stereo device");
        API::get()->log_info("[SHIPFIX3] no HMD/eye/camera transforms are modified");
    }
};

std::unique_ptr<ShipExplorerStereoFixV3> g_plugin{new ShipExplorerStereoFixV3()};
