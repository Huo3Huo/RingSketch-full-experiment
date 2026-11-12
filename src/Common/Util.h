#pragma once
#ifndef UTIL_H
#define UTIL_H

#include <x86intrin.h>

// #include <sys/io.h>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <unordered_map>
// REPRO-002: Reader ownership and errors; include before upstream packing pragma.
#include <memory>
#include <stdexcept>
#include <cmath>

#include "hash.h"

typedef int Clock_t;

#pragma pack(1)

#define MAX_TRAFFIC 6

enum HHKeyType{
    five_tuples = 1,
    srcIP_dstIP = 2,
    srcIP_srcPort = 3,
    dstIP_dstPort = 4,
    srcIP = 5,
    dstIP = 6,
};

enum HHHKeyType{
    oneD = 1,
    twoD = 2,
};

typedef double TIMESTAMP;

// struct TIMESTAMP{
//     uint8_t array[8];
// };

#define TUPLES_LEN 13

struct TUPLES{
    uint8_t data[TUPLES_LEN];

    inline uint32_t srcIP() const{
        return *((uint32_t*)(data));
    }

    inline uint32_t dstIP() const{
        return *((uint32_t*)(&data[4]));
    }

    inline uint16_t srcPort() const{
        return *((uint16_t*)(&data[8]));
    }

    inline uint16_t dstPort() const{
        return *((uint16_t*)(&data[10]));
    }

    inline uint8_t proto() const{
        return *((uint8_t*)(&data[12]));
    }

    inline uint64_t srcIP_dstIP() const{
        return *((uint64_t*)(data));
    }

    inline uint64_t srcIP_srcPort() const{
        uint64_t ip = srcIP();
        uint64_t port = srcPort();
        return ((ip << 32) | port);
    }

    inline uint64_t dstIP_dstPort() const{
        uint64_t ip = dstIP();
        uint64_t port = dstPort();
        return ((ip << 32) | port);
    }
}EMPTY;

bool operator == (const TUPLES& a, const TUPLES& b){
    return memcmp(a.data, b.data, sizeof(TUPLES)) == 0;
}

namespace std{
    template<>
    struct hash<TUPLES>{
        size_t operator()(const TUPLES& item) const noexcept
        {
            return Hash::BOBHash32((uint8_t*)&item, sizeof(TUPLES), 0);
        }
    };
}

typedef int32_t COUNT_TYPE;

typedef std::chrono::high_resolution_clock::time_point TP;

inline TP now(){
    return std::chrono::high_resolution_clock::now();
}

inline double durationms(TP finish, TP start){
    return std::chrono::duration_cast<std::chrono::duration<double,std::ratio<1,1000000>>>(finish - start).count();
}

template<typename T>
T Median(std::vector<T> vec, uint32_t len){
    std::sort(vec.begin(), vec.end());
    return (len & 1) ? vec[len >> 1] : (vec[len >> 1] + vec[(len >> 1) - 1]) / 2.0;
}

std::pair<TUPLES*, TIMESTAMP*> read_data(const char *PATH, const  uint64_t length, uint64_t *cnt) {
    // REPRO-002 BEGIN: checked 21-byte records, bounded reads, exception-safe ownership.
    if (!length || !cnt) throw std::runtime_error("empty/invalid reader request");
    std::unique_ptr<FILE, decltype(&fclose)> data(fopen(PATH, "rb"), fclose);
    if (!data) throw std::runtime_error("cannot open CAIDA input");
    std::unique_ptr<TUPLES[]> items(new TUPLES[length]);
    std::unique_ptr<TIMESTAMP[]> timestamps(new TIMESTAMP[length]);
    *cnt = 0;
    unsigned char buffer[21 * 4096];
    while (*cnt < length) {
        const size_t want = std::min<uint64_t>(4096, length - *cnt);
        const size_t got = fread(buffer, 1, want * 21, data.get());
        if (ferror(data.get()) || got % 21) throw std::runtime_error("truncated/failed CAIDA record");
        for (size_t j = 0; j < got / 21; ++j) {
            memcpy(items[*cnt].data, buffer + j * 21, 13);
            memcpy(&timestamps[*cnt], buffer + j * 21 + 13, 8);
            if (!std::isfinite(timestamps[*cnt])) throw std::runtime_error("nonfinite timestamp");
            ++*cnt;
        }
        if (got != want * 21) throw std::runtime_error("unexpected CAIDA EOF");
    }
    return std::make_pair(items.release(), timestamps.release());
    // REPRO-002 END
}

TUPLES* read_data(const char *PATH, const  uint64_t length, uint64_t *cnt, uint32_t key_length) {
    TUPLES *items = new TUPLES[length];
    TUPLES *it = items;
    memset(items, 0, sizeof(TUPLES) * length);

    FILE *data = fopen(PATH, "rb");
    *cnt = 0;
    while (fread(it++, key_length, 1, data) > 0) {
        (*cnt)++;
        if ((*cnt) >= length)
            break;
    }

    fclose(data);

    return items;
}

std::string double2string(double val, int precision) {
    std::stringstream ss;
    std::string str;
    ss << std::setprecision(precision) << val;
    ss >> str;
    return str;
}

struct ItemPair{
    int time;
    TUPLES item;

    ItemPair(int _time = 0, TUPLES _item = EMPTY):
        time(_time), item(_item){}
};

bool operator == (const ItemPair& a, const ItemPair& b){
    return memcmp(&a, &b, sizeof(ItemPair)) == 0;
}

namespace std{
    template<>
    struct hash<ItemPair>{
        size_t operator()(const ItemPair& item) const noexcept
        {
            return Hash::BOBHash32((uint8_t*)&item, sizeof(ItemPair), 0);
        }
    };
}

#endif
