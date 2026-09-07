// Geometry contract check, runnable without the game or ROM.
// c++ -std=c++17 -Iinclude tools/verify_water_shore.cpp -o /tmp/water-shore-check
#include "wr64/water_shore.h"
#include <cassert>
#include <limits>
#include <iostream>

int main() {
    using namespace wr64::water;
    // Wall triangle: (0,-10,0), (0,10,0), (0,10,20).
    // At y=0 its waterline is x=0, 0<=z<=10.
    const float diagonal=std::sqrt(0.5f);
    const std::array<float,16> wall={0,-10,0, 1,0,0, 0,0,-1,
        0,-diagonal,diagonal, 0,1,0, -20};
    std::array<float,4> s;
    assert(shoreline_segment(wall,0,s));
    assert(std::abs(s[0])<1e-4 && std::abs(s[2])<1e-4);
    assert(std::abs(std::min(s[1],s[3]))<1e-4);
    assert(std::abs(std::max(s[1],s[3])-10)<1e-4);
    assert(std::abs(shoreline_distance_squared(3,5,s)-9)<1e-4);
    assert(std::abs(shoreline_distance_squared(3,14,s)-25)<1e-4);
    assert(shoreline_segment(wall,5,s));
    assert(std::abs(std::max(s[1],s[3])-15)<1e-4);
    assert(!shoreline_segment(wall,15,s));
    assert(!shoreline_segment(wall,-15,s));
    auto invalid=wall; invalid[3]=std::numeric_limits<float>::quiet_NaN();
    assert(!shoreline_segment(invalid,0,s));
    invalid=wall; invalid[3]=0; invalid[4]=1;
    assert(!shoreline_segment(invalid,0,s));
    std::cout << "Shoreline triangle, level changes, distance and invalid-data checks passed\n";
}
