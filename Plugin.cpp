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
};

std::unique_ptr<ShipExplorerStereoFixV2> g_plugin{new ShipExplorerStereoFixV2()};
