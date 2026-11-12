#ifndef RING_SKETCH_DUAL_POINTER_H
#define RING_SKETCH_DUAL_POINTER_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <immintrin.h>
#include "../Abstract.h"

// Keep upstream headers unchanged, including their packing. This class owns its layout.
#pragma pack(push, 8)
class RingSketchDualPointer : public Abstract {
public:
    struct Config {
        int width, bits, threshold, window;
        int fastSpeed, slowSpeed; // positions per packet, same phase origin at position 0
        int hashes = 4;
        int allocationBytes = 0; // Optional budget padding, never addressable as counters.
    };
    struct Positions {
        std::array<int, 5> values{};
        int count = 4;
        int& operator[](size_t i) { return values[i]; }
        int operator[](size_t i) const { return values[i]; }
        const int* begin() const { return values.data(); }
        const int* end() const { return values.data() + count; }
        bool operator==(const Positions& other) const {
            return count == other.count && std::equal(begin(), end(), other.begin());
        }
    };
    struct WriteInterval {
        int64_t lo = 0, hi = -1;
        bool valid() const { return lo <= hi; }
    };
    struct QueryResult {
        int estimate = 0;
        int ageLower = 0, ageUpper = 0;
        unsigned overwrittenMask = 0; // certainly newer than target, NOT all collisions
        bool fine = false;
        bool emptyOrExpired = false;
        bool conditionalBounds = false; // upper bound assumes at least one surviving bucket
    };

    static Config forBudget(int bytes, int bits, int threshold, int slowPeriod,
                            int window = 8192, int hashes = 4) {
        if (bytes < 8 || bits < 2 || bits > 16 || window <= 0 || slowPeriod <= 1)
            throw std::invalid_argument("invalid dual-pointer budget/configuration");
        // Original repro budgets counter payload; metadata is reported separately.
        if (hashes != 4 && hashes != 5) throw std::invalid_argument("supported hash counts are 4 and 5");
        int quantum = hashes;
        while (quantum % 4 || quantum % slowPeriod) quantum += hashes;
        const int64_t availableBits = int64_t(bytes / 8) * 64;
        const int64_t w = (availableBits / bits) / quantum * quantum;
        if (w < 4 || w > std::numeric_limits<int>::max())
            throw std::invalid_argument("invalid width");
        const int64_t vf = 65535LL * w / window;
        const int64_t vs = w / slowPeriod;
        if (vf > std::numeric_limits<int>::max() || vs <= 0)
            throw std::invalid_argument("invalid clock speed");
        return {int(w), bits, threshold, window, int(vf), int(vs), hashes, bytes / 8 * 8};
    }

    explicit RingSketchDualPointer(const Config& c) : cfg_(c), maximum_(0), now_(0) {
        if ((c.hashes != 4 && c.hashes != 5) || c.width < c.hashes || c.width % c.hashes
                || c.width % 4 || c.bits < 2 || c.bits > 16 || c.window <= 0
                || c.fastSpeed <= 0 || c.slowSpeed <= 0 || c.slowSpeed >= c.fastSpeed)
            throw std::invalid_argument("invalid dual-pointer configuration");
        maximum_ = (1 << c.bits) - 1;
        if (c.threshold <= 0 || c.threshold >= maximum_)
            throw std::invalid_argument("both clock stages must be nonempty");
        const size_t needed = (uint64_t(c.width) * c.bits + 63) / 64;
        if (c.allocationBytes < 0 || c.allocationBytes % 8
                || (c.allocationBytes && size_t(c.allocationBytes) < needed * 8))
            throw std::invalid_argument("counter allocation too small or unaligned");
        words_.assign(c.allocationBytes ? size_t(c.allocationBytes) / 8 : needed, 0);
        name = "RingDual";
    }
    RingSketchDualPointer(const RingSketchDualPointer&) = delete;
    RingSketchDualPointer& operator=(const RingSketchDualPointer&) = delete;

    const Config& config() const { return cfg_; }
    int maximum() const { return maximum_; }
    int64_t time() const { return now_; }
    int fastPointer() const { return int(now_ * cfg_.fastSpeed % cfg_.width); }
    int slowPointer() const { return int(now_ * cfg_.slowSpeed % cfg_.width); }
    size_t payloadBytes() const { return words_.size() * sizeof(uint64_t); }
    size_t allocatedPayloadBytes() const { return words_.capacity() * sizeof(uint64_t); }

    Positions getPos(TUPLES key) const {
        Positions p; p.count = cfg_.hashes;
        const int interval = cfg_.width / cfg_.hashes;
        for (int i = 0; i < cfg_.hashes; ++i)
            p[i] = hash(key, 100 + i) % interval + i * interval;
        return p;
    }

    int counter(int j) const {
        if (j < 0 || j >= cfg_.width) throw std::out_of_range("bucket index");
        return read(j);
    }
    void insert(TUPLES key, int) override {
        for (int j : getPos(key)) write(j, maximum_);
    }

    void updateClock(int packets = 1) override {
        if (cfg_.bits == 12 && packets == 1) {
            updateClockSimd12();
            return;
        }
        updateClockScalar(packets);
    }

    // Reference path retained for equivalence checks and non-production bit widths.
    void updateClockScalar(int packets = 1) {
        if (packets < 0 || now_ + packets > std::numeric_limits<int>::max())
            throw std::invalid_argument("packet advance outside supported repro time domain");
        if (!packets) return;
        const int64_t end = now_ + packets;
        const int64_t f0 = now_ * cfg_.fastSpeed, f1 = end * cfg_.fastSpeed;
        const int64_t s0 = now_ * cfg_.slowSpeed, s1 = end * cfg_.slowSpeed;
        const int w = cfg_.width, fp0 = int(f0 % w), fp1 = int(f1 % w);
        const int sp0 = int(s0 % w), sp1 = int(s1 % w);
        const int64_t fc = f1 / w - f0 / w, sc = s1 / w - s0 / w;
        for (int j = 0; j < w; ++j) {
            const int c = read(j);
            if (!c) continue;
            int next;
            if (c > cfg_.threshold) {
                const int remainingFast = c - cfg_.threshold;
                const int64_t visits = fc + (j < fp1) - (j < fp0);
                if (visits < remainingFast) {
                    next = c - int(visits);
                } else {
                    // Exact event index of the fast visit that reaches the threshold.
                    const int64_t first = f0 + (int64_t(j) + w - fp0) % w;
                    const int64_t crossing = first + int64_t(remainingFast - 1) * w;
                    // Exclude slow visits at OR BEFORE crossing, including simultaneous visits.
                    const int64_t through = int64_t(I128(crossing) * cfg_.slowSpeed / cfg_.fastSpeed) + 1;
                    const int64_t slowVisits = before(s1,j,w) - before(through,j,w);
                    next = int(std::max<int64_t>(0, int64_t(cfg_.threshold) - slowVisits));
                }
            } else {
                const int64_t visits = sc + (j < sp1) - (j < sp0);
                next = int(std::max<int64_t>(0, int64_t(c) - visits));
            }
            if (next != c) write(j, next);
        }
        now_ = end;
    }

    // Abstract's single-clock range operation has no unambiguous two-clock semantics.
    void updateRange(int, int, int) override {
        throw std::logic_error("use updateClock for ordered dual-pointer maintenance");
    }

    // Exact inverse for an already-written bucket, including a saturated history.
    // c=0 is also the empty sentinel, so this method does not prove membership.
    WriteInterval intervalForBucket(int j) const {
        const int c = counter(j), h = maximum_ - cfg_.threshold;
        const I128 w = cfg_.width, vf = cfg_.fastSpeed, vs = cfg_.slowSpeed;
        const I128 fTotal = I128(now_) * vf, sTotal = I128(now_) * vs;
        const I128 lastF = I128(j) + floorDiv(fTotal - 1 - j, w) * w;
        I128 lo, hi;
        if (c > cfg_.threshold) {
            const int a = maximum_ - c;
            lo = floorDiv(lastF - I128(a) * w, vf) + 1;
            hi = floorDiv(lastF - I128(a - 1) * w, vf);
        } else {
            const int q = cfg_.threshold - c;
            const I128 lastS = I128(j) + floorDiv(sTotal - 1 - j, w) * w;
            // Earliest crossing possible after a nonnegative write.
            I128 uMin = I128(j) + I128(h - 1) * w;
            I128 uMax = fTotal - 1;
            if (q == 0) {
                uMin = std::max(uMin, ceilDiv(lastS * vf, vs));
            } else {
                if (c != 0)
                    uMin = std::max(uMin, ceilDiv((lastS - I128(q) * w) * vf, vs));
                uMax = std::min(uMax, ceilDiv((lastS - I128(q - 1) * w) * vf, vs) - 1);
            }
            // Restrict the crossing to actual fast visits at this bucket.
            uMin = I128(j) + ceilDiv(uMin - j, w) * w;
            uMax = I128(j) + floorDiv(uMax - j, w) * w;
            if (uMin > uMax) return {};
            lo = floorDiv(uMin - I128(h) * w, vf) + 1;
            hi = floorDiv(uMax - I128(h - 1) * w, vf);
        }
        lo = std::max<I128>(lo, 0);
        hi = std::min<I128>(hi, now_);
        if (lo > hi) return {};
        return {int64_t(lo), int64_t(hi)};
    }

    QueryResult query(TUPLES key) const {
        const auto p = getPos(key);
        QueryResult out;
        bool allFine = true;
        std::array<WriteInterval, 5> intervals;
        int64_t earliest = now_, latest = now_;
        for (int i = 0; i < cfg_.hashes; ++i) {
            const int c = read(p[i]);
            if (!c) {
                out.estimate = cfg_.window;
                out.ageUpper = cfg_.window;
                out.emptyOrExpired = true;
                return out;
            }
            allFine = allFine && c > cfg_.threshold;
            intervals[i] = intervalForBucket(p[i]);
            if (!intervals[i].valid()) throw std::logic_error("unreachable bucket state");
            earliest = std::min(earliest, intervals[i].lo);
            latest = std::min(latest, intervals[i].hi);
        }
        for (int i = 0; i < cfg_.hashes; ++i)
            if (intervals[i].lo > latest) out.overwrittenMask |= 1U << i;
        out.ageLower = int(now_ - latest);
        out.ageUpper = int(std::min<int64_t>(cfg_.window, now_ - earliest));
        if (out.ageLower > cfg_.window) {
            out.estimate = cfg_.window;
            out.ageUpper = out.ageLower; // no valid active-window bound
            out.emptyOrExpired = true;
            return out;
        }
        out.conditionalBounds = true;
        out.fine = allFine;
        if (allFine) {
            out.estimate = originalFineEstimate(p);
        } else {
            // Minimax absolute-error point in the oldest-feasible envelope.
            // It is NOT a proof of a surviving owner, nor ARE optimality.
            out.estimate = out.ageLower + (out.ageUpper - out.ageLower) / 2;
        }
        return out;
    }
    int estimate(TUPLES key, int) override { return query(key).estimate; }

private:
    using I128 = __int128;
    Config cfg_;
    int maximum_;
    int64_t now_;
    std::vector<uint64_t> words_; // only per-bucket storage; no owner or timestamp arrays
    friend struct DualPointerTestAccess;

    // Eight contiguous 12-bit counters occupy exactly 12 bytes. No padding or
    // persistent unpacked mirror is required; bounded copies avoid tail overreads.
    static __m256i unpack12(const unsigned char* p) {
        uint64_t low; uint32_t high;
        std::memcpy(&low,p,8); std::memcpy(&high,p+8,4);
        const __m128i bytes = _mm_unpacklo_epi64(_mm_cvtsi64_si128(low),_mm_cvtsi32_si128(high));
        const __m128i pairs = _mm_shuffle_epi8(bytes,
            _mm_setr_epi8(0,1,1,2,3,4,4,5,6,7,7,8,9,10,10,11));
        const __m128i aligned = _mm_blend_epi16(pairs,_mm_srli_epi16(pairs,4),0xaa);
        return _mm256_cvtepu16_epi32(_mm_and_si128(aligned,_mm_set1_epi16(4095)));
    }
    static void pack12(unsigned char* p, __m256i values) {
        const __m128i counters = _mm_packus_epi32(_mm256_castsi256_si128(values),
                                               _mm256_extracti128_si256(values,1));
        const __m128i pairs = _mm_madd_epi16(counters,_mm_set1_epi32(0x10000001));
        const __m128i bytes = _mm_shuffle_epi8(pairs,
            _mm_setr_epi8(0,1,2,4,5,6,8,9,10,12,13,14,-1,-1,-1,-1));
        const uint64_t low = uint64_t(_mm_cvtsi128_si64(bytes));
        const uint32_t high = uint32_t(_mm_cvtsi128_si32(_mm_srli_si128(bytes,8)));
        std::memcpy(p,&low,8); std::memcpy(p+8,&high,4);
    }
    void updateClockSimd12() {
        if (now_ >= std::numeric_limits<int>::max())
            throw std::invalid_argument("packet advance outside supported repro time domain");
        const int w=cfg_.width;
        const int64_t f0=now_*cfg_.fastSpeed,f1=f0+cfg_.fastSpeed;
        const int64_t s0=now_*cfg_.slowSpeed,s1=s0+cfg_.slowSpeed;
        const int fp0=int(f0%w),fp1=int(f1%w),sp0=int(s0%w),sp1=int(s1%w);
        const int fc=int(f1/w-f0/w),sc=int(s1/w-s0/w);
        const auto zero=_mm256_setzero_si256(), threshold=_mm256_set1_epi32(cfg_.threshold);
        const auto fastCycles=_mm256_set1_epi32(fc),slowCycles=_mm256_set1_epi32(sc);
        const auto fastStart=_mm256_set1_epi32(fp0),fastEnd=_mm256_set1_epi32(fp1);
        const auto slowStart=_mm256_set1_epi32(sp0),slowEnd=_mm256_set1_epi32(sp1);
        auto crossingValue = [&](int j,int c) {
            const int64_t first=f0+(int64_t(j)+w-fp0)%w;
            const int64_t crossing=first+int64_t(c-cfg_.threshold-1)*w;
            const int64_t through=int64_t(I128(crossing)*cfg_.slowSpeed/cfg_.fastSpeed)+1;
            return int(std::max<int64_t>(0,int64_t(cfg_.threshold)-(before(s1,j,w)-before(through,j,w))));
        };
        int j=0; auto* bytes=reinterpret_cast<unsigned char*>(words_.data());
        for (;j+8<=w;j+=8) {
            const auto c=unpack12(bytes+size_t(j)*3/2);
            const auto positions=_mm256_add_epi32(_mm256_set1_epi32(j),_mm256_setr_epi32(0,1,2,3,4,5,6,7));
            // Comparison masks are -1; subtracting them adds the visit indicator.
            const auto fv=_mm256_add_epi32(_mm256_sub_epi32(fastCycles,_mm256_cmpgt_epi32(fastEnd,positions)),_mm256_cmpgt_epi32(fastStart,positions));
            const auto sv=_mm256_add_epi32(_mm256_sub_epi32(slowCycles,_mm256_cmpgt_epi32(slowEnd,positions)),_mm256_cmpgt_epi32(slowStart,positions));
            const auto fine=_mm256_cmpgt_epi32(c,threshold);
            const auto fastNext=_mm256_sub_epi32(c,fv);
            auto next=_mm256_max_epi32(zero,_mm256_blendv_epi8(_mm256_sub_epi32(c,sv),fastNext,fine));
            const auto crossing=_mm256_andnot_si256(_mm256_cmpgt_epi32(fastNext,threshold),fine);
            unsigned mask=unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(crossing)));
            if (mask) {
                alignas(32) int current[8],updated[8];
                _mm256_store_si256(reinterpret_cast<__m256i*>(current),c);
                _mm256_store_si256(reinterpret_cast<__m256i*>(updated),next);
                while(mask) {const int lane=__builtin_ctz(mask);updated[lane]=crossingValue(j+lane,current[lane]);mask&=mask-1;}
                next=_mm256_load_si256(reinterpret_cast<const __m256i*>(updated));
            }
            pack12(bytes+size_t(j)*3/2,next);
        }
        for (;j<w;++j) {
            const int c=read(j),fv=fc+(j<fp1)-(j<fp0),sv=sc+(j<sp1)-(j<sp0);
            const int next=c>cfg_.threshold ? (c-fv>cfg_.threshold ? c-fv : crossingValue(j,c)) : std::max(0,c-sv);
            write(j,next);
        }
        ++now_;
    }

    static I128 floorDiv(I128 n, I128 d) {
        I128 q = n / d, r = n % d;
        return q - (r < 0);
    }
    static I128 ceilDiv(I128 n, I128 d) { return -floorDiv(-n, d); }
    static int64_t before(int64_t total, int j, int w) {
        return total / w + (j < total % w);
    }
    int read(int j) const {
        const uint64_t bit = uint64_t(j) * cfg_.bits;
        const size_t word = size_t(bit >> 6);
        const unsigned offset = unsigned(bit & 63);
        uint64_t value = words_[word] >> offset;
        if (offset + cfg_.bits > 64) value |= words_[word+1] << (64-offset);
        return int(value & uint64_t(maximum_));
    }
    void write(int j, int value) {
        const uint64_t bit = uint64_t(j) * cfg_.bits;
        const size_t word = size_t(bit >> 6);
        const unsigned offset = unsigned(bit & 63);
        const uint64_t mask = uint64_t(maximum_);
        words_[word] = (words_[word] & ~(mask << offset)) | (uint64_t(value) << offset);
        if (offset + cfg_.bits > 64) {
            const unsigned highBits = offset + cfg_.bits - 64;
            const uint64_t highMask = (uint64_t(1) << highBits) - 1;
            words_[word+1] = (words_[word+1] & ~highMask) | (uint64_t(value) >> (64-offset));
        }
    }
    int originalFineEstimate(const Positions& p) const {
        // Same Min clamp, distance ordering, missing-neighbor fallback and rounding
        // as repro. The constant offset 65535-K cancels from the trajectory formula.
        int minimum = maximum_ - 1;
        for (int j : p) minimum = std::min(minimum,read(j));
        const int pointer = fastPointer(), w = cfg_.width;
        auto distance = [=](int j) { return j <= pointer ? pointer-j : pointer-j+w; };
        int maxDistance = 0, minDistance = w;
        for (int j : p) if (read(j) == minimum) maxDistance = std::max(maxDistance,distance(j));
        bool found = false;
        for (int j : p) if (read(j) == minimum+1) {
            const int d = distance(j);
            if (d > maxDistance && d < minDistance) { minDistance=d; found=true; }
        }
        if (!found) minDistance=maxDistance=w;
        return int((int64_t(w)*(maximum_-1-minimum)+(minDistance+maxDistance)/2
                    +cfg_.fastSpeed/2-1)/cfg_.fastSpeed);
    }
};
#pragma pack(pop)
#endif
