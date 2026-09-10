// Phase 03: the RT64-backed renderer context.
//
// ultramodern defines the interface it needs from a renderer but ships no
// implementation, so binding RT64 to it is the project's job. The mapping is
// close: RT64::Application::setup / processDisplayLists / updateScreen / end
// line up almost one-to-one with the interface's virtuals.
//
// RT64 wants a Core struct full of pointers to N64 hardware registers. Only the
// VI registers are real here -- ultramodern owns those and hands them over. The
// DPC and MI registers have no meaning in a recompilation: there is no RDP
// interrupt to service, because the RDP is driven entirely by display lists we
// pass to RT64 directly. They are backed by storage owned by this context so
// RT64 always has somewhere valid to read and write, and checkInterrupts is a
// no-op for the same reason.

#include "wr64/renderer.h"

#include <cstdio>

#if defined(_WIN32)
// RT64 reaches dxcapi.h from here, and that header needs the COM interfaces
// IUnknown and IStream. ultramodern's renderer_context.hpp, included above,
// pulls in Windows.h under WIN32_LEAN_AND_MEAN, which is precisely the macro
// that omits the OLE headers declaring them. RT64's own translation units do
// not hit this because they include Windows.h without that macro first.
#   include <unknwn.h>
#   include <objidl.h>
#endif

#include "hle/rt64_application.h"

namespace {

// Strip the MIPS segment bits from a virtual address to get an RDRAM offset.
// KSEG0 (0x80000000) and KSEG1 (0xA0000000) are both direct-mapped windows onto
// physical memory, so masking is all that is needed here -- the game does not
// use the TLB.
constexpr uint32_t physical(uint32_t vaddr) {
    return vaddr & 0x00FFFFFFu;
}

// RT64 takes a plain function pointer for interrupt notification, so this
// cannot be a lambda with captures.
void no_interrupts() {
    // Nothing to do: see the note at the top of the file.
}

ultramodern::renderer::SetupResult translate(RT64::Application::SetupResult result) {
    using RT = RT64::Application::SetupResult;
    using UM = ultramodern::renderer::SetupResult;
    switch (result) {
        case RT::Success:                  return UM::Success;
        case RT::DynamicLibrariesNotFound: return UM::DynamicLibrariesNotFound;
        case RT::InvalidGraphicsAPI:       return UM::InvalidGraphicsAPI;
        case RT::GraphicsAPINotFound:      return UM::GraphicsAPINotFound;
        case RT::GraphicsDeviceNotFound:   return UM::GraphicsDeviceNotFound;
        default:                           return UM::GraphicsDeviceNotFound;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64_msaa(ultramodern::renderer::Antialiasing aa) {
    using UM = ultramodern::renderer::Antialiasing;
    switch (aa) {
        case UM::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case UM::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case UM::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        default:         return RT64::UserConfiguration::Antialiasing::None;
    }
}

}  // namespace

namespace wr64 {

RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle,
                         bool developer_mode) {
    RT64::Application::Core core{};

    core.window = window_handle.window;
    core.RDRAM = rdram;
    core.DMEM = dmem_.data();
    core.IMEM = imem_.data();
    core.HEADER = nullptr;

    core.MI_INTR_REG = &mi_intr_reg_;

    core.DPC_START_REG    = &dpc_regs_[0];
    core.DPC_END_REG      = &dpc_regs_[1];
    core.DPC_CURRENT_REG  = &dpc_regs_[2];
    core.DPC_STATUS_REG   = &dpc_regs_[3];
    core.DPC_CLOCK_REG    = &dpc_regs_[4];
    core.DPC_BUFBUSY_REG  = &dpc_regs_[5];
    core.DPC_PIPEBUSY_REG = &dpc_regs_[6];
    core.DPC_TMEM_REG     = &dpc_regs_[7];

    // The VI registers are ultramodern's, and the game writes them through the
    // recompiled libultra, so RT64 must read the same storage rather than a copy.
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    core.VI_STATUS_REG         = &vi->VI_STATUS_REG;
    core.VI_ORIGIN_REG         = &vi->VI_ORIGIN_REG;
    core.VI_WIDTH_REG          = &vi->VI_WIDTH_REG;
    core.VI_INTR_REG           = &vi->VI_INTR_REG;
    core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
    core.VI_TIMING_REG         = &vi->VI_TIMING_REG;
    core.VI_V_SYNC_REG         = &vi->VI_V_SYNC_REG;
    core.VI_H_SYNC_REG         = &vi->VI_H_SYNC_REG;
    core.VI_LEAP_REG           = &vi->VI_LEAP_REG;
    core.VI_H_START_REG        = &vi->VI_H_START_REG;
    core.VI_V_START_REG        = &vi->VI_V_START_REG;
    core.VI_V_BURST_REG        = &vi->VI_V_BURST_REG;
    core.VI_X_SCALE_REG        = &vi->VI_X_SCALE_REG;
    core.VI_Y_SCALE_REG        = &vi->VI_Y_SCALE_REG;

    core.checkInterrupts = no_interrupts;

    RT64::ApplicationConfiguration app_config{};
    app_config.appId = "WaveRace64Recomp";
    app_config.useConfigurationFile = true;

    app_ = std::make_unique<RT64::Application>(core, app_config);

    // F1 opens RT64's developer UI, and every path to it -- the key handler, the
    // event filter RT64 installs for itself, State::inspect() -- is gated on
    // this. It is on unconditionally so the debug menu is always one key away;
    // nothing is drawn until F1 is pressed. The argument is kept in the
    // signature because ultramodern's callback carries it.
    (void)developer_mode;
    app_->userConfig.developerMode = true;

    const RT64::Application::SetupResult result = app_->setup(window_handle.thread_id);
    setup_result = translate(result);

    if (result != RT64::Application::SetupResult::Success) {
        // Leave app_ in place: the caller inspects get_setup_result() and the
        // destructor still needs something valid to tear down.
        std::fprintf(stderr, "RT64 setup failed (%d)\n", static_cast<int>(result));
        return;
    }

    chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    valid_ = true;
}

RT64Context::~RT64Context() = default;

bool RT64Context::valid() {
    return valid_;
}

bool RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                const ultramodern::renderer::GraphicsConfig& new_config) {
    if (!valid_) {
        return false;
    }
    if (old_config == new_config) {
        return true;
    }
    app_->userConfig.antialiasing = to_rt64_msaa(new_config.msaa_option);
    app_->updateMultisampling();
    return true;
}

void RT64Context::enable_instant_present() {
    // Not wired up yet. This shortens the present path so a paused frame keeps
    // updating; it affects menu responsiveness rather than whether the game
    // boots, so it is deferred rather than guessed at.
}

void RT64Context::send_dl(const OSTask* task) {
    if (!valid_) {
        return;
    }

    // Boot bring-up tracing: the first display list is the moment the game
    // stops initialising and starts drawing, which is the single most useful
    // event to see during phase 04. Counted, not printed every frame.
    static uint64_t dl_count = 0;
    if (dl_count == 0) {
        std::fprintf(stderr, "[wr64] first display list: ucode 0x%08X data 0x%08X dl 0x%08X\n",
                     task->t.ucode, task->t.ucode_data, task->t.data_ptr);
        std::fflush(stderr);
    }
    ++dl_count;

    // Identify the microcode before submitting anything. RT64 selects its
    // graphics binary interface from the task's ucode text and data addresses,
    // and processDisplayLists asserts that one has been chosen -- it does not
    // do this itself. An emulator would leave the task in DMEM for RT64 to
    // read; a recompilation has no real DMEM, so the addresses are handed over
    // from the OSTask directly.
    app_->interpreter->loadUCodeGBI(physical(task->t.ucode), physical(task->t.ucode_data), true);

    // RT64 indexes straight off the RDRAM base, so it needs a physical address.
    // The OSTask carries virtual KSEG0 addresses -- the display list pointer
    // arrives as 0x801388D0 -- and handing that over unmasked indexes two
    // gigabytes past the 8 MB of RDRAM, which faults immediately.
    //
    // Bisect switch for phase 04: setting WR64_SKIP_DL isolates whether a fault
    // is inside RT64's display list processing or somewhere else entirely.
    static const bool skip_dl = std::getenv("WR64_SKIP_DL") != nullptr;
    if (!skip_dl) {
        app_->processDisplayLists(app_->core.RDRAM, physical(task->t.data_ptr), 0, true);
    }

    if (dl_count == 1) {
        std::fprintf(stderr, "[wr64] send_dl returned from the first display list\n");
        std::fflush(stderr);
    }
}

void RT64Context::send_dummy_workload(uint32_t fb_address) {
    // Used to keep presenting while the game submits no display lists. Boot does
    // not depend on it, so it stays a no-op until phase 04 shows it is needed.
    (void)fb_address;
}

void RT64Context::update_screen() {
    if (valid_) {
        static uint64_t frames = 0;
        // A steady frame count is how we tell "presenting an empty screen" from
        // "stalled before the first present" -- the two look identical to a user
        // and are entirely different problems.
        if (frames == 0 || frames == 60 || frames == 600) {
            std::fprintf(stderr, "[wr64] update_screen #%llu\n",
                         static_cast<unsigned long long>(frames));
            std::fflush(stderr);
        }
        ++frames;
        app_->updateScreen();
    }
}

void RT64Context::shutdown() {
    if (valid_) {
        app_->end();
        valid_ = false;
    }
}

uint32_t RT64Context::get_display_framerate() const {
    if (!valid_) {
        return 60;
    }
    // RT64 does not expose a measured display rate here; the runtime only uses
    // this for pacing, and 60 is right for this game's NTSC target.
    return 60;
}

float RT64Context::get_resolution_scale() const {
    return 1.0f;
}

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

}  // namespace wr64
