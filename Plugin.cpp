#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
    int early_logs = 0;
    int pre_logs = 0;
    int post_logs = 0;

    void early_cb(
        UEVR_StereoRenderingDeviceHandle device,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        if (early_logs++ < 80) {
            API::get()->log_info(
                "[SHIPDIAG7] EARLY fired device=%p view=%d double=%d w2m=%.3f",
                device, view_index, is_double ? 1 : 0, world_to_meters
            );
        }
    }

    void pre_cb(
        UEVR_StereoRenderingDeviceHandle device,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        if (pre_logs++ < 80) {
            API::get()->log_info(
                "[SHIPDIAG7] PRE fired device=%p view=%d double=%d w2m=%.3f",
                device, view_index, is_double ? 1 : 0, world_to_meters
            );
        }
    }

    void post_cb(
        UEVR_StereoRenderingDeviceHandle device,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        if (post_logs++ < 80) {
            API::get()->log_info(
                "[SHIPDIAG7] POST fired device=%p view=%d double=%d w2m=%.3f",
                device, view_index, is_double ? 1 : 0, world_to_meters
            );
        }
    }
}

class ShipExplorerStereoDiagV7 final : public uevr::Plugin {
public:
    void on_initialize() override {
        auto callbacks = API::get()->param()->sdk->callbacks;

        API::get()->log_info("[SHIPDIAG7] plugin initialized");
        API::get()->log_info("[SHIPDIAG7] registering EARLY/PRE/POST stereo callbacks");

        callbacks->on_early_calculate_stereo_view_offset(&early_cb);
        callbacks->on_pre_calculate_stereo_view_offset(&pre_cb);
        callbacks->on_post_calculate_stereo_view_offset(&post_cb);

        API::get()->log_info("[SHIPDIAG7] registration complete");
    }
};

std::unique_ptr<ShipExplorerStereoDiagV7> g_plugin{new ShipExplorerStereoDiagV7()};
