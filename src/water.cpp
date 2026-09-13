#include "wr64/water.h"
#include "wr64/water_shore.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <array>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <stdexcept>
#include <set>
#include <vector>
#include "json/json.hpp"
#include <librecomp/game.hpp>

namespace wr64::water {
namespace {
// Modern (the menu's Enhanced) and the Modern style (the menu's Deep) by default,
// with clarity at 85% and Aqua brightness at 80%: the look chosen for release.
// It is not free -- see docs/WATER.md -- and the Water tab steps it up to Best or
// down to Original, which is a true bypass.
//
// With saved settings, the config's Load callback runs before the first frame
// and overwrites whatever is here. On a first run there is no water.json and no
// callback, so these apply -- keep them equal to src/frontend.cpp's defaults.
std::atomic<uint32_t> selected{uint32_t(Quality::Modern)};
std::atomic<uint32_t> selectedStyle{uint32_t(Style::Modern)};
std::atomic<float> aquaBrightness{0.8f}, aquaTint{0.5f}, aquaClarity{0.85f};
std::atomic<uint32_t> selectedRipples{uint32_t(RippleDetail::Normal)};
std::atomic<bool> selectedSpray{true};
std::atomic<uint32_t> debugView{0};
std::atomic<bool> comparisonFlip{false};
std::atomic<bool> raceResetRequested{false};
interop::WaterMaterial frame{};
uint32_t frameRipples=uint32_t(RippleDetail::Normal);
Style frameStyle=Style::Modern;
float frameBrightness=0.8f, frameTint=0.5f, frameClarity=0.85f;
bool frameOnePlayer=true;
struct PendingFrame { uint32_t list; interop::WaterMaterial material; bool onePlayer; };
std::deque<PendingFrame> pendingFrames;
std::mutex frameMutex;
bool initialized = false;
uint32_t previousTick = 0, previousCourse = ~0u, generation = 0;
float currentTime = 0, previousTime = 0;
uint64_t visualTicks = 0;
std::array<interop::float4,4> previousCraft;
std::array<interop::WaterMaterial,10> profiles;

// The sun editor's state (see water.h). Written by the F1 panel on RT64's thread
// and read here on the game thread, so everything is behind one small lock.
std::mutex sunMutex;
std::array<interop::float4,kCourses> sunOverride{};
std::array<bool,kCourses> sunOverridden{};
std::atomic<bool> profilesReady{false};
std::atomic<uint32_t> lastCourse{~0u};
std::atomic<float> headingX{0.0f}, headingZ{0.0f};
std::atomic<bool> headingValid{false};

// gCameraPerspective and the index of the camera in use; +0x64 and +0x6C are the
// camera's horizontal heading, the vector func_8006E674 culls buoys against.
constexpr uint32_t kCameraIndex=0x80223930, kCameraBase=0x80227C80, kCameraStride=0x10C;

std::filesystem::path sun_path() {
    return recomp::get_config_path() / "water_sun.json";
}

bool valid_sun(const interop::float4 &s) {
    auto in=[](float v){ return std::isfinite(v) && v>=-4.0f && v<=4.0f; };
    return in(s.x) && in(s.y) && in(s.z) && in(s.w) && s.w>=0.0f && s.x*s.x+s.y*s.y+s.z*s.z>=0.01f;
}

// water_sun.json: the same fields assets/water/profiles.json uses, for the
// courses that have an override. Read once, after the profiles; a bad entry is
// skipped and reported rather than rejecting the rest.
void load_sun_overrides() {
    std::ifstream file(sun_path());
    if (!file) return;
    try {
        nlohmann::json doc; file >> doc;
        uint32_t loaded=0;
        std::lock_guard lock(sunMutex);
        for (const auto &course:doc.at("courses")) {
            const uint32_t id=course.at("id").get<uint32_t>();
            const auto &a=course.at("sun_direction");
            if (id>=kCourses || !a.is_array() || a.size()!=4) continue;
            interop::float4 s{a.at(0).get<float>(),a.at(1).get<float>(),a.at(2).get<float>(),a.at(3).get<float>()};
            if (!valid_sun(s)) {
                std::fprintf(stderr,"[water] water_sun.json: course %u's sun is out of range; ignored\n",id);
                continue;
            }
            sunOverride[id]=s;
            sunOverridden[id]=true;
            ++loaded;
        }
        std::fprintf(stderr,"[water] sun overrides from water_sun.json: %u course(s)\n",loaded);
    } catch (const std::exception &error) {
        std::fprintf(stderr,"[water] water_sun.json ignored: %s\n",error.what());
    }
}

void load_profiles() {
    interop::WaterMaterial base{};
    base.deepColor={0.008f,0.10f,0.15f,0.003f};
    base.shallowColor={0.03f,0.44f,0.38f,0.19f};
    base.sunDirection={-0.45f,0.75f,0.48f,2};
    base.sunColor={1,0.91f,0.75f,1};
    base.skyColor={0.12f,0.36f,0.66f,0.36f};
    base.animation={0,0,1,0.5f}; base.surface={0,0.9f,0.3f,0};
    base.optics={66,0,0.12f,0};
    base.effects={1,0,0,0};
    profiles.fill(base);
    const char *overridePath=std::getenv("WR64_WATER_PROFILES");
    try {
        std::filesystem::path filename="assets/water/profiles.json";
#ifdef __APPLE__
        if (std::filesystem::exists("../Resources/assets/water/profiles.json")) filename="../Resources/assets/water/profiles.json";
#endif
        if (overridePath) filename=overridePath;
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("cannot open "+filename.string());
        nlohmann::json doc; file >> doc;
        if (doc.at("schema_version") != 1 || doc.at("courses").size() != profiles.size()) throw std::runtime_error("expected schema 1 and ten courses");
        auto candidate=profiles;
        std::array<bool,10> seen{};
        for (const auto &course:doc.at("courses")) {
            const uint32_t id=course.at("id").get<uint32_t>();
            if (id>=profiles.size() || seen[id]) throw std::runtime_error("invalid or duplicate course ID");
            seen[id]=true;
            auto read=[&](const char *name,interop::float4 &value,float minimum,float maximum) {
                const auto &a=course.at(name);
                if (!a.is_array() || a.size()!=4) throw std::runtime_error("expected four material components");
                for (uint32_t i=0;i<4;i++) {
                    const float v=a.at(i).get<float>();
                    if (!std::isfinite(v) || v<minimum || v>maximum) throw std::runtime_error("material component outside supported range");
                    value[i]=v;
                }
            };
            auto &m=candidate[id];
            read("deep_color",m.deepColor,0,2); read("shallow_color",m.shallowColor,0,2);
            read("sun_direction",m.sunDirection,-4,4); read("sun_color",m.sunColor,0,4); read("sky_color",m.skyColor,0,2);
            if (m.deepColor.w<=0 || m.shallowColor.w<0.065f || m.sunDirection.w<0 || m.sunDirection.x*m.sunDirection.x+m.sunDirection.y*m.sunDirection.y+m.sunDirection.z*m.sunDirection.z<0.01f) throw std::runtime_error("invalid absorption, roughness or sun direction");
            auto scalar=[&](const nlohmann::json &value,float minimum,float maximum) {
                const float v=value.get<float>();
                if (!std::isfinite(v)) throw std::runtime_error("non-finite material component");
                return std::clamp(v,minimum,maximum);
            };
            m.optics.x=scalar(course.value("visibility_range",std::clamp(0.20f/m.deepColor.w,35.0f,140.0f)),10.0f,300.0f);
            m.optics.y=scalar(course.value("authored_reflection",0.0f),0.0f,1.0f);
            m.optics.z=scalar(course.value("caustics",0.0f),0.0f,0.5f);
            m.animation.z=scalar(course.at("detail"),0.0f,2.0f);
            m.animation.w=scalar(course.at("foam"),0.0f,1.0f);
            if (!course.at("wind").is_array() || course.at("wind").size()!=2) throw std::runtime_error("expected two wind components");
            m.surface.y=scalar(course.at("wind").at(0),-2.0f,2.0f);
            m.surface.z=scalar(course.at("wind").at(1),-2.0f,2.0f);
        }
        profiles=candidate;
        std::fprintf(stderr,"[water] loaded ten course profiles\n");
    } catch (const std::exception &error) {
        std::fprintf(stderr,"[water] using default profile: %s\n",error.what());
    }
}

uint32_t word(const uint8_t *rdram, uint32_t address) {
    uint32_t result; std::memcpy(&result, rdram + (address & 0x7fffff), 4); return result;
}
float real(const uint8_t *rdram,uint32_t address) {
    float result; std::memcpy(&result,rdram+(address & 0x7fffff),4); return result;
}
int16_t half(const uint8_t *rdram, uint32_t address) {
    int16_t result; std::memcpy(&result, rdram + ((address & 0x7fffff) ^ 2), 2); return result;
}

void capture_shoreline(const uint8_t *rdram, interop::WaterMaterial &m, bool reset) {
    static uint32_t savedPlanes=0, savedList=0, savedGrid=0, savedCourse=~0u;
    static float savedHeight=0;
    static std::vector<std::array<float,4>> segments;
    const uint32_t course=uint32_t(m.identity.z);
    const uint32_t planes=word(rdram,0x801C3B84), list=word(rdram,0x801C3B80), grid=word(rdram,0x801C3B7C);
    if (reset || course!=savedCourse || planes!=savedPlanes || list!=savedList || grid!=savedGrid || m.surface.x!=savedHeight) {
        segments.clear();
        savedPlanes=planes; savedList=list; savedGrid=grid; savedCourse=course; savedHeight=m.surface.x;
        // Course 9 is rider selection. Only accept the original per-course
        // pointer tables, after the original loader has installed the data.
        const bool known=course<9 &&
            (planes==word(rdram,0x800D6B4C+course*4) || planes==word(rdram,0x800D6C0C+course*4));
        auto valid=[](uint32_t address,uint32_t bytes) {
            return address>=0x80306800 && address<=0x80316800 && bytes<=0x80316800-address;
        };
        const uint32_t extentX=word(rdram,0x801C3B4C),extentZ=word(rdram,0x801C3B58);
        const uint32_t shiftX=word(rdram,0x801C3B70),shiftZ=word(rdram,0x801C3B78);
        const uint32_t strideX=word(rdram,0x801C3B60),strideZ=word(rdram,0x801C3B68);
        if (known && valid(planes,64) && valid(list+2,2) && valid(grid,2) && list+2<planes &&
            extentX>0 && extentZ>0 && extentX<=65536 && extentZ<=65536 &&
            shiftX>=6 && shiftX<=12 && shiftZ>=6 && shiftZ<=12 && strideX<=10 && strideZ<=10) {
            std::set<uint16_t> shapes;
            const bool transpose=half(rdram,0x801C3B40)!=0;
            const uint32_t columns=(extentX+(1u<<shiftX)-1)>>shiftX,rows=(extentZ+(1u<<shiftZ)-1)>>shiftZ;
            for (uint32_t z=0;z<rows;z++) for (uint32_t x=0;x<columns;x++) {
                const uint32_t index=transpose ? (z | (x<<strideZ)) : (x | (z<<strideX));
                const uint32_t cell=grid+index*2;
                if (!valid(cell,2) || cell>=list+2) continue;
                uint32_t entry=list+uint16_t(half(rdram,cell))*2+2;
                for (uint32_t count=0;count<512 && entry>=list+2 && entry<planes;count++,entry+=2) {
                    const uint16_t id=uint16_t(half(rdram,entry));
                    if (!id) break;
                    if (id<=1024 && valid(planes+(id-1)*64,64)) shapes.insert(id-1);
                }
            }
            for (uint16_t id:shapes) {
                std::array<float,16> plane;
                for (uint32_t i=0;i<16;i++) plane[i]=real(rdram,planes+id*64+i*4);
                std::array<float,4> segment;
                if (shoreline_segment(plane,m.surface.x,segment)) segments.push_back(segment);
            }
        }
        std::fprintf(stderr,"[water] course %u shoreline: %zu static segments at height %.1f\n",course,segments.size(),m.surface.x);
    }
    // Bound transport and compute cost. Selection depends on craft world
    // positions, never a camera or screen-depth sample. Emission fades well
    // inside the tile; retained foam decays when a segment leaves the set.
    for (uint32_t tile=0;tile<4;tile++) {
        if (m.craftCurrent[tile].w<0) continue;
        std::vector<std::pair<float,size_t>> nearby;
        for (size_t i=0;i<segments.size();i++) {
            const auto &c=m.craftCurrent[tile];
            const float d=shoreline_distance_squared(c.x,c.z,segments[i]);
            if (d<1600*1600) nearby.emplace_back(d,i);
        }
        std::sort(nearby.begin(),nearby.end());
        const uint32_t count=std::min(size_t(WATER_SHORE_SEGMENTS_PER_TILE),nearby.size());
        m.shoreCounts[tile]=float(count);
        for (uint32_t i=0;i<count;i++) {
            const auto &s=segments[nearby[i].second];
            m.shoreSegments[tile*WATER_SHORE_SEGMENTS_PER_TILE+i]={s[0],s[1],s[2],s[3]};
        }
    }
}
}

Quality quality() {
    const uint32_t value=selected.load();
    return static_cast<Quality>(comparisonFlip.load() ? (value ? 0 : 1) : value);
}
void set_quality(Quality value) { selected.store(std::min(uint32_t(value), 3u)); comparisonFlip.store(false); }
void set_style(Style value) { selectedStyle.store(std::min(uint32_t(value), 2u)); }
void set_aqua_brightness(float percent) { aquaBrightness.store(std::isfinite(percent) ? std::clamp(percent / 100.0f, 0.0f, 1.0f) : 0.5f); }
void set_aqua_tint(float percent) { aquaTint.store(std::isfinite(percent) ? std::clamp(percent / 100.0f, 0.0f, 1.0f) : 0.5f); }
void set_clarity(float percent) { aquaClarity.store(std::isfinite(percent) ? std::clamp(percent / 100.0f, 0.0f, 1.0f) : 0.5f); }
const char* course_name(uint32_t course) {
    static const char* const names[kCourses]={"Dolphin Park","Sunny Beach","Sunset Bay","Marine Fortress",
        "Drake Lake","Port Blue","Twilight City","Southern Island","Glacier Coast","Rider Selection"};
    return course<kCourses ? names[course] : "no course";
}
uint32_t current_course() { return lastCourse.load(); }
bool profiles_loaded() { return profilesReady.load(); }
Sun profile_sun(uint32_t course) {
    const interop::float4 s=profiles[std::min(course,kCourses-1)].sunDirection;
    return {s.x,s.y,s.z,s.w};
}
bool sun_override(uint32_t course, Sun *out) {
    if (course>=kCourses) return false;
    std::lock_guard lock(sunMutex);
    if (!sunOverridden[course]) return false;
    const interop::float4 &s=sunOverride[course];
    *out={s.x,s.y,s.z,s.w};
    return true;
}
void set_sun_override(uint32_t course, const Sun &sun) {
    if (course>=kCourses) return;
    const interop::float4 s{sun.x,sun.y,sun.z,sun.strength};
    if (!valid_sun(s)) return;
    std::lock_guard lock(sunMutex);
    sunOverride[course]=s;
    sunOverridden[course]=true;
}
void clear_sun_override(uint32_t course) {
    if (course>=kCourses) return;
    std::lock_guard lock(sunMutex);
    sunOverridden[course]=false;
}
bool camera_heading(float *x, float *z) {
    if (!headingValid.load()) return false;
    *x=headingX.load(); *z=headingZ.load();
    return true;
}
// Writes every override in effect -- the ones loaded at startup included -- so the
// file always says exactly what the game will use next time. A course reverted to
// its profile is left out.
bool save_sun_overrides(std::string &status) {
    nlohmann::json doc;
    doc["schema_version"]=1;
    doc["courses"]=nlohmann::json::array();
    uint32_t count=0;
    {
        std::lock_guard lock(sunMutex);
        for (uint32_t id=0;id<kCourses;id++) {
            if (!sunOverridden[id]) continue;
            const interop::float4 &s=sunOverride[id];
            doc["courses"].push_back({{"id",id},{"name",course_name(id)},{"sun_direction",{s.x,s.y,s.z,s.w}}});
            ++count;
        }
    }
    const std::filesystem::path path=sun_path();
    std::ofstream out(path);
    if (!out) { status="could not write "+path.string(); return false; }
    out << doc.dump(2) << "\n";
    status="saved "+std::to_string(count)+" course(s) to water_sun.json";
    return true;
}

void set_ripple_detail(RippleDetail value) { selectedRipples.store(std::min(uint32_t(value), 2u)); }
void set_spray_enabled(bool enabled) { selectedSpray.store(enabled); }
void toggle() {
    comparisonFlip.store(!comparisonFlip.load());
    const char *names[]={"Original","Modern","High","Ultra"};
    std::fprintf(stderr, "[water] comparison: %s\n", names[uint32_t(quality())]);
}
void cycle_debug() { debugView.store((debugView.load()+1)%15); }
void reset_for_race() { raceResetRequested.store(true); }

void publish_frame(const uint8_t *rdram, uint32_t display_list) {
    if (!initialized) {
        load_profiles();
        profilesReady.store(true);
        load_sun_overrides();
        if (const char *v = std::getenv("WR64_WATER")) {
            selected.store(std::strcmp(v,"ultra")==0 ? 3 : std::strcmp(v,"high")==0 ? 2 :
                (std::strcmp(v,"modern")==0 || std::strcmp(v,"1")==0) ? 1 : 0);
        }
        if (const char *v = std::getenv("WR64_WATER_STYLE")) {
            set_style(std::strcmp(v,"classic")==0 ? Style::Classic :
                std::strcmp(v,"aqua")==0 ? Style::Aqua : Style::Modern);
        }
        if (const char *v = std::getenv("WR64_WATER_RIPPLES")) {
            set_ripple_detail(std::strcmp(v,"soft")==0 ? RippleDetail::Soft :
                std::strcmp(v,"strong")==0 ? RippleDetail::Strong : RippleDetail::Normal);
        }
        if (const char *v = std::getenv("WR64_WATER_SPRAY")) {
            set_spray_enabled(std::strcmp(v,"off")!=0 && std::strcmp(v,"0")!=0);
        }
        if (const char *v = std::getenv("WR64_WATER_DEBUG")) debugView.store(std::clamp(std::atoi(v),0,14));
        // Percentages, as the sliders store them. For comparing captures of one
        // setting against another without touching the saved settings.
        if (const char *v = std::getenv("WR64_WATER_CLARITY")) set_clarity(float(std::atof(v)));
        if (const char *v = std::getenv("WR64_WATER_BRIGHTNESS")) set_aqua_brightness(float(std::atof(v)));
        if (const char *v = std::getenv("WR64_WATER_TINT")) set_aqua_tint(float(std::atof(v)));
        const char *rippleNames[]={"Soft","Normal","Strong"};
        const char *styleNames[]={"Modern","Classic","Aqua"};
        std::fprintf(stderr,"[water] appearance: %s, %s ripples, spray %s, clarity %.0f%%, brightness %.0f%%, tint %.0f%%\n",
            styleNames[selectedStyle.load()],rippleNames[selectedRipples.load()],
            selectedSpray.load() ? "On" : "Off",aquaClarity.load()*100,aquaBrightness.load()*100,aquaTint.load()*100);
        initialized = true;
    }
    const uint32_t tick = word(rdram, 0x80151960);
    static const uint32_t compareTick=[]() {
        const char *text=std::getenv("WR64_TEST_WATER_COMPARE_TICK");
        if (!text) return 0u;
        char *end=nullptr; const unsigned long value=std::strtoul(text,&end,10);
        return end!=text && *end=='\0' && value<=UINT32_MAX ? uint32_t(value) : 0u;
    }();
    static bool compared=false;
    if (compareTick && !compared && tick>=compareTick) {
        compared=true;
        comparisonFlip.store(true);
        std::fprintf(stderr,"[water] test comparison at tick %u: Original\n",tick);
    }
    const uint32_t course = word(rdram, 0x800D8170);
    const uint32_t divider = std::clamp(word(rdram,0x800D461C),1u,3u);
    const bool paused = half(rdram, 0x801CE624) != -1;
    const bool reset = raceResetRequested.exchange(false) || course != previousCourse || tick < previousTick || tick-previousTick > 10;
    if (reset) {
        currentTime = previousTime = 0;
        visualTicks = 0;
        ++generation;
        std::fprintf(stderr,"[water] visual generation %u at game tick %u, course %u\n",generation,tick,course);
    }
    else if (tick != previousTick) {
        previousTime = currentTime;
        if (!paused) visualTicks += uint64_t(tick-previousTick) * divider;
        currentTime = float(double(visualTicks) / 60.0);
    }
    previousTick = tick; previousCourse = course;
    interop::WaterMaterial frame = profiles[std::min(course,9u)];
    if (course<kCourses) {
        std::lock_guard lock(sunMutex);
        if (sunOverridden[course]) frame.sunDirection=sunOverride[course];
    }
    lastCourse.store(course);
    {
        const uint32_t camera=kCameraBase+(word(rdram,kCameraIndex)&3)*kCameraStride;
        const float fx=real(rdram,camera+0x64), fz=real(rdram,camera+0x6C);
        const float length=std::sqrt(fx*fx+fz*fz);
        const bool ok=std::isfinite(length) && length>0.0001f;
        if (ok) { headingX.store(fx/length); headingZ.store(fz/length); }
        headingValid.store(ok);
    }
    frame.animation.x = previousTime; frame.animation.y = currentTime;
    frame.identity = {float(uint32_t(quality())),float(debugView.load()),float(course),float(generation)};
    frame.surface.x = float(int32_t(word(rdram,0x80192458)));
    // The authored lake reflection was reviewed in the one-player fog path.
    // Do not apply that compatibility input to a different original layout.
    const bool onePlayer=word(rdram,0x800DAB28)==1;
    if (!onePlayer) frame.optics.y=0;
    const uint32_t count=std::min(word(rdram,0x801982F0),4u);
    for (uint32_t i=0;i<4;i++) {
        interop::float4 craft{0,0,0,-1};
        if (i<count) {
            const uint32_t base=0x80192690+i*0x1718;
            craft={real(rdram,base+0x44),real(rdram,base+0x48),real(rdram,base+0x4c),0};
            // The original wave and flat-water solvers set bit 0 in each
            // contact point's +0x1c flags. +0x28 is the last point index.
            const uint32_t points=word(rdram,base+0x30) & 0x7fffff;
            const uint32_t last=word(rdram,base+0x28);
            if (points>=0x1000 && last<32 && points+(last+1)*0x20<=0x800000) {
                uint32_t wet=0;
                for (uint32_t j=0;j<=last;j++) wet += (word(rdram,points+j*0x20+0x1c)&1) != 0;
                craft.w=float(wet)/float(last+1);
            }
            if (!std::isfinite(craft.x) || !std::isfinite(craft.y) || !std::isfinite(craft.z) || std::abs(craft.x)>1000000 || std::abs(craft.z)>1000000) craft={0,0,0,-1};
        }
        const auto &old=previousCraft[i];
        const float dx=craft.x-old.x,dz=craft.z-old.z;
        const bool discontinuity=reset || old.w<0 || dx*dx+dz*dz>1024*1024;
        frame.craftCurrent[i]=craft;
        frame.craftPrevious[i]=discontinuity ? craft : old;
        previousCraft[i]=craft;
    }
    capture_shoreline(rdram,frame,reset);
    static FILE *trace=[]() {
        const char *name=std::getenv("WR64_WATER_TRACE");
        FILE *f=name ? std::fopen(name,"w") : nullptr;
        if (f) std::fprintf(f,"tick,state,mode,course,seed,rider_hash,x,y,z,wet,divider\n");
        return f;
    }();
    if (trace && tick%30==0) {
        uint64_t hash=14695981039346656037ull;
        for (uint32_t offset=0;offset<count*0x1718;offset++) hash=(hash^rdram[0x192690+offset])*1099511628211ull;
        const auto &c=frame.craftCurrent[0];
        std::fprintf(trace,"%u,%u,%u,%u,%u,%016llx,%.6f,%.6f,%.6f,%.6f,%u\n",tick,word(rdram,0x800DAB24),word(rdram,0x801CE620),course,word(rdram,0x800D4640),
            static_cast<unsigned long long>(hash),c.x,c.y,c.z,c.w,divider);
        std::fflush(trace);
    }
    std::lock_guard lock(frameMutex);
    pendingFrames.push_back({display_list & 0x7fffff,frame,onePlayer});
    // The game double-buffers lists. The bound also covers skipped render tasks.
    while (pendingFrames.size() > 16) pendingFrames.pop_front();
}

void begin_frame(uint32_t display_list) {
    std::lock_guard lock(frameMutex);
    const auto match = std::find_if(pendingFrames.begin(),pendingFrames.end(),[&](const auto &p) {
        return p.list == (display_list & 0x7fffff);
    });
    frame = {};
    if (match == pendingFrames.end()) return; // Unknown tasks retain original water.
    frame = match->material;
    frameOnePlayer = match->onePlayer;
    pendingFrames.erase(pendingFrames.begin(),std::next(match));
    frame.identity.x = frame.identity.z <= 9 ? float(uint32_t(quality())) : 0;
    frame.identity.y = float(debugView.load());
    // All water draws in this display list must agree on resource needs, even
    // if the menu applies a new preference while the list is being rewritten.
    frameStyle = static_cast<Style>(selectedStyle.load());
    frameBrightness = aquaBrightness.load();
    frameTint = aquaTint.load();
    frameClarity = aquaClarity.load();
    // This renderer field is a boolean selecting the original raster base,
    // not the menu enum. Aqua must take the complete Modern shading path.
    frame.optics.w = frameStyle == Style::Classic ? 1.0f : 0.0f;
    frameRipples = selectedRipples.load();
    frame.effects.x = selectedSpray.load() ? 1.0f : 0.0f;
}

interop::WaterMaterial material(uint32_t view) {
    static const bool trace = std::getenv("WR64_WATER_MATERIAL_TRACE") != nullptr;
    auto result = frame;
    result.surface.w = float(view);
    // Only the numbers the trace prints, not a copy of the material: this runs
    // once per water draw per frame, and the material is 1584 bytes.
    const interop::float4 beforeDeep = result.deepColor;
    const interop::float4 beforeShallow = result.shallowColor;
    const float beforeVisibility = result.optics.x;
    const float beforeSeeThrough = result.optics.y;
    // Keep the published course profile immutable. Each view/draw applies the
    // current preference once, so repeated draws cannot amplify the ripples.
    constexpr float rippleScales[]={0.5f,1.0f,1.7f};
    result.animation.z *= rippleScales[frameRipples];
    if (frameStyle == Style::Aqua) {
        // Lift the course's own palette toward clear aqua in linear light.
        // Scaling retains sunset/night lighting instead of painting every
        // course tropical blue. Roughness, normals and all effects stay intact.
        result.deepColor.x *= 2.0f;
        result.deepColor.y *= 1.85f;
        result.deepColor.z *= 1.35f;
        result.shallowColor.x *= 1.7f;
        result.shallowColor.y *= 1.30f;
        result.shallowColor.z *= 1.22f;
        // Both sliders are centered on the reviewed Aqua look. Tint shifts
        // blue toward turquoise without altering the reflected sky/scenery.
        //
        // The ranges are wide on purpose. They scale only the water body's own
        // colour, which the shader then darkens by the lighting, hides behind
        // the refracted bottom and the reflection, and compresses in tone
        // mapping and gamma: the first ranges here, 0.65-1.35 and +-12-20%,
        // came out as roughly +-15% and +-5% on screen, which read as a
        // slider that does nothing. Brightness is exponential so that 50% is
        // unchanged and each end is the same number of stops away.
        const float brightness = std::pow(2.5f, frameBrightness * 2.0f - 1.0f);  // 0.4 to 2.5
        const float tint = frameTint * 2.0f - 1.0f;
        for (auto *color : {&result.deepColor, &result.shallowColor}) {
            color->x *= brightness * (1.0f - tint * 0.55f);
            color->y *= brightness * (1.0f + tint * 0.30f);
            color->z *= brightness * (1.0f - tint * 0.30f);
        }
    }
    if (frameStyle != Style::Classic) {
        // Clarity, for the two styles that shade the water themselves. Classic
        // keeps the cartridge's own transparency and its shader reads none of
        // this, so the slider is hidden there.
        //
        // 50% is each style's reviewed look. Aqua's clearer shallows are a
        // fixed step on top of Modern: absorption x0.6, visibility x1.4.
        float absorption = frameStyle == Style::Aqua ? 0.6f : 1.0f;
        float visibility = frameStyle == Style::Aqua ? 1.4f : 1.0f;
        const float s = frameClarity - 0.5f;
        if (s < 0) {
            // Murk. Absorption has to rise steeply to show at all: over the
            // ~50 units of water usually in view, the profiles' absorption lets
            // three quarters of the red through, so doubling it is barely seen.
            absorption *= std::pow(6.0f, -2.0f * s);                    // up to x6
            visibility *= 1.0f + 1.2f * s;                              // down to x0.4
        } else {
            absorption *= 1.0f - 1.33f * s;                             // down to x1/3
            visibility *= 1.0f + 0.57f * s;                             // up to x1.29
        }
        result.deepColor.w *= absorption;
        // Keep the finite visibility fade: beyond the authored underwater
        // geometry the shader must still resolve to water, never exposed sky.
        result.optics.x = std::min(result.optics.x * visibility, std::max(beforeVisibility, 140.0f));
        // The upper half shows what the game drew under the surface -- the
        // sea floor, fish, the course's own water colour -- by raising the
        // profile's authored_reflection weight toward 1. Absorption and the
        // visibility range alone cannot do that: the shader lights and
        // darkens the refracted bottom as part of the water body, which is
        // why the water stays opaque-looking however far they are pushed.
        // See docs/WATER.md, "Clarity".
        //
        // One player only, like the profile's own value. In two-player races
        // the cartridge draws its water opaque, so what lies "under" the
        // surface is that flat water, and blending toward it flattens away
        // the ripples and lighting -- tried on a two-player race and seen.
        const float seeThrough = std::clamp(frameClarity * 2.0f - 1.0f, 0.0f, 1.0f);
        if (frameOnePlayer) result.optics.y += (1.0f - result.optics.y) * seeThrough;
    }
    // What the renderer is actually being handed, which is the only way to tell
    // "the style does nothing" from "the style never reaches the renderer" --
    // the two look identical on screen. See docs/WATER.md.
    if (trace) {
        static int shown = 0;
        if (shown < 3) {
            ++shown;
            const char *names[] = {"Modern","Classic","Aqua"};
            std::fprintf(stderr,
                "[water-trace] quality=%u style=%s opticsW=%.1f "
                "deep %.4f %.4f %.4f a=%.4f -> %.4f %.4f %.4f a=%.4f  "
                "shallow %.4f %.4f %.4f -> %.4f %.4f %.4f  vis %.1f -> %.1f  see-through %.2f -> %.2f  "
                "sun %.3f %.3f %.3f %.2f\n",
                uint32_t(quality()), names[uint32_t(frameStyle)], result.optics.w,
                beforeDeep.x, beforeDeep.y, beforeDeep.z, beforeDeep.w,
                result.deepColor.x, result.deepColor.y, result.deepColor.z, result.deepColor.w,
                beforeShallow.x, beforeShallow.y, beforeShallow.z,
                result.shallowColor.x, result.shallowColor.y, result.shallowColor.z,
                beforeVisibility, result.optics.x, beforeSeeThrough, result.optics.y,
                result.sunDirection.x, result.sunDirection.y, result.sunDirection.z, result.sunDirection.w);
            std::fflush(stderr);
        }
    }
    return result;
}
}
