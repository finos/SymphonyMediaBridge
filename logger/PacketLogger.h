#pragma once
#include "concurrency/MpmcQueue.h"
#include <thread>

namespace memory
{
class Packet;
}

namespace logger
{
struct PacketLogItem
{
    PacketLogItem();
    PacketLogItem(const memory::Packet& packet, uint64_t receiveTime);

    uint64_t receiveTimestamp;
    uint32_t transmitTimestamp; // 24 bit
    uint32_t ssrc;
    uint16_t sequenceNumber;
    uint16_t size;
};

class PacketLoggerThread
{
public:
    PacketLoggerThread(FILE* logFile, size_t backlogSize);
    ~PacketLoggerThread();

    void post(const memory::Packet& item, uint64_t receiveTime);
    void stop();

private:
    void run();

    std::atomic_bool _running;
    concurrency::MpmcQueue<PacketLogItem> _logQueue;
    FILE* _logFile;
    std::unique_ptr<std::thread> _thread;
};

class PacketLogReader
{
public:
    explicit PacketLogReader(FILE* logFile);
    ~PacketLogReader();

    bool getNext(PacketLogItem& item);

    void rewind();

    bool isOpen() const { return _logFile != nullptr; }

private:
    FILE* _logFile;
};

} // namespace logger