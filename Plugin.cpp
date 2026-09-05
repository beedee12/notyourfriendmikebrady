#include <atomic>
#include <cstdint>
#include <memory>
#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
    std::atomic<uint64_t> tick_id{0};
    std::atomic<uint64_t> viewport_id{0};
    std::atomic<uint64_t> slate_id{0};

    std::atomic<uint32_t> view1_count{0};
    std::atomic<uint32_t> view2_count{0};
    std::atomic<uint32_t> other_view_count{0};

    std::atomic<uint32_t> early_count{0};
    std::atomic<uint32_t> pre_count{0};
    std::atomic<uint32_t> post_count{0};

    std::atomic<uint32_t> detail_logs{0};
    constexpr uint32_t MAX_DETAIL_LOGS = 240;

    void log_detail(const char* stage, int view_index, bool is_double, float w2m) {
        const uint32_t n = detail_logs.fetch_add(1);
        if (n >= MAX_DETAIL_LOGS) {
            return;
        }

        API::get()->log_info(
            "[SHIPDIAG9] %s tick=%llu viewport=%llu slate=%llu view=%d double=%d w2m=%.3f",
            stage,
            static_cast<unsigned long long>(tick_id.load()),
            static_cast<unsigned long long>(viewport_id.load()),
            static_cast<unsigned long long>(slate_id.load()),
            view_index,
            is_double ? 1 : 0,
            w2m
        );
    }

    void early_cb(
        UEVR_StereoRenderingDeviceHandle,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        early_count.fetch_add(1);

        if (view_index == 1) {
            view1_count.fetch_add(1);
        } else if (view_index == 2) {
            view2_count.fetch_add(1);
        } else {
            other_view_count.fetch_add(1);
        }

        log_detail("EARLY", view_index, is_double, world_to_meters);
    }

    void pre_cb(
        UEVR_StereoRenderingDeviceHandle,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        pre_count.fetch_add(1);
        log_detail("PRE", view_index, is_double, world_to_meters);
    }

    void post_cb(
        UEVR_StereoRenderingDeviceHandle,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f*,
        UEVR_Rotatorf*,
        bool is_double)
    {
        post_count.fetch_add(1);
        log_detail("POST", view_index, is_double, world_to_meters);
    }

    void pre_engine_tick_cb(UEVR_UGameEngineHandle, float delta_seconds) {
        const auto id = tick_id.fetch_add(1) + 1;

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] TICK_BEGIN tick=%llu dt=%.6f viewport=%llu",
                static_cast<unsigned long long>(id),
                delta_seconds,
                static_cast<unsigned long long>(viewport_id.load())
            );
        }
    }

    void post_engine_tick_cb(UEVR_UGameEngineHandle, float delta_seconds) {
        const auto id = tick_id.load();

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] TICK_END tick=%llu dt=%.6f viewport=%llu",
                static_cast<unsigned long long>(id),
                delta_seconds,
                static_cast<unsigned long long>(viewport_id.load())
            );
        }
    }

    void pre_viewport_cb(
        UEVR_UGameViewportClientHandle,
        UEVR_FViewportHandle,
        UEVR_FCanvasHandle)
    {
        const auto id = viewport_id.fetch_add(1) + 1;

        view1_count.store(0);
        view2_count.store(0);
        other_view_count.store(0);
        early_count.store(0);
        pre_count.store(0);
        post_count.store(0);

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] VIEWPORT_BEGIN viewport=%llu tick=%llu",
                static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(tick_id.load())
            );
        }
    }

    void post_viewport_cb(
        UEVR_UGameViewportClientHandle,
        UEVR_FViewportHandle,
        UEVR_FCanvasHandle)
    {
        const auto id = viewport_id.load();

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] VIEWPORT_END viewport=%llu tick=%llu views{1=%u 2=%u other=%u} callbacks{early=%u pre=%u post=%u}",
                static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(tick_id.load()),
                view1_count.load(),
                view2_count.load(),
                other_view_count.load(),
                early_count.load(),
                pre_count.load(),
                post_count.load()
            );
        }
    }

    void pre_slate_cb(
        UEVR_FSlateRHIRendererHandle,
        UEVR_FViewportInfoHandle)
    {
        const auto id = slate_id.fetch_add(1) + 1;

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] SLATE_BEGIN slate=%llu viewport=%llu tick=%llu",
                static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(viewport_id.load()),
                static_cast<unsigned long long>(tick_id.load())
            );
        }
    }

    void post_slate_cb(
        UEVR_FSlateRHIRendererHandle,
        UEVR_FViewportInfoHandle)
    {
        const auto id = slate_id.load();

        if (id <= 180) {
            API::get()->log_info(
                "[SHIPDIAG9] SLATE_END slate=%llu viewport=%llu tick=%llu",
                static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(viewport_id.load()),
                static_cast<unsigned long long>(tick_id.load())
            );
        }
    }
}

class ShipExplorerStereoDiagV9 final : public Plugin {
public:
    void on_initialize() override {
        API::get()->log_info("[SHIPDIAG9] loaded - render multiplicity diagnostic; NO camera modification");

        auto* cbs = API::get()->param()->sdk->callbacks;

        cbs->on_early_calculate_stereo_view_offset(&early_cb);
        cbs->on_pre_calculate_stereo_view_offset(&pre_cb);
        cbs->on_post_calculate_stereo_view_offset(&post_cb);

        cbs->on_pre_engine_tick(&pre_engine_tick_cb);
        cbs->on_post_engine_tick(&post_engine_tick_cb);

        cbs->on_pre_viewport_client_draw(&pre_viewport_cb);
        cbs->on_post_viewport_client_draw(&post_viewport_cb);

        cbs->on_pre_slate_draw_window_render_thread(&pre_slate_cb);
        cbs->on_post_slate_draw_window_render_thread(&post_slate_cb);

        API::get()->log_info("[SHIPDIAG9] all diagnostic callbacks registered");
    }
};

std::unique_ptr<ShipExplorerStereoDiagV9> g_plugin{new ShipExplorerStereoDiagV9()};
