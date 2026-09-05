#include <memory>
#include "uevr/Plugin.hpp"
using namespace uevr;

class ShipExplorerStereoFixV2 final : public uevr::Plugin {
public:
    void on_initialize() override {
        auto vr = API::get()->param()->vr;
        vr->set_mod_value("VR_NativeStereoFix", "false");
        vr->set_mod_value("VR_NativeStereoFixSamePass", "false");
        vr->set_mod_value("VR_Compatibility_SplitScreen", "false");
        API::get()->log_info("[SHIPFIX2] loaded: SamePass OFF, NativeStereoFix OFF, SplitScreen OFF");
        API::get()->log_info("[SHIPFIX2] no manual HMD transform");
    }
    void on_early_calculate_stereo_view_offset(
        UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters,
        UEVR_Vector3f*, UEVR_Rotatorf*, bool is_double) override {
        if (m_logs++ < 40)
            API::get()->log_info("[SHIPFIX2] view=%d double=%d w2m=%.3f",
                view_index, is_double ? 1 : 0, world_to_meters);
    }
private:
    int m_logs{};
};
std::unique_ptr<ShipExplorerStereoFixV2> g_plugin{new ShipExplorerStereoFixV2()};
