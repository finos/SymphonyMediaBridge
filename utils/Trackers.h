#pragma once
#include "Time.h"
#include <algorithm>
#include <atomic>
#include <inttypes.h>
namespace utils
{

// Lock free rolling average tracker
class AvgTracker
{
public:
    explicit AvgTracker(double alpha, double startValue = 0) : _value(startValue), _alpha(alpha) {}

    void update(double value);

    double get() const { return _value.load(); }

private:
    std::atomic<double> _value;
    const double _alpha;
};

// Lock free max tracker with exponential decay
class MaxTracker
{
public:
    explicit MaxTracker(double decay, double startValue = 0) : _value(startValue), _decay(decay) {}

    void update(double value);

    double get() const { return _value.load(); }

private:
    std::atomic<double> _value;
    const double _decay;
};

// Single threaded max tracker
class MaxTrackerPlain
{
public:
    explicit MaxTrackerPlain(double decay, double startValue = 0) : _value(startValue), _decay(decay) {}

    void update(double value);
    void reset(double value) { _value = value; }
    double get() const { return _value; }

private:
    double _value;
    const double _decay;
};

class AvgRateTracker
{
public:
    explicit AvgRateTracker(double alpha) : _value(0), _avgTime(0), _alpha(alpha), _prevTimestamp(0) {}

    void update(double value, uint64_t timestamp);
    double get() const;
    double get(uint64_t timestamp, uint64_t maxTimeSinceSample) const;
    void set(double value, uint64_t interval);

private:
    double _value;
    double _avgTime;
    const double _alpha;
    uint64_t _prevTimestamp;
};

class TimeTracker
{
public:
    TimeTracker() : _start(Time::getApproximateTime()), _stopTime(_start - 1) {}

    void stop();
    uint64_t getElapsed() const;

private:
    uint64_t _start;
    uint64_t _stopTime;
};

template <size_t BUCKET_COUNT>
class RateTracker
{
public:
    explicit RateTracker(uint64_t bucketInterval) : _pos(0), _bucketInterval(bucketInterval), _prevTimestamp(0) {}

    void update(double itemSize, uint64_t timestamp)
    {
        auto bucket = &_buckets[_pos];
        if (static_cast<int64_t>(timestamp - bucket->start) > static_cast<int64_t>(_bucketInterval))
        {
            _pos = (_pos + 1) % BUCKET_COUNT;
            bucket = &_buckets[_pos];
            bucket->size = 0;
            bucket->start =
                _prevTimestamp == 0 && timestamp > _bucketInterval ? timestamp - _bucketInterval : _prevTimestamp;
        }
        bucket->size += itemSize;
        _prevTimestamp = timestamp;
    }

    double get(const uint64_t timestamp, const uint64_t desiredInterval) const
    {
        double totalSize = 0;
        int64_t duration = timestamp - _prevTimestamp;
        const int64_t interval = static_cast<int64_t>(desiredInterval);
        if (duration < 0 || duration > interval)
        {
            return 0;
        }

        const int currentPos = _pos + BUCKET_COUNT;
        for (size_t i = 0; i < BUCKET_COUNT; ++i)
        {
            const auto& bucket = _buckets[(currentPos - i) % BUCKET_COUNT];
            const auto newDuration = static_cast<int64_t>(timestamp - bucket.start);
            if (newDuration > interval)
            {
                break;
            }
            duration = newDuration;
            totalSize += bucket.size;
        }

        if (duration < interval / 4)
        {
            return 0;
        }

        return totalSize / static_cast<double>(std::max(int64_t(1), duration));
    }

    // read using previous submission timestamp.
    // Will report last bit rate also after a long pause
    double get(const uint64_t interval) const { return get(_prevTimestamp, interval); }

private:
    struct Bucket
    {
        Bucket() : start(0), size(0) {}
        uint64_t start;
        double size;
    };

    Bucket _buckets[BUCKET_COUNT];
    size_t _pos;
    const uint64_t _bucketInterval;
    uint64_t _prevTimestamp;
};

template <size_t NUM_BUCKETS, uint64_t BUCKET_SIZE_MS, uint64_t SNAPSHOT_DELTA_MS>
struct TrackerWithSnapshot
{
    TrackerWithSnapshot() : tracker(BUCKET_SIZE_MS), _lastSnapshotUpdatedAt(0) { snapshot.store(0.0); }
    utils::RateTracker<NUM_BUCKETS> tracker;
    std::atomic<double> snapshot;
    void update(double itemSize, uint64_t timestamp)
    {
        tracker.update(itemSize, timestamp);
        if (timestamp - _lastSnapshotUpdatedAt > (SNAPSHOT_DELTA_MS >> 1))
        {
            snapshot.store(tracker.get(timestamp, SNAPSHOT_DELTA_MS * utils::Time::ms));
            _lastSnapshotUpdatedAt = timestamp;
        }
    }

private:
    uint64_t _lastSnapshotUpdatedAt;
};

class TimeGuard
{
public:
    TimeGuard(uint64_t maxUs, int lineNumber, const char* name, const char* fileName)
        : _maxTime(maxUs * 1000),
          _lineNumber(lineNumber),
          _name(name),
          _fileName(fileName)
    {
    }

    ~TimeGuard() { check(); }

    void check();

private:
    uint64_t _maxTime;
    const int _lineNumber;
    const char* _name;
    const char* _fileName;
    TimeTracker _timeTracker;
};
#define TOKENPASTE(x, y) x##y
#define TOKENCONCAT(x, y) TOKENPASTE(x, y)
#define TIME_GUARD(x, t) utils::TimeGuard TOKENCONCAT(x, __LINE__)(t, __LINE__, #x, __FILE__)
} // namespace utils
