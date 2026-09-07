#include "wr64/haptics.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace wr64::haptics;
static bool zero(MotorLevels v) { return v.low==0 && v.high==0 && v.left_trigger==0 && v.right_trigger==0; }
static uint64_t count(const Mixer& m, Event e) { return m.counts()[size_t(e)]; }
static Sample race() {
    Sample s; s.active=true; s.racing=true; s.go=true; s.slot=0; s.course=1;
    s.dt=.05f; s.speed=80; s.wet=1; s.throttle=1; return s;
}
static void advance(Mixer& m, Sample& s, double& time) { ++s.tick; time+=s.dt; m.submit(s,time); }
static Mixer events_only() { Mixer m; m.configure({Mode::Impacts,1,0,true}); return m; }

static void events() {
    auto m=events_only(); auto s=race(); double t=10; m.submit(s,t);
    s.buoy=1; s.buoy_success=true; advance(m,s,t);
    assert(count(m,Event::Buoy)==1);
    auto buoy=m.output(t+.02); assert(buoy.high>buoy.low && buoy.high>.1);
    m.submit(s,t+.025); assert(count(m,Event::Buoy)==1); // duplicated draw
    for(int i=0;i<8;i++) advance(m,s,t);
    assert(count(m,Event::Buoy)==1 && zero(m.output(t+.02))); // latched outcome
    s.buoy=2; s.misses=1; advance(m,s,t);
    assert(count(m,Event::Miss)==1 && count(m,Event::Buoy)==1);
    auto miss=m.output(t+.02); assert(miss.low>miss.high);
    for(int i=0;i<10;i++) advance(m,s,t);
    s.power=5; advance(m,s,t); assert(count(m,Event::Power)==1);
    for(int i=0;i<10;i++) advance(m,s,t);
    s.buoy=3; advance(m,s,t); assert(count(m,Event::Buoy)==2); // max power pass
    s.lap=1; advance(m,s,t); assert(count(m,Event::Lap)==0);
    s.lap=2; advance(m,s,t); assert(count(m,Event::Lap)==1);
    s.finished=true; advance(m,s,t); assert(count(m,Event::Finish)==1);
    for(int i=0;i<20;i++) advance(m,s,t);
    assert(count(m,Event::Finish)==1 && zero(m.output(t+.02)));
}
static void physics() {
    auto m=events_only(); auto s=race(); double t=10; m.submit(s,t);
    s.airborne=true; s.wet=0; advance(m,s,t);
    s.airborne=false; s.wet=1; advance(m,s,t);
    assert(count(m,Event::Landing)==0); // one short dry sample is chop
    s.airborne=true; s.wet=0; advance(m,s,t);
    s.vertical_speed=-70; advance(m,s,t); advance(m,s,t);
    s.airborne=false; s.wet=1; advance(m,s,t);
    assert(count(m,Event::Landing)==1);
    auto landing=m.output(t+.025); assert(landing.low>.3 && landing.high>0);
    s.collision=true; s.impact=20; advance(m,s,t);
    assert(count(m,Event::Collision)==1);
    for(int i=0;i<8;i++) advance(m,s,t);
    assert(count(m,Event::Collision)==1); // sustained scraping is one hit
    s.crashed=true; advance(m,s,t); assert(count(m,Event::Crash)==1);
    s.airborne=true; s.vertical_speed=-900;
    for(int i=0;i<12;i++) advance(m,s,t);
    s.crashed=false; s.airborne=false; advance(m,s,t);
    assert(count(m,Event::Crash)==1 && count(m,Event::Landing)==1);
    assert(zero(m.output(t+.02))); // recovery teleport must be silent
}
static void countdown_and_stops() {
    auto m=events_only(); auto s=race(); s.racing=s.go=false; s.countdown=-1;
    double t=10; m.submit(s,t);
    for(int cue=3;cue>0;cue--) {
        s.countdown=cue; advance(m,s,t);
        for(int i=0;i<14;i++) advance(m,s,t);
    }
    assert(count(m,Event::Countdown)==3);
    s.go=s.racing=true; s.countdown=0; advance(m,s,t);
    assert(count(m,Event::Go)==1 && m.output(t+.02).low>0);
    assert(zero(m.output(t+.03,false))); // menu/focus stop
    advance(m,s,t); m.output(t+.001,true); advance(m,s,t);
    assert(zero(m.output(t+.02))); // no deferred GO after resume
    s.misses=1; advance(m,s,t); assert(!zero(m.output(t+.02)));
    s.paused=true; advance(m,s,t); assert(zero(m.output(t+.02)));
    s.paused=false; advance(m,s,t); assert(zero(m.output(t+.02)));
    s.misses=2; advance(m,s,t); assert(!zero(m.output(t+.02)));
    assert(zero(m.output(t+.25))); // hung guest cannot sustain rumble
    m.submit(s,t+.26); assert(zero(m.output(t+.27))); // repeated stale draw cannot revive it
    advance(m,s,t); assert(zero(m.output(t+.02)));
    s.retired=true; advance(m,s,t); assert(count(m,Event::Retire)==1);
    s.course=2; advance(m,s,t); assert(zero(m.output(t+.02)));
    s.slot=1; advance(m,s,t); assert(zero(m.output(t+.02)));
    s.tick=0; t+=.05; m.submit(s,t); assert(zero(m.output(t+.02)));
}
static void settings_and_cadence() {
    Mixer m; auto s=race(); double t=10; m.submit(s,t);
    assert(zero(m.output(t+.01))); // Default is events only: no continuous bed.
    m.configure({Mode::Full,.8f,.35f,true});
    advance(m,s,t);
    float max_low=0;
    for(int i=0;i<50;i++) {
        advance(m,s,t); auto v=m.output(t+.005); max_low=std::max(max_low,v.low);
        assert(v.low>=0 && v.low<=1 && v.high>=0 && v.high<=1);
    }
    assert(max_low>0 && max_low<.3); // restrained propulsion bed
    m.configure({Mode::Impacts,.8f,1,true}); advance(m,s,t);
    assert(zero(m.output(t+.01)));
    m.configure({Mode::Off,1,1,true}); s.buoy=1; s.buoy_success=true; advance(m,s,t);
    assert(zero(m.output(t+.01)));
    m.configure({Mode::Full,0,1,true}); advance(m,s,t); assert(zero(m.output(t+.01)));
    m.configure({Mode::Full,1,1,false}); advance(m,s,t); advance(m,s,t);
    auto v=m.output(t+.01); assert(v.left_trigger==0 && v.right_trigger==0);
    s.throttle=0; s.speed=0;
    for(int i=0;i<20;i++) { advance(m,s,t); v=m.output(t+.01); }
    assert(v.low<.0001 && v.high<.0001);
    s.speed=80; s.throttle=1; s.airborne=true;
    for(int i=0;i<20;i++) { advance(m,s,t); v=m.output(t+.01); }
    assert(v.low<.0001 && v.high<.0001);
    s.speed=s.wet=s.impact=s.vertical_speed=std::numeric_limits<float>::quiet_NaN();
    advance(m,s,t); v=m.output(t+.01); assert(std::isfinite(v.low) && std::isfinite(v.high));
    assert(zero(m.output(std::numeric_limits<double>::quiet_NaN())));
    // Motor envelopes depend on elapsed time, not how often they are sampled.
    auto slow=events_only(), fast=events_only(); s=race(); t=10; slow.submit(s,t); fast.submit(s,t);
    s.tick++; s.collision=true; s.impact=22; t+=s.dt; slow.submit(s,t); fast.submit(s,t);
    for(int i=1;i<=20;i++) {
        const double now=t+i*.005;
        auto a=fast.output(now);
        if(i%4==0) { auto b=slow.output(now); assert(std::abs(a.low-b.low)<.00001 && std::abs(a.high-b.high)<.00001); }
    }
    auto half=events_only(); half.configure({Mode::Impacts,.5f,0,true});
    s=race(); half.submit(s,20); s.tick=1; s.collision=true; s.impact=22;
    half.submit(s,20.05);
    auto full=events_only(); s.tick=0; s.collision=false; full.submit(s,20);
    s.tick=1; s.collision=true; full.submit(s,20.05);
    auto a=full.output(20.08), b=half.output(20.08);
    assert(std::abs(a.low*.5f-b.low)<.00001 && std::abs(a.left_trigger*.5f-b.left_trigger)<.00001);
}
static void decoder() {
    std::vector<uint8_t> ram(8*1024*1024);
    auto w=[&](uint32_t a,int32_t v){std::memcpy(ram.data()+(a&0x7fffff),&v,4);};
    auto h=[&](uint32_t a,uint16_t v){std::memcpy(ram.data()+((a&0x7fffff)^2),&v,2);};
    auto f=[&](uint32_t a,float v){std::memcpy(ram.data()+(a&0x7fffff),&v,4);};
    w(0x800DAB24,0x28); w(0x800DAB28,1); w(0x801982F0,4); w(0x800D48DC,2);
    w(0x800D8170,1); w(0x801CE648,3); w(0x801CE650,2); h(0x801CE624,0xffff);
    w(0x80228D08,1); w(0x80228A90,15); w(0x800D461C,3);
    const auto p=0x80192690+2*0x1718, r=0x801C2938+2*0x378;
    f(p+0xB90,50); f(p+0xB7C,-20); h(p+0xC66,0x8000); w(p+0xB60,-40);
    w(p+0x28,3); w(p+0xC78,2); h(p+0xC7C,1); f(p+0xC40,12);
    w(r+0xC,9); w(r+0x28,1); w(r+0x134,2); w(r+0x12C,5);
    auto before=ram; auto s=observe(ram.data()); assert(ram==before);
    assert(s.active && s.go && s.racing && s.slot==2 && !s.paused && s.countdown==0);
    assert(s.speed==90 && s.vertical_speed==-20 && s.throttle==1 && s.steering==-.5f);
    assert(s.wet==.5f && !s.airborne && s.collision && s.impact==12);
    assert(s.buoy==9 && s.buoy_success && s.power==5 && s.misses==2);
    h(p+0x1608,5); assert(observe(ram.data()).crashed);
    h(p+0x1608,0); w(p+0xC54,0x17); assert(observe(ram.data()).crashed);
    w(p+0xC54,7); w(p+0xC58,55); assert(observe(ram.data()).crashed);
    w(p+0xC58,56); assert(!observe(ram.data()).crashed);
    auto finish_mixer=events_only(); finish_mixer.submit(observe(ram.data()),10);
    w(0x80151960,1); w(0x800DAB24,0x2A); w(r+0x2F4,1);
    auto terminal=observe(ram.data());
    assert(terminal.active && terminal.finished && !terminal.racing);
    finish_mixer.submit(terminal,10.05);
    assert(count(finish_mixer,Event::Finish)==1 && !zero(finish_mixer.output(10.08)));
    w(0x80151960,2); finish_mixer.submit(observe(ram.data()),10.10);
    assert(!zero(finish_mixer.output(10.12)));
    w(r+0x2F4,0); assert(!observe(ram.data()).active);
    w(0x800DAB24,0x29); w(r+0x2EC,1);
    terminal=observe(ram.data()); assert(terminal.active && terminal.retired && !terminal.racing);
    w(0x800DAB24,0x34); assert(!observe(ram.data()).active);
    w(0x800DAB24,0x28); w(r+0x2EC,0);
    h(0x801CE624,0); assert(observe(ram.data()).paused);
    w(0x800DAB24,7); assert(!observe(ram.data()).active); // attract/AI demo
    w(0x800DAB24,0x28); w(0x800D48DC,-1); assert(!observe(ram.data()).active);
    w(0x800D48DC,4); assert(!observe(ram.data()).active);
    assert(!observe(nullptr).active);
}
int main() {
    events(); physics(); countdown_and_stops(); settings_and_cadence(); decoder();
    std::cout<<"Haptics event, cadence, stop, settings and read-only human-slot decoding checks passed.\n";
}
