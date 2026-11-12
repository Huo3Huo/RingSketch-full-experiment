#ifndef ABSTRACT_H
#define ABSTRACT_H

#include <unordered_map>

#include <string.h>

#include "../Common/Util.h"

class Abstract {
public:
    std::string name;

    Abstract(){}
    virtual ~Abstract() {};

    virtual void insert(TUPLES x, int t) = 0;
    virtual void updateClock(int insertTimesPerUpdate = 1) = 0;
    virtual int estimate(TUPLES x, int t) = 0;
    virtual void updateRange(int beg, int end, int val) = 0;
    virtual double cardinalEst(int threshold, int t) {
        return 0.0;
    } 
};

template<typename DATA_TYPE>
class CacheAbstract{
public:
    bool haveLastEvicted;
    DATA_TYPE lastEvicted;

    virtual void insert(const DATA_TYPE& item) = 0;
    virtual bool inCache(const DATA_TYPE& item) = 0;
    virtual std::vector<DATA_TYPE> getItems() = 0;
    virtual void updateClock(int insertTimesPerUpdate) = 0;
};

#endif
