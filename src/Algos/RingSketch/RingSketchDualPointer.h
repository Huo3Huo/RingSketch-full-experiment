#ifndef RING_SKETCH_TRAJECTORY_H
#define RING_SKETCH_TRAJECTORY_H

#include "RingSketchDualPointerState.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace RingSketchTrajectory {
using Sketch = RingSketchDualPointer;
using I128 = __int128;

inline I128 floorDiv(I128 n,I128 d) { I128 q=n/d,r=n%d; return q-(r<0); }
inline I128 ceilDiv(I128 n,I128 d) { return -floorDiv(-n,d); }

inline int counterFromWrite(const Sketch& s,int j,int64_t r) {
    const auto& c=s.config(); const I128 w=c.width,vf=c.fastSpeed,vs=c.slowSpeed;
    const int h=s.maximum()-c.threshold; const I128 now=s.time();
    const I128 first=ceilDiv(I128(r)*vf-I128(j),w);
    const I128 last=ceilDiv(now*vf-I128(j),w);
    const I128 fastVisits=last-first;
    if(fastVisits<h) return s.maximum()-int(fastVisits);
    const I128 crossing=I128(j)+(first+h-1)*w;
    const I128 firstSlow=floorDiv(crossing*vs-I128(j)*vf,w*vf)+1;
    const I128 endSlow=ceilDiv(now*vs-I128(j),w);
    return int(std::max<I128>(0,I128(c.threshold)-(endSlow-firstSlow)));
}

inline Sketch::QueryResult query(const Sketch& s,TUPLES key) {
    auto out=s.query(key);
    if(out.emptyOrExpired || out.fine) return out;
    const auto p=s.getPos(key); std::array<Sketch::WriteInterval,4> iv;
    int64_t lo=s.time(),hi=0; int observedMin=s.maximum();
    for(int i=0;i<4;++i) {
        observedMin=std::min(observedMin,s.counter(p[i])); iv[i]=s.intervalForBucket(p[i]);
        if(!iv[i].valid()) return out;
        lo=std::min(lo,iv[i].lo); hi=std::max(hi,iv[i].hi);
    }
    if(lo>hi || hi-lo>128) return out;
    int64_t bestWrite=-1; int bestSupport=-1; int bestDistance=std::numeric_limits<int>::max();
    for(int64_t r=lo;r<=hi;++r) {
        const int age=int(s.time()-r); if(age<out.ageLower || age>out.ageUpper) continue;
        std::array<int,4> e; int emin=s.maximum(),emax=-1;
        for(int i=0;i<4;++i){e[i]=counterFromWrite(s,p[i],r);emin=std::min(emin,e[i]);emax=std::max(emax,e[i]);}
        if(emax-emin>1 || observedMin<emin || observedMin>emax) continue;
        int support=0,exactMin=s.maximum(),exactMax=-1; bool hasMin=false,contradiction=false;
        for(int i=0;i<4;++i) {
            const int actual=s.counter(p[i]);
            if(actual==e[i]) {++support;exactMin=std::min(exactMin,actual);exactMax=std::max(exactMax,actual);hasMin|=actual==emin;}
            if(actual<e[i]) {contradiction=true;break;}
        }
        if(contradiction || !hasMin || exactMax-exactMin>1) continue;
        const int distance=std::abs(age-out.estimate);
        if(support>bestSupport || (support==bestSupport && distance<bestDistance)
                || (support==bestSupport && distance==bestDistance && r<bestWrite))
            {bestWrite=r;bestSupport=support;bestDistance=distance;}
    }
    if(bestSupport>=2) out.estimate=int(s.time()-bestWrite);
    return out;
}
} // namespace RingSketchTrajectory

#endif
