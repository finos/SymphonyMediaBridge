#include "bwe/BandwidthEstimator.h"
#include "utils/Time.h"
#include <algorithm>
#include <array>

namespace bwe
{
void Config::sanitize()
{
    congestion.recoveryTime = std::max(congestion.recoveryTime, 1.0); // must not be zero
    estimate.initialKbpsDownlink = std::max(100.0, estimate.initialKbpsDownlink); // must not be zero
}

namespace
{
enum StateVariables
{
    QueuedBits = 0,
    Bandwidth,
    ClockOffset
};
}

BandwidthEstimator::CongestionState::CongestionState(double margin_)
    : margin(margin_),
      bandwidth(20 * utils::Time::ms),
      packetCount(0),
      start(0),
      avgEstimate(0),
      congestionTrigger(500.0, 100.0)
{
}

void BandwidthEstimator::CongestionState::onNewEstimate(double kbps)
{
    avgEstimate = (avgEstimate == 0 ? kbps : avgEstimate + 0.001 * (kbps - avgEstimate));
    dip.intensity -= 0.0005 * dip.intensity;
}

BandwidthEstimator::BandwidthEstimator(const Config& config)
    : _config(config),
      _baseClockOffset(0),
      _lambda(_config.alpha * _config.alpha * (DIMENSIONALITY + _config.kappa)),
      _processNoise({0, 40, 0.001}),
      _weightCovariance0((_lambda / (DIMENSIONALITY + _lambda)) + (1 + _config.beta - _config.alpha * _config.alpha)),
      _weightCovariance(1.0 / (2.0 * (DIMENSIONALITY + _lambda))),
      _weightMean(_weightCovariance),
      _weightMean0(1.0 - _weightMean * DIMENSIONALITY * 2.0),
      _sigmaWeight(sqrt(DIMENSIONALITY + _lambda)),
      _receiveBitrate(50 * utils::Time::ms),
      _previousTransmitTime(0),
      _previousReceiveTime(0),
      _observedDelay(0),
      _packetSize0(0),
      _congestion(0.0)
{
    _state(Bandwidth) = _config.estimate.initialKbpsDownlink;

    const math::Matrix<double, 3> initDelta({8000.0 * 8, _config.estimate.initialKbpsDownlink * 0.001, 0.1});
    _covarianceP = initDelta * math::transpose(initDelta);
}

void BandwidthEstimator::onUnmarkedTraffic(uint32_t packetSize, uint64_t receiveTimeNs)
{
    if (_baseClockOffset != 0 && _state(QueuedBits) < 1600 * 8)
    {
        _state(QueuedBits) = _state(QueuedBits) + packetSize * 8;
    }
    _previousReceiveTime = receiveTimeNs;
    _receiveBitrate.update(packetSize * 8, receiveTimeNs);
}

double BandwidthEstimator::predictDelay() const
{
    const double offsetAdjustment = _packetSize0 * 8 / _state(Bandwidth);
    return predictAbsoluteDelay(_state) - _state(ClockOffset) + offsetAdjustment;
}

void BandwidthEstimator::update(uint32_t packetSize, uint64_t transmitTimeNs, uint64_t receiveTimeNs)
{
    if (_baseClockOffset == 0 && _state(QueuedBits) == 0 && _previousTransmitTime == 0)
    {
        // base Offset is very sensitive, if you start 5ms behind it will create a lower estimate, assuming higher delay
        // and longer queue. Starting with a long queue is also a bad start
        _baseClockOffset = receiveTimeNs - transmitTimeNs;
        _previousTransmitTime = transmitTimeNs - 5 * utils::Time::sec;
        _previousReceiveTime = receiveTimeNs - 5 * utils::Time::ms;
        _packetSize0 = packetSize;
    }

    const double tau = std::max(0.0, static_cast<double>(transmitTimeNs - _previousTransmitTime) / utils::Time::ms);
    const double observedDelay =
        static_cast<double>(static_cast<int64_t>(receiveTimeNs - transmitTimeNs - _baseClockOffset)) / utils::Time::ms;

    const auto expectedState = transitionState(packetSize, tau, _state);
    const auto expectedDelay = predictAbsoluteDelay(expectedState);

    auto actualDelay = (observedDelay - _state(ClockOffset));
    if (actualDelay < 0)
    {
        _state(QueuedBits) = 0; // queue must be empty before this packet
        _state(ClockOffset) = observedDelay;
        actualDelay = 0;
        _packetSize0 = packetSize;
    }
    else if (actualDelay == 0 && packetSize > _packetSize0)
    {
        _packetSize0 = packetSize;
    }
    else if (observedDelay > expectedDelay && tau > 20.0 && expectedState(QueuedBits) < _config.mtu * 8 * 3)
    {
        // inter transmit interval larger than 20ms risk having radio shut down. adjust co up
        _state(ClockOffset) += std::min(30.0, observedDelay - expectedDelay);
        _packetSize0 = packetSize;
    }

    auto processNoise = _processNoise;
    double measurementNoise = _config.measurementNoise;
    calculateProcessNoise(transitionState(packetSize, tau, _state),
        actualDelay,
        packetSize,
        receiveTimeNs,
        processNoise,
        measurementNoise);

    _receiveBitrate.update(packetSize * 8, receiveTimeNs);

    // generate alternative current positions
    std::array<math::Matrix<double, 3>, SIGMA_POINTS> sigmaPoints;
    generateSigmaPoints(_state, _covarianceP, processNoise, sigmaPoints);

    // calculate where we would have transitioned from alternative positions, and what the delay would be then
    std::array<double, SIGMA_POINTS> predictedDelays;
    for (size_t i = 0; i < sigmaPoints.size(); ++i)
    {
        sigmaPoints[i] = transitionState(packetSize, tau, sigmaPoints[i]);
        predictedDelays[i] = predictAbsoluteDelay(sigmaPoints[i]);
    }
    const double predictedMeanDelay = predictedDelays[0]; // delay of mean state, because 1/bw is non-linear

    predictedDelays[SIGMA_POINTS - 2] += measurementNoise;
    predictedDelays[SIGMA_POINTS - 1] -= measurementNoise;

    math::Matrix<double, 3> predictedMeanState;
    for (size_t i = 1; i < sigmaPoints.size(); ++i)
    {
        predictedMeanState += sigmaPoints[i];
    }
    predictedMeanState *= _weightMean;
    predictedMeanState += sigmaPoints[0] * _weightMean0;

    // calculate variance
    math::Matrix<double, 3, 3> statePredictionCovariance;
    for (auto& point : sigmaPoints)
    {
        point = (point - predictedMeanState);
    }

    for (size_t i = 1; i < sigmaPoints.size(); ++i)
    {
        statePredictionCovariance += math::outerProduct(sigmaPoints[i]);
    }
    statePredictionCovariance *= _weightCovariance;
    statePredictionCovariance += (_weightCovariance0 * math::outerProduct(sigmaPoints[0]));
    assert(math::isValid(statePredictionCovariance));

    const auto residual0 = predictedDelays[0] - predictedMeanDelay;
    double covDelay = _weightCovariance0 * residual0 * residual0;
    math::Matrix<double, 3> crossCovariance;
    for (size_t i = 1; i < predictedDelays.size(); ++i)
    {
        const auto residual = predictedDelays[i] - predictedMeanDelay;
        covDelay += _weightCovariance * residual * residual;
        crossCovariance += residual * sigmaPoints[i];
    }
    crossCovariance *= _weightCovariance;
    crossCovariance += _weightCovariance0 * residual0 * sigmaPoints[0];

    // update position towards mean position
    const auto kalmanGain = crossCovariance * (1.0 / covDelay);
    _state = predictedMeanState + kalmanGain * (observedDelay - predictedMeanDelay);
    _covarianceP = statePredictionCovariance - crossCovariance * math::transpose(kalmanGain);

    _state(Bandwidth) = std::max(_state(Bandwidth), _config.modelMinBandwidth);
    _state(Bandwidth) = std::min(_config.estimate.maxKbps, _state(Bandwidth));
    sanitizeQueue(observedDelay, _state);
    _state(ClockOffset) = std::min(observedDelay, _state(ClockOffset));

    assert(math::isValid(_covarianceP));
    math::makeSymmetric(_covarianceP);
    assert(math::isSymmetric(_covarianceP));
    assert(math::isValid(_covarianceP));

    _observedDelay = observedDelay;

    updateCongestionMargin(
        utils::Time::diff(_previousReceiveTime, receiveTimeNs) / static_cast<double>(utils::Time::ms));

    _previousReceiveTime = receiveTimeNs;
    _previousTransmitTime = transmitTimeNs;
}

void BandwidthEstimator::sanitizeQueue(const double observedDelay, math::Matrix<double, 3>& state)
{
    state(QueuedBits) = std::max(0.0, state(QueuedBits));

    if (observedDelay - predictAbsoluteDelay(state) < 0 && state(QueuedBits) > _config.mtu * 3)
    {
        // adjust queue if we received packet earlier than expected. If bw is higher than expected, queue should also
        // have drained more
        const double delayErr = predictAbsoluteDelay(state) - observedDelay;
        state(QueuedBits) -= delayErr * state(Bandwidth) / 3;
        state(QueuedBits) = std::max(0.0, state(QueuedBits));
    }

    state(QueuedBits) = std::max(0.0, std::min(state(QueuedBits), _config.maxNetworkQueue * 8));
}

void BandwidthEstimator::calculateProcessNoise(const math::Matrix<double, 3>& currentState,
    double actualDelay,
    uint32_t packetSize,
    uint64_t receiveTimeNs,
    math::Matrix<double, 3>& processNoise,
    double& measurementNoise)
{
    if (actualDelay * currentState(Bandwidth) < currentState(QueuedBits) &&
        currentState(QueuedBits) > _config.mtu * 2 * 8)
    {
        // queue is longer than expected and > 2pkts. Trust observation more and adjust bw more
        processNoise(Bandwidth) = 200;
        measurementNoise = _config.measurementNoise * 0.5;
        return;
    }

    measurementNoise = _config.measurementNoise * analyseCongestion(actualDelay, packetSize, receiveTimeNs);
}

double BandwidthEstimator::analyseCongestion(double actualDelay, const uint32_t packetSize, const uint64_t timestamp)
{
    _congestion.onNewEstimate(_state(Bandwidth));
    double congestionScale = 1.0;

    if (actualDelay > 90.0 && _state(QueuedBits) < _config.mtu * 100.0 * 8 &&
        actualDelay * _state(Bandwidth) > (packetSize * 8 + _state(QueuedBits) + _config.mtu * 10))
    {
        // unexpected high delay and low bit rate
        // Q is less than 100 pkts
        _congestion.bandwidth.update(packetSize * 8, timestamp);
        if (_congestion.packetCount == 0)
        {
            _congestion.start = timestamp;
        }
        const auto congestionDuration = timestamp - _congestion.start;

        if (++_congestion.packetCount > 16 &&
            _congestion.bandwidth.get(timestamp, congestionDuration) * utils::Time::ms < _state(Bandwidth))
        {
            // persistent, no sign of burst delivery at high rate
            const auto congestionBandwidth = _congestion.bandwidth.get(timestamp, congestionDuration) * utils::Time::ms;

            // make bwe sensitive to observation and reset queue and bandwidth for faster adaptation
            _state(Bandwidth) = congestionBandwidth;
            _state(QueuedBits) = actualDelay * congestionBandwidth - packetSize * 8;
            congestionScale = 0.1;
            _congestion.packetCount = 0;
        }
    }
    else
    {
        _congestion.packetCount = 0;
    }

    const auto congestionStatus = _congestion.congestionTrigger.update(actualDelay);
    if (FlankLatch::switchOn == congestionStatus)
    {
        if (++_congestion.dip.count > _config.congestion.cap.congestionEventLimit)
        {
            _congestion.dip.intensity = 1.0;
        }
    }

    if (_congestion.dip.intensity < 0.1)
    {
        _congestion.dip.bandwidthCapKbps = CongestionDips::maxCap;
        _congestion.dip.bandwidthFloorKbps = 0;
    }
    else
    {
        _congestion.dip.bandwidthCapKbps =
            std::max(_config.estimate.minKbps, _congestion.avgEstimate * _config.congestion.cap.ratio);
        if (_congestion.dip.bandwidthCapKbps < CongestionDips::maxCap &&
            utils::Time::diffLT(_congestion.start,
                timestamp,
                utils::Time::ms * _config.congestion.cap.chokeToleranceMs))
        {
            _congestion.dip.bandwidthFloorKbps = _congestion.dip.bandwidthCapKbps;
        }
        else
        {
            _congestion.dip.bandwidthFloorKbps = 0;
        }
    }
    return congestionScale;
}

// in kbps
double BandwidthEstimator::getEstimate(uint64_t timestamp) const
{
    const double estimatedBandwidth = std::min(_state(Bandwidth), _congestion.dip.bandwidthCapKbps);

    if (_previousReceiveTime != 0 &&
        utils::Time::diffGT(_previousReceiveTime, timestamp, _config.silence.timeoutMs * utils::Time::ms))
    {
        return std::max(_config.estimate.minReportedKbps,
            std::min(_config.silence.maxBandwidthKbps, estimatedBandwidth * (1.0 - _config.silence.backOff)));
    }
    return std::max(std::max(_congestion.dip.bandwidthFloorKbps, _config.estimate.minReportedKbps),
        estimatedBandwidth * (1.0 - _congestion.margin));
}

// in ms
double BandwidthEstimator::getDelay() const
{
    return (_observedDelay - _state(ClockOffset) + (_packetSize0 * 8 / _state(Bandwidth)));
}

math::Matrix<double, 3> BandwidthEstimator::getCovariance() const
{
    math::Matrix<double, 3> r;
    for (int i = 0; i < 3; ++i)
    {
        r(i) = _covarianceP(i, i);
    }
    return r;
}

// generate alternative current positions based on noise in model and process
void BandwidthEstimator::generateSigmaPoints(const math::Matrix<double, 3>& state,
    const math::Matrix<double, 3, 3>& covP,
    const math::Matrix<double, 3>& processNoise,
    std::array<math::Matrix<double, 3>, SIGMA_POINTS>& sigmaPoints)
{
    static const auto seed = covP.I() * 0.0000001; // will make it positive definite
    const auto squareRoot = math::choleskyDecompositionLL(covP + seed);
    sigmaPoints[0] = state;

    int startIndex = 1;
    for (int c = 0; c < squareRoot.columns(); ++c)
    {
        auto sigmaOffset = _sigmaWeight * squareRoot.getColumn(c);

        sigmaPoints[startIndex + 2 * c] = state + sigmaOffset;
        sigmaPoints[startIndex + 2 * c + 1] = state - sigmaOffset;
    }

    startIndex += 2 * squareRoot.columns();
    for (int i = 0; i < processNoise.rows(); ++i)
    {
        math::Matrix<double, 3> noise;
        noise(i) = processNoise(i) * _sigmaWeight;

        sigmaPoints[startIndex + i * 2] = state + noise;
        sigmaPoints[startIndex + i * 2 + 1] = state - noise;

        // sigmaPoints[startIndex + i * 2] = state + noise;
        //  Offset the positive clock offset sigma point. Better catch up to clock drift.
        //  CO is easily pushed down by packets with short delay.
        // sigmaPoints[startIndex + i * 2 + 1] = state - (i == 2 ? noise : noise);
    }

    // add two points for measurement noise
    startIndex += 2 * processNoise.rows();
    sigmaPoints[startIndex] = sigmaPoints[0];
    sigmaPoints[startIndex + 1] = sigmaPoints[0];

    const double maxBw = std::max(state(Bandwidth), _config.modelMinBandwidth) * 2 - _config.modelMinBandwidth;
    for (auto& point : sigmaPoints)
    {
        point(QueuedBits) = state(QueuedBits);
        // point(0) = std::max(0.0, point(0)); // no magic Q reduction varying the Q causes bw overshoot
        point(Bandwidth) = std::max(_config.modelMinBandwidth, std::min(maxBw, point(1)));
    }
}

math::Matrix<double, 3> BandwidthEstimator::transitionState(const uint32_t packetSize,
    const double tau,
    const math::Matrix<double, 3>& prevState)
{
    const auto bw = std::max(0.0, std::min(_config.estimate.maxKbps, prevState(Bandwidth)));
    return math::Matrix<double, 3>(
        {std::max(0.0, prevState(QueuedBits) - bw * tau) + packetSize * 8, bw, prevState(ClockOffset)});
}

void BandwidthEstimator::updateCongestionMargin(double packetIntervalMs)
{
    if (_state(QueuedBits) / _state(Bandwidth) > _config.congestion.thresholdMs && _congestion.margin == 0.0)
    {
        _congestion.margin = std::min(_state(QueuedBits) /
                (_config.congestion.recoveryTime *
                    std::max(_config.estimate.minKbps,
                        std::min(_config.estimate.maxKbps, _receiveBitrate.get(550 * utils::Time::ms)))),
            _config.congestion.backOff);
    }
    else if (_congestion.margin > 0.0)
    {
        _congestion.margin = std::max(0.0,
            _congestion.margin -
                _config.congestion.backOff * packetIntervalMs / (_config.congestion.recoveryTime * 1000));
    }
}

// excluding clock offset, adjusting for clock ref packet size
double BandwidthEstimator::predictAbsoluteDelay(const math::Matrix<double, 3>& state) const
{
    assert(state(Bandwidth) > 0.0);
    assert(math::isValid(state));
    const double offsetAdjustment = _packetSize0 * 8 / state(Bandwidth);
    return (state(QueuedBits) / state(Bandwidth)) + state(ClockOffset) - offsetAdjustment;
}

void BandwidthEstimator::reset()
{
    _state(QueuedBits) = 0;
    _state(Bandwidth) = _config.estimate.initialKbpsDownlink;
    _state(ClockOffset) = 8000;
    const math::Matrix<double, 3> initDelta({8000.0 * 8, _config.estimate.initialKbpsDownlink * 0.001, 0.1});
    _covarianceP = initDelta * math::transpose(initDelta);
}

} // namespace bwe
