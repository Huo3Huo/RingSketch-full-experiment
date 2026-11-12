#ifndef _RingSketchSimd_H_
#define _RingSketchSimd_H_

#include "../Abstract.h"

#include <iostream>
#include <cstring>
#include <algorithm>

class RingSketchSimd : public Abstract {
    typedef unsigned short Clock_t;
    int width;
    int updateSpeed;
    
    Clock_t *clocks;

    int lastUpdateIdx;
    int counterSize;
    int counterNum;
    int counterInterval;
public:
    RingSketchSimd(int _window, int _memory, int _counternum, int _countersize);
    ~RingSketchSimd();
    void insert(TUPLES x, int);
    void updateClock(int insertTimesPerUpdate = 1);
    int estimate(TUPLES x, int);
    void updateRange(int beg, int end, int val);
    std::vector<int> getPos(TUPLES x);
};

RingSketchSimd::RingSketchSimd(int _window, int _memory, int _counternum, int _countersize)
    : lastUpdateIdx(0), counterSize(_countersize), counterNum(_counternum)
{
    this->name = "RingSketchSimd";

    width = 8 * _memory / counterSize;
    counterInterval = width / counterNum;

    updateSpeed = ((1ll << _countersize) - 1) * width / _window;

    clocks = new Clock_t[width];
    memset(clocks, 0, sizeof(Clock_t) * width);
}

RingSketchSimd::~RingSketchSimd()
{
    delete[] clocks;
}

std::vector<int> RingSketchSimd::getPos(TUPLES x) {
    std::vector<int> pos_array;

    for (int i = 0; i < counterNum; i++) {
        int pos = hash(x, 100 + i) % counterInterval + i * counterInterval;
        pos = (pos % width + width) % width;

        pos_array.push_back(pos);
    }

    return pos_array;
}

void RingSketchSimd::insert(TUPLES x, int)
{
    std::vector<int> pos_array = getPos(x);

    for (int i = 0; i < counterNum; i++) {
        int pos = pos_array[i];

        clocks[pos] = (1 << counterSize) - 1;
    }
}

void RingSketchSimd::updateClock(int insertTimesPerUpdate)
{
    int temp = updateSpeed * insertTimesPerUpdate;
    int subAll = temp / width;
    int len = temp % width;
    updateRange(0, width, subAll);

    int beg, end;
    if (lastUpdateIdx + len >= width) {
        beg = lastUpdateIdx, end = (lastUpdateIdx + len) % width;
        updateRange(beg, width, 1);
        updateRange(0, end, 1);
    }else {
        beg = lastUpdateIdx, end = lastUpdateIdx + len;
        updateRange(beg, end, 1);
    }
    lastUpdateIdx = end;
}

int RingSketchSimd::estimate(TUPLES x, int)
{
    Clock_t Min = (1 << counterSize) - 2;

    std::vector<int> pos_array = getPos(x);

    for (int i = 0; i < counterNum; i++) {
        int pos = pos_array[i];

        Min = std::min<Clock_t>(Min, clocks[pos]);
    }

    auto distance = [&](int a, int b) {
        if (a <= b)
            return b - a;
        return b - a + width;
    };

    int pos1 = -1;
    int max_distance = 0;
    for (int i = 0; i < counterNum; i++) {
        int pos = pos_array[i];

        if (clocks[pos] == Min) {
            int d = distance(pos, lastUpdateIdx);
            if (d > max_distance) {
                max_distance = d;
                pos1 = pos;
            }
        }
    }

    int pos2 = -1;
    int min_distance = width;
    for (int i = 0; i < counterNum; i++) {
        int pos = pos_array[i];

        if (clocks[pos] == Min + 1) {
            int d = distance(pos, lastUpdateIdx);
            if (d > max_distance && d < min_distance) {
                min_distance = d;
                pos2 = pos;
            }
        }
    }

    if (pos2 == -1)
        min_distance = max_distance = width;

    return ((1ll * width * ((1 << counterSize) - 2 - Min) + (min_distance + max_distance) / 2) + updateSpeed / 2 - 1)/ updateSpeed;
}

void RingSketchSimd::updateRange(int beg, int end, int val)
{
    if (val <= 0)
        return;

    __m256i _subVal = _mm256_set1_epi16(short(val));
    while (beg + 16 <= end) {
        __m256i clock = _mm256_loadu_si256((__m256i *)&clocks[beg]);
        __m256i subRes = _mm256_subs_epu16(clock, _subVal);
        _mm256_storeu_si256((__m256i *)&clocks[beg], subRes);
        beg += 16;
    }

    for (int j = beg; j < end; j++)
        clocks[j] = std::max(0, clocks[j] - val);
}

#endif //_RingSketchSimd_H_
