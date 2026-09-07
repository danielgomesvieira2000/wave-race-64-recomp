#include "wr64/haptics.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace wr64::haptics {
namespace {
std::mutex mutex;
Mixer mixer;
Settings settings;
ControllerOutput controller_output;
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void configure() {
    // QA comparison only; no persistent settings are changed by this override.
    static const bool force_off = [] { const char* p=std::getenv("WR64_HAPTICS"); return p && std::strcmp(p,"off")==0; }();
    auto value = settings;
    if (force_off) value.mode = Mode::Off;
    mixer.configure(value);
}
struct Trace {
    FILE* observations = nullptr;
    FILE* output = nullptr;
    Trace() {
        if (const char* path = std::getenv("WR64_HAPTICS_TRACE"); path && *path) {
            observations = std::fopen(path,"w");
            output = std::fopen((std::string(path)+".output.csv").c_str(),"w");
            if (observations) std::fprintf(observations,"tick,active,paused,slot,racing,countdown,speed,vertical,wet,airborne,impact,crashed,buoy,success,misses,power,lap,finished,retired,events\n");
            if (output) std::fprintf(output,"time,tick,allowed,low,high,left_trigger,right_trigger\n");
        }
    }
    ~Trace() { if (observations) std::fclose(observations); if (output) std::fclose(output); }
};
Trace& trace() { static Trace value; return value; }
}
void set_mode(Mode mode) { std::lock_guard lock(mutex); settings.mode=mode; configure(); }
void set_strength(double percent) { std::lock_guard lock(mutex); settings.strength=float(percent/100); configure(); }
void set_ambience(double percent) { std::lock_guard lock(mutex); settings.ambience=float(percent/100); configure(); }
void set_triggers(bool enabled) { std::lock_guard lock(mutex); settings.triggers=enabled; configure(); }
void reset_for_race() { std::lock_guard lock(mutex); mixer.reset(); }
void capture_frame(const uint8_t* rdram) {
    const Sample s = observe(rdram);
    std::lock_guard lock(mutex);
    const auto before = mixer.counts();
    mixer.submit(s, now_seconds());
    auto& t = trace();
    static uint32_t traced_tick = ~0u;
    if (t.observations && traced_tick != s.tick) {
        traced_tick = s.tick;
        std::string events;
        for (size_t i=0;i<event_count;i++) if (mixer.counts()[i] != before[i]) {
            if (!events.empty()) events+='|';
            events+=event_name(static_cast<Event>(i));
        }
        std::fprintf(t.observations,"%u,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
            s.tick,s.active,s.paused,s.slot,s.racing,s.countdown,s.speed,s.vertical_speed,s.wet,s.airborne,s.impact,s.crashed,
            s.buoy,s.buoy_success,s.misses,s.power,s.lap,s.finished,s.retired,events.c_str());
        std::fflush(t.observations);
    }
}
void update_output(SDL_GameController* controller, bool allowed) {
    const double now = now_seconds();
    MotorLevels levels;
    {
        std::lock_guard lock(mutex);
        levels = mixer.output(now, allowed);
        auto& t = trace();
        static double last_trace = -100;
        if (t.output && now-last_trace >= kOutputIntervalSeconds) {
            last_trace = now;
            std::fprintf(t.output,"%.6f,%u,%d,%.5f,%.5f,%.5f,%.5f\n",now,mixer.observation().tick,allowed,levels.low,levels.high,levels.left_trigger,levels.right_trigger);
        }
    }
    // Own both SDL rumble channels only here, on the native main loop. The
    // transport refreshes at 125 Hz and expires after 50 ms if this loop stalls.
    controller_output.update(controller,levels,allowed,now);
}
void shutdown(SDL_GameController* controller) {
    controller_output.stop(controller);
    std::lock_guard lock(mutex);
    mixer.reset();
}
}
