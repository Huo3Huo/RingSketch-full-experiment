#pragma once
#include <array>
#include <ostream>

// Fixed experimental choices, never derived from wall-clock date or time.
namespace Protocol {
constexpr int Window = 8192;
constexpr int Hashes = 4;
constexpr int OptimizedBits = 12;
constexpr int BaselineBits = 16;
constexpr int Threshold = 1984;
constexpr int SlowPeriod = 4;
constexpr int QueryEvery = 10;
constexpr int YoungMaxAge = 256;
constexpr int DevelopmentPackets = 65536;
constexpr unsigned SnapshotSeed = 135;
constexpr std::array<int, 4> Memories{{32000, 64000, 96000, 128000}};
constexpr std::array<unsigned, 4> AccuracySeeds{{131, 132, 133, 134}};
constexpr std::array<unsigned, 4> HashSeeds{{100, 101, 102, 103}};
struct TimingProfile {
    int repeats, warmups, snapshots, keysPerSnapshot, minBatches;
    double targetSeconds;
};
constexpr TimingProfile Full{5, 1, 128, 8192, 16, 0.03};
constexpr TimingProfile Smoke{2, 1, 4, 256, 1, 0.003};
template<class Values> void arrayJSON(std::ostream& out, const Values& values) {
    out << '['; bool first = true;
    for (auto v : values) { if (!first) out << ','; out << v; first = false; }
    out << ']';
}
inline void timingJSON(std::ostream& out, const TimingProfile& p) {
    out << "{\"repeats\":" << p.repeats << ",\"warmups\":" << p.warmups
        << ",\"snapshots\":" << p.snapshots << ",\"keys_per_snapshot\":" << p.keysPerSnapshot
        << ",\"min_batches\":" << p.minBatches << ",\"target_seconds\":" << p.targetSeconds << '}';
}
inline void describe(std::ostream& out) {
    out << "{\"optimized_maintenance\":\"avx2_packed12\",\"baseline_maintenance\":\"avx2_u16\",\"window\":" << Window << ",\"hashes\":" << Hashes
        << ",\"optimized_bits\":" << OptimizedBits << ",\"threshold\":" << Threshold
        << ",\"slow_period\":" << SlowPeriod << ",\"query_every\":" << QueryEvery
        << ",\"young_max_age\":" << YoungMaxAge << ",\"development_packets\":" << DevelopmentPackets
        << ",\"snapshot_seed\":" << SnapshotSeed << ",\"memory_bytes\":";
    arrayJSON(out, Memories); out << ",\"seeds\":"; arrayJSON(out, AccuracySeeds);
    out << ",\"hash_seeds\":"; arrayJSON(out, HashSeeds);
    out << ",\"throughput\":{\"full\":"; timingJSON(out, Full);
    out << ",\"smoke\":"; timingJSON(out, Smoke); out << "}}\n";
}
}
