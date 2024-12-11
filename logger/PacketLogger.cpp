#include "PacketLogger.h"
#include "concurrency/ThreadUtils.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"

namespace logger
{
PacketLogItem::PacketLogItem() : receiveTimestamp(0), transmitTimestamp(0), ssrc(0), sequenceNumber(0), size(0) {}

PacketLogItem::PacketLogItem(const memory::Packet& packet, uint64_t receiveTime) : receiveTimestamp(receiveTime)
{
    auto* header = rtp::RtpHeader::fromPacket(packet);
    if (!header)
    {
        size = 0;
        sequenceNumber = 0;
        ssrc = 0;
        transmitTimestamp = 0;
        return;
    }

    size = packet.getLength();
    sequenceNumber = header->sequenceNumber;
    ssrc = header->ssrc;
    transmitTimestamp = 0;
    rtp::getTransmissionTimestamp(packet, 3, transmitTimestamp);
}

PacketLoggerThread::PacketLoggerThread(FILE* logFile, size_t backlogSize)
    : _running(true),
      _logQueue(backlogSize),
      _logFile(logFile),
      _thread(new std::thread([this] { this->run(); }))
{
}

PacketLoggerThread::~PacketLoggerThread()
{
    if (_running)
    {
        stop();
    }
}

void PacketLoggerThread::post(const memory::Packet& packet, uint64_t receiveTime)
{
    if (!rtp::isRtpPacket(packet))
    {
        return;
    }

    _logQueue.push(PacketLogItem(packet, receiveTime));
}

void PacketLoggerThread::stop()
{
    if (!_running)
    {
        return;
    }

    _running = false;
    if (_thread)
    {
        _thread->join();
    }
}

void PacketLoggerThread::run()
{
    concurrency::setThreadName("PacketLogger");
    bool gotLogItem = false;
    PacketLogItem item;
    for (;;)
    {
        if (_logQueue.pop(item))
        {
            gotLogItem = true;

            if (_logFile)
            {
                ::fwrite(&item, sizeof(item), 1, _logFile);
            }
        }
        else
        {
            if (gotLogItem && _logFile)
            {
                ::fflush(_logFile);
            }
            gotLogItem = false;

            if (!_running.load(std::memory_order::memory_order_relaxed))
            {
                break;
            }
            utils::Time::nanoSleep(50 * utils::Time::ms);
        }
    }

    if (_logFile)
    {
        ::fclose(_logFile);
        _logFile = nullptr;
    }
}

PacketLogReader::PacketLogReader(FILE* logFile) : _logFile(logFile) {}

PacketLogReader::~PacketLogReader()
{
    if (_logFile)
    {
        ::fclose(_logFile);
    }
}

bool PacketLogReader::getNext(PacketLogItem& item)
{
    if (!_logFile)
    {
        return false;
    }
    int readItems = ::fread(&item, sizeof(item), 1, _logFile);
    return 1 == readItems;
}

void PacketLogReader::rewind()
{
    if (!_logFile)
    {
        return;
    }
    ::rewind(_logFile);
}

} // namespace logger