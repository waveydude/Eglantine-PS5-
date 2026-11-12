// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <EGL/egl.h>
#include <atomic>
#include <future>
#include <functional>
#include <string>
#include "core/frontend/emu_window.h"

namespace VideoCore { class RendererBase; }
namespace Core { class System; }
namespace Eden {
void ToggleHud();
class GraphicsWindow final : public Core::Frontend::EmuWindow {
public:
    explicit GraphicsWindow(bool use_vulkan = false);
    ~GraphicsWindow() override;
#ifdef EDEN_GPU_PROBE
    void RunGpuProbe();
#endif
    void CheckPresentation(Core::Frontend::GraphicsContext& context);
    void SetSystem(Core::System& value) { system = &value; }
    // The next frame, lossless, to logs/<file_name> (PPM). The file is written on a thread of its
    // own; the future turns true when it is there.
    std::shared_future<bool> CaptureNextFrame(VideoCore::RendererBase& renderer,
                                            std::function<void()> on_complete,
                                            std::string file_name = "game-frame.ppm");
    bool IsShown() const override { return true; }
    void OnFrameDisplayed() override;
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
private:
    bool vulkan{};
    bool splash_hidden{};
    bool first_frame_reported{};
    double frame_sample_start{-1}, frame_sample_last{}, frame_sample_worst{}, frame_sample_worst_end{};
    unsigned frame_sample_count{}, frame_total{};
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
    EGLContext root{EGL_NO_CONTEXT};
    mutable std::atomic<bool> window_context{};
    EGLSurface surface{EGL_NO_SURFACE};
    Core::System* system{};
};
}
