#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <ultramodern/renderer_context.hpp>

namespace RT64 {
struct Application;
}

namespace wr64 {

// Binds RT64 to the renderer interface ultramodern defines but does not
// implement. See src/renderer.cpp for why most of the hardware registers RT64
// asks for are backed by storage owned here rather than by anything real.
class RT64Context final : public ultramodern::renderer::RendererContext {
public:
    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle,
                bool developer_mode);
    ~RT64Context() override;

    bool valid() override;
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override;
    void enable_instant_present() override;
    void send_dl(const OSTask* task) override;
    void send_dummy_workload(uint32_t fb_address) override;
    void update_screen() override;
    void shutdown() override;
    uint32_t get_display_framerate() const override;
    float get_resolution_scale() const override;

private:
    std::unique_ptr<RT64::Application> app_;
    bool valid_ = false;

    // RSP memory and the register block RT64 reads. The DPC and MI registers
    // are inert in a recompilation; they exist so RT64 always has valid storage.
    std::array<uint8_t, 0x1000> dmem_{};
    std::array<uint8_t, 0x1000> imem_{};
    std::array<uint32_t, 8> dpc_regs_{};
    uint32_t mi_intr_reg_ = 0;
};

// Handed to ultramodern as renderer_callbacks.create_render_context.
std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

}  // namespace wr64
