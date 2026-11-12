#include "../tests/oracle_support.h"
#include "Algos/RingSketch/RingSketchDualPointer.h"
#include <limits>
#include "Protocol.h"
using Optimized=RingSketchTrajectory::Sketch;

// Oracle state is diagnostic only; neither estimator receives last-write metadata.
void inspect(const Optimized& s,TUPLES x,int target,const std::vector<int64_t>& writes,
             RingSketchSimd& baseline) {
    const auto p=positions(x,s.config().width);
    const auto q=RingSketchTrajectory::query(s,x);
    const int age=int(s.time()-target);int live=0;
    require(p==s.getPos(x),"hash layout mismatch");
    require(!q.emptyOrExpired,"active item expired early");
    for(int i=0;i<4;++i) {
        live+=writes[p[i]]==target;
        require(writes[p[i]]>=target,"invalid diagnostic write order");
        if(q.overwrittenMask&(1U<<i)) require(writes[p[i]]>target,"false collision exclusion");
    }
    require(q.ageLower<=age,"age lower bound violated");
    if(live) require(age<=q.ageUpper,"conditional upper bound violated");
    require(q.estimate>=q.ageLower&&q.estimate<=q.ageUpper,"estimate outside envelope");
    if(q.fine) require(q.estimate==baseline.estimate(x,int(s.time())),"fine query mismatch");
}
std::ofstream output(const std::string& path) {
    std::ofstream f(path);f.exceptions(std::ios::badbit|std::ios::failbit);return f;
}
void run(const std::string& path,int memory,const std::string& out,int requested) {
    const auto cfg=Dual::forBudget(memory,Protocol::OptimizedBits,Protocol::Threshold,Protocol::SlowPeriod,Protocol::Window);
    Optimized optimized(cfg);RingSketchSimd baseline(Protocol::Window,2*cfg.width,Protocol::Hashes,Protocol::BaselineBits);
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    require(bool(input),"cannot open input");
    const auto bytes=input.tellg();
    require(bytes>0&&int64_t(bytes)%21==0,"empty input or partial 21-byte record");
    const int64_t total=int64_t(bytes)/21;
    require(total<=std::numeric_limits<int>::max(),"trace exceeds supported logical time");
    const int n=requested?requested:int(total);
    require(n>0&&n<=total,"packet count outside input");
    input.seekg(0);std::vector<TUPLES> data(n);
    for(auto& x:data) {char rec[21];require(bool(input.read(rec,21)),"short input");std::memcpy(x.data,rec,13);}
    std::unordered_map<TUPLES,int> last;
    std::vector<int64_t> writes(cfg.width,-1);
    std::array<std::mt19937,4> rng;
    for(size_t i=0;i<rng.size();++i) rng[i].seed(Protocol::AccuracySeeds[i]);
    // Phase: all, original development prefix, remainder (without resetting state).
    // Cohorts: overall, young, stale, seven age bins, five survivor counts.
    Metric metrics[3][4][2][15];
    uint64_t queries=0,young=0,age256=0,coarse=0,truthChecks=0;
    auto trace=output(out+"/queries_"+std::to_string(memory)+".csv");
    trace<<"t,seed,index,truth,baseline,optimized,fine,survivors,mask,lower,upper\n";
    auto check=[&](int j) {
        require(optimized.counter(j)==resetOracle(cfg,j,writes[j],optimized.time()),"reset oracle state mismatch");
        ++checks.oracle;
    };
    for(int t=0;t<n;++t) {
        if(t&&t%Protocol::QueryEvery==0) for(int seed=0;seed<4;++seed) {
            std::uniform_int_distribution<int> sample(std::max(0,t-Protocol::Window),t-1);
            const int index=sample(rng[seed]);const auto x=data[index];
            const int written=last.at(x),truth=t-written;
            require(truth>=1&&truth<=8192,"truth outside window");
            if(t%40960==0) {
                int back=t-1;while(!(data[back]==x))--back;
                require(back==written,"direct backtracking truth mismatch");++truthChecks;
            }
            const int b=baseline.estimate(x,t);
            const auto q=RingSketchTrajectory::query(optimized,x);
            inspect(optimized,x,written,writes,baseline);
            int live=0;for(int j:positions(x,cfg.width)){check(j);live+=writes[j]==written;}
            if(truth<=Protocol::YoungMaxAge) {require(q.fine&&q.estimate==b,"young query differs from baseline");++young;}
            age256+=truth==256;coarse+=!q.fine;++queries;
            const int bin=truth<=16?0:truth<=64?1:truth<=255?2:truth==256?3:truth<=1024?4:truth<=4096?5:6;
            const int groups[]={0,truth<=256?1:2,3+bin,10+live};
            const int estimates[]={b,q.estimate};
            for(int phase:{0,t<Protocol::DevelopmentPackets?1:2}) for(int method=0;method<2;++method)
                for(int group:groups) metrics[phase][seed][method][group].add(estimates[method],truth);
            trace<<t<<','<<Protocol::AccuracySeeds[seed]<<','<<index<<','<<truth<<','<<b<<','<<q.estimate<<','<<q.fine<<','<<live<<','<<q.overwrittenMask<<','<<q.ageLower<<','<<q.ageUpper<<'\n';
        }
        last[data[t]]=t;optimized.insert(data[t],t);baseline.insert(data[t],t);
        for(int j:positions(data[t],cfg.width)) writes[j]=t;
        optimized.updateClock(1);baseline.updateClock(1);
        check(int(uint64_t(t)*7919%cfg.width));check(int(uint64_t(t)*104729%cfg.width));
        if((t+1)%4096==0||t+1==n) for(int j=0;j<cfg.width;++j) check(j);
        if((t+1)%65536==0||t+1==n) std::cout<<"PROGRESS packets="<<t+1<<"/"<<n<<" queries="<<queries<<"\n"<<std::flush;
    }
    require(queries==Protocol::AccuracySeeds.size()*uint64_t((n-1)/Protocol::QueryEvery),"query count mismatch");
    if(n>=65536)require(young&&age256&&coarse,"missing inherited development coverage");
    const char* phases[]={"all","development","outside_development"};
    const char* methods[]={"baseline","optimized"};
    const char* groups[]={"overall","young_1_256","stale_257_8192","age_1_16","age_17_64","age_65_255","age_256","age_257_1024","age_1025_4096","age_4097_8192","survivors_0","survivors_1","survivors_2","survivors_3","survivors_4"};
    auto summary=output(out+"/metrics_"+std::to_string(memory)+".csv");
    summary<<std::setprecision(20)<<"phase,seed,method,cohort,n,abs_sum,relative_sum,aae,are,hits\n";
    for(int phase=0;phase<3;++phase)for(int seed=0;seed<4;++seed)for(int method=0;method<2;++method)for(int group=0;group<15;++group) {
        const auto& m=metrics[phase][seed][method][group];
        summary<<phases[phase]<<','<<Protocol::AccuracySeeds[seed]<<','<<methods[method]<<','<<groups[group]<<','<<m.n<<','<<m.abs<<','<<m.rel<<',';
        if(m.n)summary<<(long double)m.abs/m.n<<','<<m.rel/m.n;else summary<<",";
        summary<<','<<m.hits<<'\n';
    }
    uint64_t baselineChecksum=0,optimizedChecksum=0;
    for(int j=std::max(0,n-Protocol::Window);j<n;++j) {
        baselineChecksum=baselineChecksum*131+baseline.estimate(data[j],n);
        optimizedChecksum=optimizedChecksum*131+RingSketchTrajectory::query(optimized,data[j]).estimate;
    }
    auto checksums=output(out+"/final_checksums.json");
    checksums<<"{\"baseline\":"<<baselineChecksum<<",\"optimized\":"<<optimizedChecksum<<"}\n";checksums.close();
    auto layout=output(out+"/layout_"+std::to_string(memory)+".json");
    layout<<"{\"memory\":"<<memory<<",\"width\":"<<cfg.width<<",\"bits\":12,\"threshold\":1984,\"period\":4,\"window\":8192,\"payload\":"<<optimized.payloadBytes()<<",\"allocated_payload\":"<<optimized.allocatedPayloadBytes()<<",\"baseline_payload\":"<<2*cfg.width<<",\"optimized_object_bytes\":"<<sizeof(optimized)<<",\"baseline_object_bytes\":"<<sizeof(baseline)<<",\"packets\":"<<n<<",\"queries\":"<<queries<<",\"young\":"<<young<<",\"age256\":"<<age256<<",\"coarse\":"<<coarse<<",\"oracle_checks\":"<<checks.oracle<<",\"direct_truth_checks\":"<<truthChecks<<"}\n";
    // Make delayed write failures visible before declaring success.
    trace.close();summary.close();layout.close();
    std::cout<<"PASS memory="<<memory<<" width="<<cfg.width<<"\nALL CHECKS PASSED\n"<<std::flush;
}
int main(int argc,char**argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--describe") {Protocol::describe(std::cout);return 0;}
        require(sizeof(TUPLES)==13,"key size mismatch");
        require(argc==5,"usage: experiment DATA MEMORY OUTDIR COUNT (0=full)");
        run(argv[1],std::stoi(argv[2]),argv[3],std::stoi(argv[4]));
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
