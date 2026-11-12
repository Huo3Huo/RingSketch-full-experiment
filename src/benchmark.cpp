// Timing driver only. Algorithm implementations are shared with experiment.cpp.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
#include "Algos/RingSketch/RingSketchSimd.h"
#include "Algos/RingSketch/RingSketchDualPointer.h"
#pragma pack()
#include "Protocol.h"

using Clock = std::chrono::steady_clock;
volatile uint64_t consumed = 0;
class TrajectorySketch : public RingSketchTrajectory::Sketch {
public:
    using RingSketchTrajectory::Sketch::Sketch;
    int estimate(TUPLES key, int) override {
        return RingSketchTrajectory::query(*this, key).estimate;
    }
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::ofstream output(const std::string& path) {
    std::ofstream f(path); f.exceptions(std::ios::badbit | std::ios::failbit);
    f << std::setprecision(17); return f;
}
std::unique_ptr<Abstract> instance(int memory, const std::string& method) {
    const auto c = RingSketchDualPointer::forBudget(memory, Protocol::OptimizedBits,
        Protocol::Threshold, Protocol::SlowPeriod, Protocol::Window, Protocol::OptimizedHashes);
    if (method == "optimized") return std::unique_ptr<Abstract>(new TrajectorySketch(c));
    require(method == "baseline", "unknown method");
    return std::unique_ptr<Abstract>(new RingSketchSimd(Protocol::Window, memory,
        Protocol::BaselineHashes, Protocol::BaselineBits));
}
std::vector<TUPLES> readData(const std::string& path, int requested) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    require(bool(in), "cannot open input");
    const int64_t bytes = in.tellg();
    require(bytes > 0 && bytes % 21 == 0, "invalid input length");
    const int64_t n = requested ? requested : bytes/21;
    require(n > 0 && n <= bytes/21 && n <= std::numeric_limits<int>::max(), "invalid packet count");
    in.seekg(0); std::vector<TUPLES> data(n);
    for (auto& key : data) {
        char record[21]; require(bool(in.read(record, 21)), "short input");
        std::memcpy(key.data, record, 13);
    }
    return data;
}
uint64_t stateChecksum(Abstract& sketch, const std::vector<TUPLES>& data) {
    uint64_t sum = 0; const int n = int(data.size());
    for (int j = std::max(0, n-Protocol::Window); j < n; ++j)
        sum = sum*131 + sketch.estimate(data[j], n);
    consumed = sum; return sum;
}
struct Sample {
    int time;
    std::vector<TUPLES> keys;
    uint64_t indexChecksum = 0;
};
std::vector<Sample> snapshots(const std::vector<TUPLES>& data, const Protocol::TimingProfile& profile) {
    std::mt19937 rng(Protocol::SnapshotSeed); std::vector<Sample> result;
    require(data.size() >= size_t(profile.snapshots), "too few packets for snapshots");
    for (int j = 0; j < profile.snapshots; ++j) {
        Sample sample; sample.time = int(uint64_t(j+1)*data.size()/profile.snapshots);
        std::uniform_int_distribution<int> dist(std::max(0, sample.time-Protocol::Window), sample.time-1);
        for (int k = 0; k < profile.keysPerSnapshot; ++k) {
            const int index = dist(rng);
            sample.indexChecksum = sample.indexChecksum*131 + index;
            sample.keys.push_back(data[index]);
        }
        result.push_back(std::move(sample));
    }
    return result;
}
struct Measurement { double seconds = 0; uint64_t operations = 0, checksum = 0; };
__attribute__((noinline)) Measurement queryBatch(Abstract& sketch, const Sample& sample, int batches) {
    uint64_t sum = 0; const auto start = Clock::now();
    for (int b = 0; b < batches; ++b) {
        // Prevent identical frozen batches from being hoisted/eliminated.
        asm volatile("" ::: "memory");
        for (const auto& key : sample.keys) sum += uint64_t(sketch.estimate(key, sample.time));
    }
    const double seconds = std::chrono::duration<double>(Clock::now()-start).count();
    consumed = sum;
    return {seconds, uint64_t(batches)*sample.keys.size(), sum};
}
Measurement updateRun(Abstract& sketch, const std::vector<TUPLES>& data) {
    const auto start = Clock::now();
    for (int t = 0; t < int(data.size()); ++t) { sketch.insert(data[t], t); sketch.updateClock(1); }
    Measurement m;
    m.seconds = std::chrono::duration<double>(Clock::now()-start).count();
    m.operations = data.size(); m.checksum = stateChecksum(sketch, data); return m;
}
Measurement queryRun(Abstract& sketch, const std::vector<TUPLES>& data,
    const std::vector<Sample>& samples, int batches, int memory, const std::string& method,
    int repeat, std::ofstream& rows) {
    Measurement total; int t = 0, j = 0;
    for (const auto& sample : samples) {
        // State preparation and key construction are outside query timing.
        for (; t < sample.time; ++t) { sketch.insert(data[t], t); sketch.updateClock(1); }
        const auto m = queryBatch(sketch, sample, batches);
        total.seconds += m.seconds; total.operations += m.operations; total.checksum += m.checksum;
        rows << memory << ',' << method << ',' << repeat << ',' << j++ << ',' << sample.time
            << ',' << batches << ',' << sample.keys.size() << ',' << sample.indexChecksum
            << ',' << m.seconds << ',' << m.operations << ',' << m.checksum << '\n';
        rows.flush();
    }
    return total;
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--describe") { Protocol::describe(std::cout); return 0; }
        require((argc-1)%2 == 0, "arguments require values");
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; i += 2) require(args.emplace(argv[i], argv[i+1]).second, "duplicate option");
        const auto name = args.at("--profile");
        require(name == "smoke" || name == "full" || name == "sample", "unknown profile");
        const auto p = name == "smoke" ? Protocol::Smoke : Protocol::Full;
        const auto data = readData(args.at("--data"), std::stoi(args.at("--count")));
        const int memory = std::stoi(args.at("--memory"));
        require(std::find(Protocol::Memories.begin(), Protocol::Memories.end(), memory) != Protocol::Memories.end(), "unknown memory point");
        const auto phase = args.at("--phase"), dir = args.at("--out");
        require(phase == "calibrate" || phase == "update" || phase == "query", "unknown phase");
        const auto samples = phase == "update" ? std::vector<Sample>{} : snapshots(data, p);
        if (phase == "calibrate") {
            auto rows = output(dir+"/calibration.csv");
            rows << "memory,method,batches,seconds,operations,checksum\n";
            int common = p.minBatches;
            for (const std::string method : {"baseline", "optimized"}) {
                auto s = instance(memory, method); const auto& sample = samples.front();
                for (int t = 0; t < sample.time; ++t) { s->insert(data[t], t); s->updateClock(1); }
                queryBatch(*s, sample, p.minBatches);
                int b = p.minBatches;
                for (;;) {
                    const auto m = queryBatch(*s, sample, b);
                    rows << memory << ',' << method << ',' << b << ',' << m.seconds << ',' << m.operations << ',' << m.checksum << '\n';
                    if (m.seconds >= p.targetSeconds) break;
                    b *= 2; require(b <= 16384, "calibration limit exceeded");
                }
                common = std::max(common, b);
            }
            auto selected = output(dir+"/batches.txt"); selected << common << '\n';
            std::cout << "PASS calibration memory=" << memory << " batches=" << common << '\n'; return 0;
        }
        const int batches = std::stoi(args.at("--batches"));
        require(batches >= p.minBatches && batches <= 16384, "invalid batches");
        auto rows = output(dir+"/"+phase+"_raw.csv");
        rows << "memory,method,repeat,n,operations,seconds,mops,checksum,final_state_checksum,status\n";
        std::ofstream snapshotRows;
        if (phase == "query") {
            snapshotRows = output(dir+"/query_snapshots.csv");
            snapshotRows << "memory,method,repeat,snapshot,q,batches,keys,index_checksum,seconds,operations,checksum\n";
        }
        for (int repeat = -p.warmups; repeat < p.repeats; ++repeat) {
            // Alternate order; each measured configuration runs alone on the same CPU.
            std::array<std::string, 2> order{{"baseline", "optimized"}};
            if ((repeat+p.warmups)%2) std::swap(order[0], order[1]);
            for (const auto& method : order) {
                auto s = instance(memory, method);
                const auto m = phase == "update" ? updateRun(*s, data)
                    : queryRun(*s, data, samples, batches, memory, method, repeat, snapshotRows);
                const auto finalChecksum = phase == "update" ? m.checksum : stateChecksum(*s, data);
                require(m.seconds > 0, "nonpositive duration");
                rows << memory << ',' << method << ',' << repeat << ',' << data.size() << ',' << m.operations
                    << ',' << m.seconds << ',' << m.operations/m.seconds/1e6 << ',' << m.checksum << ',' << finalChecksum << ",OK\n";
                rows.flush();
                std::cout << "PROGRESS " << phase << " memory=" << memory << " method=" << method
                    << " repeat=" << repeat << " seconds=" << m.seconds << '\n' << std::flush;
            }
        }
        rows.close(); if (phase == "query") snapshotRows.close();
        std::cout << "ALL CHECKS PASSED\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
