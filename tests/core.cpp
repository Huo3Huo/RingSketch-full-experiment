#include "oracle_support.h"
#include "Algos/RingSketch/RingSketchDualPointer.h"
#include "Protocol.h"
void optimizedSimdTests() {
    std::mt19937 random(136);
    uint64_t compared=0;
    // Arbitrary valid counter states expose packing, mixed stages, crossings,
    // saturation, all pointer phases, and four-counter tails independently of keys.
    for(int w:{4,8,12,16,20,64,68,128}) for(int threshold:{1,1984,4094})
    for(auto speed:std::vector<std::pair<int,int>>{{3,1},{w+1,2},{8*w-1,w/4},{500*w+3,w/2}}) {
        const Dual::Config c{w,12,threshold,8192,speed.first,speed.second};
        Dual simd(c),scalar(c);
        for(int round=0;round<100;++round) {
            const int t=round<8?round:int(random()%1000000);
            DualPointerTestAccess::time(simd,t);DualPointerTestAccess::time(scalar,t);
            for(int j=0;j<w;++j) {
                const int choices[]={0,1,4095,threshold,std::min(4095,threshold+1),std::min(4095,threshold+8),int(random()%4096)};
                const int value=choices[(j+round)%7];
                DualPointerTestAccess::set(simd,j,value);DualPointerTestAccess::set(scalar,j,value);
            }
            simd.updateClock();scalar.updateClockScalar();
            for(int j=0;j<w;++j){require(simd.counter(j)==scalar.counter(j),"12-bit SIMD arbitrary-state mismatch");++compared;}
            require(simd.time()==scalar.time() && simd.fastPointer()==scalar.fastPointer() && simd.slowPointer()==scalar.slowPointer(),"SIMD clock mismatch");
        }
    }
    // Independent event oracle for 12-bit SIMD (including threshold coincidences).
    for(int threshold:{1,1984,4094}) {
        const Dual::Config c{16,12,threshold,8192,127,4};
        Dual simd(c);Events events(c);
        for(int j=0;j<c.width;++j){int v=std::min(4095,threshold+j-4);v=std::max(0,v);DualPointerTestAccess::set(simd,j,v);events.values[j]=v;}
        for(int t=0;t<100;++t){simd.updateClock();events.advance(1);compare(simd,events);}
    }
    // Real streams: query equivalence, coarse ages, expiry and repeated collisions.
    const auto config=Dual::forBudget(192,12,1984,4);
    Dual simd(config),scalar(config);
    simd.insert(key(1),0);scalar.insert(key(1),0);
    for(int t=0;t<10000;++t) {
        if(t%17==0){auto x=key(2+random()%32);simd.insert(x,t);scalar.insert(x,t);}
        simd.updateClock();scalar.updateClockScalar();
        for(int j=0;j<config.width;++j){require(simd.counter(j)==scalar.counter(j),"SIMD stream state mismatch");++compared;}
        if(t%23==0) for(int k=1;k<35;++k) {
            const auto a=RingSketchTrajectory::query(simd,key(k)),b=RingSketchTrajectory::query(scalar,key(k));
            require(a.estimate==b.estimate && a.ageLower==b.ageLower && a.ageUpper==b.ageUpper && a.overwrittenMask==b.overwrittenMask && a.fine==b.fine && a.emptyOrExpired==b.emptyOrExpired,"SIMD trajectory query mismatch");
        }
    }
    // Production widths exercise full vector blocks and every actual tail layout.
    for(int memory:Protocol::Memories) {
        const auto c=Dual::forBudget(memory,12,1984,4);Dual a(c),b(c);
        require(a.payloadBytes()==size_t(memory) && a.allocatedPayloadBytes()==size_t(memory),"SIMD added payload");
        for(int t=0;t<300;++t) {
            auto x=key(random()%512);a.insert(x,t);b.insert(x,t);
            a.updateClock();b.updateClockScalar();
            for(int j=0;j<c.width;++j){require(a.counter(j)==b.counter(j),"production SIMD mismatch");++compared;}
        }
        a.updateClock(300);b.updateClockScalar(300);
        for(int t=0;t<20;++t){a.updateClock();b.updateClockScalar();}
        for(int j=0;j<c.width;++j)require(a.counter(j)==b.counter(j),"SIMD/batch fallback mismatch");
    }
    Dual limit(config);DualPointerTestAccess::time(limit,std::numeric_limits<int>::max());
    bool rejected=false;try{limit.updateClock();}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"SIMD logical time overflow accepted");
    std::cout<<"PASS optimized SIMD/scalar counters="<<compared<<", independent event oracle, trajectory queries, payload and tails\n";
}
int main() {
    try {
        scalarSimdTest(); packedAndBoundaries(); updateExhaustive(); inverseExhaustive(); collisionAndRandom();optimizedSimdTests();
        uint64_t forward=0,states=0,mixed=0;
        for(int w:{4,8,12,16}) for(int bits:{3,4,5}) {
            const int k=(1<<bits)-1;
            std::vector<std::pair<int,int>> speeds{{3,1},{7,2},{8*w-1,std::max(1,w/4)}};
            for(int theta=1;theta<k;++theta) for(auto speed:speeds) {
                const Dual::Config cfg{w,bits,theta,8192,speed.first,speed.second};
                for(int t=0;t<=24;++t) {
                    Dual state(cfg);DualPointerTestAccess::time(state,t);
                    for(int r=0;r<=t;++r) for(int j=0;j<w;++j) {
                        require(RingSketchTrajectory::counterFromWrite(state,j,r)==resetOracle(cfg,j,r,t),
                                "optimized forward trajectory disagrees with independent event oracle");
                        ++forward;
                    }
                }
            }
        }
        for(int memory:Protocol::Memories) {
            const auto cfg=Dual::forBudget(memory,12,1984,4);const auto p=positions(key(88),cfg.width);
            Dual s(cfg);const int k=s.maximum();
            for(int r:{0,1,2,3}) for(int age=1;age<=8192;++age) {
                const int t=r+age;DualPointerTestAccess::time(s,t);int lo=k,hi=0;bool fine=false,slow=false,live=true;
                for(int j:p){const int c=RingSketchTrajectory::counterFromWrite(s,j,r);DualPointerTestAccess::set(s,j,c);live=live&&c>0;lo=std::min(lo,c);hi=std::max(hi,c);fine|=c>cfg.threshold;slow|=c<=cfg.threshold;}
                if(!live) continue;
                require(hi-lo<=1,"production no-collision counters violated Min/Min+1");
                const auto q=RingSketchTrajectory::query(s,key(88));
                require(q.ageLower<=age && age<=q.ageUpper,"optimized production no-collision bounds exclude true age");
                require(q.estimate>=q.ageLower && q.estimate<=q.ageUpper,"optimized production no-collision estimate outside bounds");
                ++states;if(fine&&slow)++mixed;
            }
        }
        require(states>0,"no production-like no-collision states reached in optimized sweep");
        std::cout<<"PASS optimized forward="<<forward<<" no_collision_states="<<states<<" mixed_stage="<<mixed<<"\nALL CHECKS PASSED\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
