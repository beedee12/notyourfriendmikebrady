#include <windows.h>
#include <cstdint>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
    using GetViewPassFn = int32_t(__fastcall*)(void*, bool, uint32_t);
    GetViewPassFn original_fn = nullptr;
    void** vtable = nullptr;
    bool hooked = false;
    int logs = 0;

    int32_t __fastcall get_view_pass(void* self, bool stereo, uint32_t index) {
        const int32_t original = original_fn ? original_fn(self, stereo, index) : 0;
        if (!stereo) return original;

        // V4: same V3 experiment, but swap the two eye assignments.
        // Unreal EStereoscopicPass: FULL=0, LEFT=1, RIGHT=2.
        int32_t forced = original;
        if (index == 0) forced = 2;      // RIGHT
        else if (index == 1) forced = 1; // LEFT
        else forced = 0;                 // extra/full pass

        if (logs++ < 80) {
            API::get()->log_info("[SHIPFIX4] index=%u original=%d forced=%d",
                                 index, original, forced);
        }
        return forced;
    }

    void install(UEVR_StereoRenderingDeviceHandle device) {
        if (hooked || !device) return;
        auto*** object = reinterpret_cast<void***>(device);
        if (!object || !*object) return;
        vtable = *object;

        void* target = vtable[5];
        if (!target) return;

        DWORD old{};
        if (!VirtualProtect(&vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            API::get()->log_error("[SHIPFIX4] VirtualProtect failed");
            return;
        }

        original_fn = reinterpret_cast<GetViewPassFn>(target);
        vtable[5] = reinterpret_cast<void*>(&get_view_pass);

        DWORD dummy{};
        VirtualProtect(&vtable[5], sizeof(void*), old, &dummy);
        FlushInstructionCache(GetCurrentProcess(), &vtable[5], sizeof(void*));
        hooked = true;

        API::get()->log_info("[SHIPFIX4] GetViewPassForIndex hooked; eyes SWAPPED");
    }

    void stereo_callback(UEVR_StereoRenderingDeviceHandle device, int, float,
                         UEVR_Vector3f*, UEVR_Rotatorf*, bool) {
        install(device);
    }
}

class ShipExplorerStereoFixV4 final : public uevr::Plugin {
public:
    void on_initialize() override {
        API::get()->param()->sdk->callbacks->on_early_calculate_stereo_view_offset(&stereo_callback);
        API::get()->log_info("[SHIPFIX4] loaded; waiting for stereo device");
    }
};

std::unique_ptr<ShipExplorerStereoFixV4> g_plugin{new ShipExplorerStereoFixV4()};
