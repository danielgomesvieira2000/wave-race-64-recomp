#pragma once
#include <array>
#include <cmath>
#include <algorithm>

namespace wr64::water {
// Intersect the original bounded collision plane with the mean water plane.
// The four half-space tests are those used by func_8007F448. Work in doubles
// here because near-horizontal beaches produce poorly conditioned divisions.
inline bool shoreline_segment(const std::array<float,16> &p, float waterY,
                              std::array<float,4> &result) {
    for (float v:p) if (!std::isfinite(v)) return false;
    const double norm=p[3]*p[3]+p[4]*p[4]+p[5]*p[5];
    const double horizontal=p[3]*p[3]+p[5]*p[5];
    if (norm<0.9 || norm>1.1 || horizontal<1e-10 || p[15]>=0) return false;
    const double dy=double(p[1])-waterY;
    const double dx=-p[3]*p[4]*dy/horizontal, dz=-p[5]*p[4]*dy/horizontal;
    const double tx=-p[5]/std::sqrt(horizontal), tz=p[3]/std::sqrt(horizontal);
    double low=-1e6, high=1e6;
    for (int side=0;side<4;side++) {
        const int j=side==0 ? 6 : side==1 ? 9 : 12;
        const double sign=side==2 ? -1 : 1;
        const double bound=side==3 ? p[15] : 0;
        const double a=sign*(p[j]*tx+p[j+2]*tz);
        const double b=sign*(p[j]*dx+p[j+1]*dy+p[j+2]*dz-bound);
        if (std::abs(a)<1e-9) { if (b<0) return false; }
        else if (a>0) low=std::max(low,-b/a);
        else high=std::min(high,-b/a);
    }
    if (high-low<0.1 || std::abs(low)>100000 || std::abs(high)>100000) return false;
    result={float(p[0]-dx-tx*low),float(p[2]-dz-tz*low),
            float(p[0]-dx-tx*high),float(p[2]-dz-tz*high)};
    for (float v:result) if (!std::isfinite(v) || std::abs(v)>100000) return false;
    return true;
}
inline float shoreline_distance_squared(float x,float z,const std::array<float,4> &s) {
    const float dx=s[2]-s[0],dz=s[3]-s[1];
    const float t=std::clamp(((x-s[0])*dx+(z-s[1])*dz)/std::max(dx*dx+dz*dz,0.001f),0.0f,1.0f);
    const float ex=x-s[0]-t*dx,ez=z-s[1]-t*dz;
    return ex*ex+ez*ez;
}
}
