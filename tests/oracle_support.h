#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "Algos/RingSketch/RingSketch.h"
#include "Algos/RingSketch/RingSketchSimd.h"
#include "Algos/RingSketch/RingSketchDualPointerState.h"
#pragma pack()

using Dual = RingSketchDualPointer;
struct DualPointerTestAccess {
    static void set(Dual& s,int j,int value) { s.write(j,value); }
    static void time(Dual& s,int64_t t) { s.now_=t; }
};
struct Counts {
    uint64_t packed=0, updates=0, inverse=0, fine=0, queries=0, oracle=0;
    uint64_t survivors[5]={0,0,0,0,0};
} checks;
void require(bool ok,const std::string& message) { if(!ok) throw std::runtime_error(message); }
TUPLES key(uint64_t n) { TUPLES x{};std::memcpy(x.data,&n,8);x.data[12]=17;return x; }
std::array<int,4> positions(TUPLES x,int w) {
    std::array<int,4> p;
    for(int i=0;i<4;++i) p[i]=hash(x,100+i)%(w/4)+i*(w/4);
    return p;
}

// Independent oracle: merge every visit, with explicit same-time state ownership.
// It stores diagnostic state only and is never used by the candidate.
struct Events {
    Dual::Config c;
    int k;
    int64_t time=0;
    std::vector<int> values;
    explicit Events(Dual::Config cfg):c(cfg),k((1<<cfg.bits)-1),values(cfg.width,0) {}
    void insert(TUPLES x) { for(int j:positions(x,c.width)) values[j]=k; }
    void advance(int packets) {
        int64_t f=time*c.fastSpeed,s=time*c.slowSpeed;
        const int64_t fe=(time+packets)*c.fastSpeed,se=(time+packets)*c.slowSpeed;
        while(f<fe || s<se) {
            const bool doF=f<fe && (s==se || f*c.slowSpeed<=s*c.fastSpeed);
            const bool doS=s<se && (f==fe || s*c.fastSpeed<=f*c.slowSpeed);
            const int fj=int(f%c.width),sj=int(s%c.width);
            if(doF && doS && fj==sj) {
                const int old=values[fj];
                if(old>0) --values[fj]; // fine OR coarse, never both
            } else {
                if(doF && values[fj]>c.threshold) --values[fj];
                if(doS && values[sj]>0 && values[sj]<=c.threshold) --values[sj];
            }
            if(doF) ++f;
            if(doS) ++s;
        }
        time+=packets;
    }
};
void compare(const Dual& s,const Events& e) {
    require(s.time()==e.time,"time mismatch");
    require(s.fastPointer()==e.time*e.c.fastSpeed%e.c.width,"fast pointer mismatch");
    require(s.slowPointer()==e.time*e.c.slowSpeed%e.c.width,"slow pointer mismatch");
    for(int j=0;j<e.c.width;++j) {
        ++checks.updates;
        if(s.counter(j)!=e.values[j])
            throw std::runtime_error("event mismatch t="+std::to_string(e.time)+" bucket="+std::to_string(j));
    }
}

void packedAndBoundaries() {
    std::mt19937 rng(131);
    for(int bits=2;bits<=16;++bits) for(int w:{4,12,68,128}) {
        const int k=(1<<bits)-1;
        Dual s({w,bits,k/2,8192,8*w-1,std::max(1,w/4)});
        std::vector<int> expected(w);
        for(int step=0;step<300;++step) {
            const int j=rng()%w,value=rng()%(k+1);
            DualPointerTestAccess::set(s,j,value);expected[j]=value;
            for(int p=0;p<w;++p) { require(s.counter(p)==expected[p],"packed neighbor corruption");++checks.packed; }
        }
    }
    Dual s({4,3,5,8192,8,4});
    for(int j=0;j<4;++j) DualPointerTestAccess::set(s,j,6);
    s.updateClock();
    require(s.counter(0)==5,"same-time crossing double-decremented");
    require(s.counter(1)==4,"later slow visit in same packet was incorrectly skipped");
    Dual t({4,3,5,8192,8,4});
    DualPointerTestAccess::set(t,0,5);t.updateClock();
    require(t.counter(0)==4,"already coarse must decrement on coincident slow visit");
    for(int j=0;j<4;++j) DualPointerTestAccess::set(t,j,1);
    t.updateClock(20);
    for(int j=0;j<4;++j) require(t.counter(j)==0,"zero underflow");
    require(t.query(key(1)).emptyOrExpired,"empty/expired status missing");
    t.insert(key(1),20);t.updateClock();
    require(!t.query(key(1)).emptyOrExpired,"expired bucket cannot refresh");
    bool caught=false;try{t.updateClock(-1);}catch(const std::invalid_argument&){caught=true;}
    require(caught,"negative advance accepted");
    caught=false;try{t.updateRange(0,4,1);}catch(const std::logic_error&){caught=true;}
    require(caught,"ambiguous single-clock maintenance accepted");
}

void updateExhaustive() {
    for(int w:{4,12}) for(int bits:{2,3,5}) {
        const int k=(1<<bits)-1;
        for(int threshold:{1,k/2,k-1}) for(auto speed:std::vector<std::pair<int,int>>{{3,1},{w+1,2},{8*w-1,w/4}})
        for(int start=0;start<5;++start) for(int packets:{0,1,2,5}) for(int value=0;value<=k;++value) {
            const Dual::Config cfg{w,bits,threshold,8192,speed.first,speed.second};
            Dual s(cfg),sequential(cfg); Events e(cfg);
            DualPointerTestAccess::time(s,start);DualPointerTestAccess::time(sequential,start);e.time=start;
            for(int j=0;j<w;++j) {
                const int v=(value+j)%(k+1);
                DualPointerTestAccess::set(s,j,v);DualPointerTestAccess::set(sequential,j,v);e.values[j]=v;
            }
            s.updateClock(packets);e.advance(packets);compare(s,e);
            for(int i=0;i<packets;++i) sequential.updateClock();
            for(int j=0;j<w;++j) require(s.counter(j)==sequential.counter(j),"batch differs from packetwise update");
        }
    }
}

void inverseExhaustive() {
    for(int w:{4,8}) for(int bits:{2,3,4}) {
        const int k=(1<<bits)-1;
        for(int threshold:{1,k/2,k-1}) for(auto speed:std::vector<std::pair<int,int>>{{3,1},{7,2},{8*w-1,w/4}}) {
            const Dual::Config cfg{w,bits,threshold,8192,speed.first,speed.second};
            for(int t=0;t<=20;++t) {
                std::vector<std::vector<std::set<int>>> allowed(w,std::vector<std::set<int>>(k+1));
                for(int r=0;r<=t;++r) {
                    Events e(cfg);e.time=r;
                    std::fill(e.values.begin(),e.values.end(),k);e.advance(t-r);
                    for(int j=0;j<w;++j) allowed[j][e.values[j]].insert(r);
                }
                Dual s(cfg);DualPointerTestAccess::time(s,t);
                for(int j=0;j<w;++j) for(int c=0;c<=k;++c) {
                    DualPointerTestAccess::set(s,j,c);
                    const auto interval=s.intervalForBucket(j);
                    const auto& truth=allowed[j][c];
                    ++checks.inverse;
                    if(truth.empty()) require(!interval.valid(),"unreachable state accepted");
                    else if(!interval.valid() || interval.lo!=*truth.begin() || interval.hi!=*truth.rbegin()
                            || interval.hi-interval.lo+1!=int64_t(truth.size()))
                        throw std::runtime_error("inverse mismatch w="+std::to_string(w)+" bits="+std::to_string(bits)
                            +" threshold="+std::to_string(threshold)+" t="+std::to_string(t)+" j="+std::to_string(j)+" c="+std::to_string(c));
                }
            }
        }
    }
}

void inspectQuery(const Dual& s,TUPLES x,int64_t targetWrite,const std::vector<int64_t>& writes,
                  RingSketchSimd& sameLayout) {
    const auto p=positions(x,s.config().width);
    require(p==s.getPos(x),"hash layout differs");
    const auto result=s.query(x);
    const int age=int(s.time()-targetWrite);
    require(age>=0 && age<=s.config().window,"test query outside declared window");
    require(!result.emptyOrExpired,"active item expired early");
    int live=0;
    for(int i=0;i<4;++i) {
        if(writes[p[i]]==targetWrite) ++live;
        require(writes[p[i]]>=targetWrite,"diagnostic write time invalid");
        if(result.overwrittenMask&(1U<<i)) require(writes[p[i]]>targetWrite,"false collision identification");
    }
    ++checks.survivors[live];++checks.queries;
    require(result.ageLower<=age,"unconditional age lower bound violated");
    if(live) require(age<=result.ageUpper,"conditional age upper bound violated");
    if(result.fine) {
        require(result.estimate==sameLayout.estimate(x,int(s.time())),"young query changed from repro");++checks.fine;
    } else {
        require(result.estimate>=result.ageLower && result.estimate<=result.ageUpper,"estimate outside envelope");
        if(live) require(std::abs(result.estimate-age)<=(result.ageUpper-result.ageLower+1)/2,"midpoint error bound violated");
    }
}

void collisionAndRandom() {
    const auto cfg=Dual::forBudget(128,16,65407,4); // W=64, H=128
    // Use a compact clock with the same fast rate and shorter logical capacity.
    const Dual::Config c{64,8,127,512,cfg.fastSpeed,16};
    const auto target=key(1);
    const auto targetPos=positions(target,64);
    std::array<TUPLES,4> collider;
    for(int part=0;part<4;++part) {
        bool found=false;
        for(uint64_t n=100;n<100000 && !found;++n) {
            auto x=key(n);auto p=positions(x,64);bool ok=true;
            for(int j=0;j<4;++j) ok=ok&&((p[j]==targetPos[j])==(part==j));
            if(ok){collider[part]=x;found=true;}
        }
        require(found,"cannot construct controlled collision");
    }
    for(int covered=0;covered<=4;++covered) {
        Dual s(c);Events e(c);RingSketchSimd baseline(8192,128,4,16);
        std::vector<int64_t> writes(64,-1);
        s.updateClock(50);e.advance(50);baseline.updateClock(50);
        s.insert(target,50);e.insert(target);baseline.insert(target,50);
        for(int j:targetPos) writes[j]=50;
        s.updateClock(100);e.advance(100);baseline.updateClock(100);
        for(int j=0;j<covered;++j) {
            s.insert(collider[j],150);e.insert(collider[j]);baseline.insert(collider[j],150);
            for(int p:positions(collider[j],64)) writes[p]=150;
        }
        s.updateClock(3);e.advance(3);baseline.updateClock(3);compare(s,e);
        inspectQuery(s,target,50,writes,baseline);
        const auto q=s.query(target);
        if(covered<4) require(q.overwrittenMask==((1U<<covered)-1),"controlled overwrite not identified");
        else require(q.estimate!=103 && q.overwrittenMask==0,"full-cover ambiguity example missing");
    }
    for(int seed:{131,132,133,134}) {
        std::mt19937 rng(seed);Dual s(c);Events e(c);RingSketchSimd baseline(8192,128,4,16);
        std::vector<int64_t> writes(64,-1);std::array<int64_t,64> last;last.fill(-1);
        for(int step=0;step<1200;++step) {
            const int id=rng()%64;const auto x=key(id);
            s.insert(x,int(s.time()));e.insert(x);baseline.insert(x,int(s.time()));
            last[id]=s.time();for(int j:positions(x,64)) writes[j]=s.time();
            const int n=rng()%8;s.updateClock(n);e.advance(n);baseline.updateClock(n);compare(s,e);
            for(int q=0;q<4;++q) {
                const int idq=rng()%64;
                if(last[idq]>=0 && s.time()-last[idq]<=512) inspectQuery(s,key(idq),last[idq],writes,baseline);
            }
        }
    }
}

using Wide=__int128;
Wide floorQ(Wide a,Wide b){Wide q=a/b;return q-(a%b<0);}
int resetOracle(const Dual::Config& c,int j,int64_t r,int64_t t) {
    if(r<0) return 0;
    const int k=(1<<c.bits)-1,h=k-c.threshold;
    // Count visits from the saved last reset, not from candidate's previous state.
    const int64_t firstCycle=(r*c.fastSpeed+c.width-1-j)/c.width;
    const int64_t lastCycle=(t*c.fastSpeed+c.width-1-j)/c.width;
    const int64_t count=lastCycle-firstCycle;
    if(count<h) return k-int(count);
    const int64_t crossing=j+(firstCycle+h-1)*c.width;
    const int64_t firstSlow=int64_t(floorQ(Wide(crossing)*c.slowSpeed-Wide(j)*c.fastSpeed,
                                        Wide(c.width)*c.fastSpeed))+1;
    const int64_t endSlow=(t*c.slowSpeed+c.width-1-j)/c.width;
    return int(std::max<int64_t>(0,int64_t(c.threshold)-(endSlow-firstSlow)));
}

struct Metric {
    uint64_t n=0,abs=0,hits=0;long double rel=0;
    void add(int estimate,int truth){++n;const int64_t e=std::llabs(int64_t(estimate)-truth);abs+=e;hits+=e==0;rel+=(long double)e/truth;}
};
int ageBin(int a) { if(a<=16)return 0;if(a<=64)return 1;if(a<=255)return 2;if(a<=1024)return 3;if(a<=4096)return 4;return 5; }

const int Window=8192;
void scalarSimdTest() {
    for (int memory : {32, 40, 32000}) {
        RingSketch scalar(Window, memory, 4, 16); RingSketchSimd simd(Window, memory, 4, 16);
        const int w = scalar.width, speed = scalar.updateSpeed;
        // Independent closed-form sweep count, with two interleaved repeat-heavy streams.
        std::vector<int64_t> written(w, -1);
        for (int t = 0; t < 9000; ++t) {
            TUPLES k = key(memory < 100 ? t % 19 : t % 3);
            scalar.insert(k, t); simd.insert(k, t);
            for (int pos : scalar.getPos(k)) written[pos] = int64_t(t) * speed;
            scalar.updateClock(1); simd.updateClock(1);
            int64_t total = int64_t(t + 1) * speed;
            require(scalar.lastUpdateIdx == total % w, "pointer oracle");
            // Full state check at boundaries; covers saturating decay and pointer wrap.
            if (t % 127 == 0 || t == 8999) for (int pos = 0; pos < w; ++pos) {
                int64_t age = written[pos] < 0 ? 65535 : (total + w - 1 - pos) / w - (written[pos] + w - 1 - pos) / w;
                require(scalar.clocks[pos] == std::max<int64_t>(0, 65535 - age), "closed-form decay oracle");
            }
            for (int j = 0; j < 5; ++j) require(scalar.estimate(key(j), t + 1) == simd.estimate(key(j), t + 1), "scalar/SIMD estimate mismatch");
        }
    }
    // Independently calculable quantization and collision filtering in original estimator.
    RingSketch s(Window, 32, 4, 16); auto p = s.getPos(key(77));
    require(s.width == 16 && s.updateSpeed == 127, "small example dimensions");
    s.lastUpdateIdx = 0;
    for (int pos : p) s.clocks[pos] = 65534;
    require(s.estimate(key(77), 1) == 0, "quantization example 0");
    for (int pos : p) s.clocks[pos] = 65000;
    require(s.estimate(key(77), 1) == 67, "quantization example 67");
    int unchanged = s.estimate(key(77), 1); s.clocks[p[0]] = 65535;
    require(s.estimate(key(77), 1) == unchanged, "large collided counter must not lower minimum");
    for (int pos : p) s.clocks[pos] = 0;
    require(s.estimate(key(77), 1) == 8256, "expired saturation example");
    std::cout << "PASS scalar/SIMD (135000 estimates), independent sweep state, wrap, collisions, quantization, expiry\n";
}
