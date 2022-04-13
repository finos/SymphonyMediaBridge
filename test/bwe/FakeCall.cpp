#include "FakeCall.h"
#include "FakeAudioSource.h"
#include "FakeCrossTraffic.h"
#include "FakeMedia.h"
#include "bwe/Estimator.h"
#include "test/transport/NetworkLink.h"
namespace fakenet
{

Call::Call(memory::PacketPoolAllocator& allocator,
    bwe::Estimator& estimator,
    NetworkLink* firstLink,
    bool audio,
    uint64_t duration)
    : _bwe(estimator),
      _allocator(allocator),
      _timeCursor(utils::Time::getAbsoluteTime()),
      _endTime(_timeCursor + duration)
{
    addLink(firstLink);
    if (audio)
    {
        auto audio = new FakeAudioSource(_allocator, 90, 0);
        _mediaSources.push_back(audio);
    }
}

Call::~Call()
{
    for (auto* link : _links)
    {
        delete link;
    }

    for (auto* src : _mediaSources)
    {
        delete src;
    }
}

void Call::addLink(NetworkLink* link)
{
    _links.push_back(link);
    logger::info("added link %ukbps", "Call", link->getBandwidthKbps());
}

void Call::addSource(MediaSource* source)
{
    _mediaSources.push_back(source);
}

double Call::getEstimate() const
{
    return _bwe.getEstimate(_timeCursor);
}

// returns false when done
bool Call::run(uint64_t period)
{
    char data[1400];
    std::fill(data, data + 1400, 0xdd);

    uint64_t nextLog = _timeCursor + period;
    for (; utils::Time::diff(_timeCursor, _endTime) > 0;)
    {
        const auto t = _timeCursor;
        int64_t timeAdvance = utils::Time::diff(_timeCursor, nextLog);
        for (auto* src : _mediaSources)
        {
            for (auto packet = src->getPacket(t); packet; packet = src->getPacket(t))
            {
                auto& header = getMetaData(*packet);
                header.sendTime = t;
                _links.front()->push(std::move(packet), t);
            }
            timeAdvance = std::min(timeAdvance, src->timeToRelease(t));
        }

        for (size_t i = 0; i < _links.size() - 1; ++i)
        {
            for (auto packet = _links[i]->pop(t); packet; packet = _links[i]->pop(t))
            {
                _links[i + 1]->push(std::move(packet), t);
            }
            timeAdvance = std::min(timeAdvance, _links[i]->timeToRelease(t));
        }

        for (auto packet = _links.back()->pop(t); packet; packet = _links.back()->pop(t))
        {
            packet->get();
            // logger::debug("received %llu sz %zu", "", t / 1000000, packet->getLength());
            if (packet->get()[0] != FakeCrossTraffic::CROSS_TRAFFIC_PROTOCOL)
            {
                auto& header = getMetaData(*packet);
                _bwe.update(packet->getLength(), header.sendTime, t);
            }
        }

        timeAdvance = std::min(timeAdvance, _links.back()->timeToRelease(t));

        if (timeAdvance > 0)
        {
            _timeCursor += timeAdvance;
        }

        if (utils::Time::diff(t, nextLog) <= 0)
        {
            logger::info("estimate %.0f kbps owd %.1fms, link "
                         "%.0f, Q %zu",
                "",
                _bwe.getEstimate(_timeCursor),
                _bwe.getDelay(),
                _links[0]->getBitRateKbps(t),
                _links[0]->getQueueLength());

            return true;
        }
    }

    return false;
}

} // namespace fakenet
