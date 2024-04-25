#pragma once
#include "bwe/Estimator.h"
#include "bwe/FlankLatch.h"
#include "math/Matrix.h"
#include "utils/Trackers.h"
#include <array>
#include <cstdint>

namespace bwe
{

struct Config
{
    double alpha = 0.7;
    double kappa = 3.0;
    double beta = 2.0;
    double measurementNoise = 100.1 * 3;
    double maxNetworkQueue = 500 * 1024;
    const double modelMinBandwidth = 25.0;

    struct Estimate
    {
        double minKbps = 250;
        double maxKbps = 500000;
        double initialKbpsDownlink = 200;
        double initialKbpsUplink = 0;
        double minReportedKbps = 500;
    } estimate;

    struct Congestion
    {
        double backOff = 0.4;
        // in seconds
        double recoveryTime = 10.1;
        double thresholdMs = 500.0;

        // estimate cap after repeated congestion events
        struct
        {
            uint32_t congestionEventLimit = 1;

            // cap estimate at this level of average link estimate
            double ratio = 0.5;
            // max choke time before bw floor is cancelled
            uint32_t chokeToleranceMs = 1000.0;
        } cap;
    } congestion;

    struct Silence
    {
        uint32_t timeoutMs = 900;
        double backOff = 0.5;
        double maxBandwidthKbps = 500.0;
    } silence;

    const uint32_t mtu = 1470;

    // use this before initializing Estimator as some values can cause division by zero
    void sanitize();
};

class BandwidthEstimator : public Estimator
{
    static const int DIMENSIONALITY = 3 + 3 + 1; // state, process noise, obs noise
    static const int SIGMA_POINTS = DIMENSIONALITY * 2 + 1;

public:
    explicit BandwidthEstimator(const Config& config);

    void init(double clockOffset);

    void update(uint32_t packetSize, uint64_t transmitTimeNs, uint64_t receiveTimeNs) override;
    void onUnmarkedTraffic(uint32_t packetSize, uint64_t receiveTimeNs) override;

    math::Matrix<double, 3> getState() const { return _state; }
    math::Matrix<double, 3> getCovariance() const;
    double getEstimate(uint64_t timestamp) const override;
    double getDelay() const override;
    double predictDelay() const;

    // kbps
    double getReceiveRate(uint64_t timestamp) const override
    {
        return utils::Time::ms * _receiveBitrate.get(timestamp, utils::Time::ms * 750);
    }

    void reset();

private:
    void generateSigmaPoints(const math::Matrix<double, 3>& state,
        const math::Matrix<double, 3, 3>& covP,
        const math::Matrix<double, 3>& processNoise,
        std::array<math::Matrix<double, 3>, SIGMA_POINTS>& sigmaPoints);

    math::Matrix<double, 3> transitionState(uint32_t packetSize, double tau, const math::Matrix<double, 3>& prevState);
    double predictAbsoluteDelay(const math::Matrix<double, 3>& state) const;

    void updateCongestionMargin(double packetIntervalMs);
    double analyseCongestion(double actualDelay, uint32_t packetSize, uint64_t timestamp);
    void calculateProcessNoise(const math::Matrix<double, 3>& currentState,
        double actualDelay,
        uint32_t packetSize,
        uint64_t receiveTimeNs,
        math::Matrix<double, 3>& processNoise,
        double& measurementNoise);

    const Config _config;
    uint64_t _baseClockOffset;
    const double _lambda;
    math::Matrix<double, 3> _state; // Queue bits, Bandwidth kbps, clockOffset ms
    math::Matrix<double, 3, 3> _covarianceP;
    const math::Matrix<double, 3> _processNoise; // Q, Bw, offset  to control the filter

    const double _weightCovariance0;
    const double _weightCovariance;
    const double _weightMean;
    const double _weightMean0;
    const double _sigmaWeight;
    // in bits per nanosecond
    utils::RateTracker<40> _receiveBitrate;
    uint64_t _previousTransmitTime;
    uint64_t _previousReceiveTime;
    double _observedDelay;
    double _packetSize0; // packet size at clock offset reset

    struct ClockOffsetTrack
    {
        uint32_t packet0Size; // first packet in queue
        uint64_t packet0;
    };

    struct CongestionDips
    {
        static constexpr double maxCap = 100000.0;

        uint32_t count = 0;
        double intensity = 0;
        double bandwidthCapKbps = maxCap;
        double bandwidthFloorKbps = 0.0;
    };
    struct CongestionState
    {
        explicit CongestionState(double margin);
        void onNewEstimate(double kbps);

        double margin;
        utils::RateTracker<10> bandwidth;
        int packetCount;
        uint64_t start;
        double avgEstimate;

        CongestionDips dip;
        FlankLatch congestionTrigger;
    } _congestion;
};

} // namespace bwe
