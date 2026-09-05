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

        // V5 starts from V3's stable mapping:
        //   index 0 -> LEFT
        //   index 1 -> RIGHT
        //
        // But preserve the engine's original classification for index 1 when it
        // already returns a real stereo pass. This is the smallest possible test
        // for recovering the HMD-transformed eye without restoring the duplicate.
        int32_t forced = original;

        if (index == 0) {
            forced = 1; // LEFT, same as V3
        } else if (index == 1) {
            // Keep original only if it is already LEFT/RIGHT. Otherwise force RIGHT.
            forced = (original == 1 || original == 2) ? original : 2;
        } else {
            forced = 0; // reject extra/full passes
        }

        if (logs++ < 120) {
            API::get()->log_info(
                "[SHIPFIX5] index=%u original=%d result=%d",
                index, original, forced
            );
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
            API::get()->log_error("[SHIPFIX5] VirtualProtect failed");
            return;
        }

        original_fn = reinterpret_cast<GetViewPassFn>(target);
        vtable[5] = reinterpret_cast<void*>(&get_view_pass);

        DWORD dummy{};
        VirtualProtect(&vtable[5], sizeof(void*), old, &dummy);
        FlushInstructionCache(GetCurrentProcess(), &vtable[5], sizeof(void*));

        hooked = true;
        API::get()->log_info("[SHIPFIX5] GetViewPassForIndex hooked");
    }

    void stereo_callback(UEVR_StereoRenderingDeviceHandle device, int, float,
                         UEVR_Vector3f*, UEVR_Rotatorf*, bool) {
        install(device);
    }
}

class ShipExplorerStereoFixV5 final : public uevr::Plugin {
public:
    void on_initialize() override {
        API::get()->param()->sdk->callbacks->on_early_calculate_stereo_view_offset(&stereo_callback);
        API::get()->log_info("[SHIPFIX5] loaded; V3 mapping plus selective original pass preservation");
    }
};

std::unique_ptr<ShipExplorerStereoFixV5> g_plugin{new ShipExplorerStereoFixV5()};
