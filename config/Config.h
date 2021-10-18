#pragma once

#include "config/ConfigReader.h"
#include "utils/Time.h"
#include <string>
namespace config
{

class Config : public ConfigReader
{
public:
    CFG_PROP(std::string, ip, "");
    CFG_PROP(uint16_t, port, 8080);
    CFG_PROP(bool, logStdOut, true);
    CFG_PROP(std::string, logLevel, "INFO");

    // If mixer does not receive any packets during this timeout, it's considered abandoned and is garbage collected.
    CFG_PROP(int, mixerInactivityTimeoutMs, 2 * 60 * 1000);
    CFG_PROP(int, numWorkerTreads, 0);
    CFG_PROP(std::string, logFile, "/tmp/smb.log");

    CFG_PROP(uint32_t, maxLastN, 16);
    CFG_PROP(uint32_t, defaultLastN, 5);
    CFG_PROP(uint32_t, dropInboundAfterInactive, 3);

    CFG_GROUP()
    // Value between 0 and 127, where 127 is the lowest audio level and 0 the highest.
    CFG_PROP(int32_t, silenceThresholdLevel, 127);
    CFG_GROUP_END(audio);

    CFG_GROUP()
    // fix SCTP port to 5000 to support old CS
    CFG_PROP(bool, fixedPort, true);
    CFG_GROUP_END(sctp);

    CFG_GROUP()
    CFG_PROP(bool, enableIpv6, false);
    CFG_PROP(bool, useAwsInfo, false);
    CFG_PROP(uint16_t, singlePort, 10000);
    CFG_PROP(std::string, publicIpv4, "");
    CFG_PROP(std::string, publicIpv6, "");
    CFG_PROP(std::string, preferredIp, "");
    CFG_PROP(uint16_t, udpPortRangeLow, 10006);
    CFG_PROP(uint16_t, udpPortRangeHigh, 26000);
    CFG_PROP(uint32_t, sharedPorts, 1);
    CFG_PROP(uint32_t, maxCandidateCount, 5 * 3);

    CFG_GROUP()
    CFG_PROP(bool, enable, false);
    CFG_PROP(uint16_t, port, 4443);
    CFG_PROP(uint32_t, iceTimeoutSec, 7);
    CFG_PROP(uint16_t, aliasPort, 0);
    CFG_GROUP_END(tcp)

    CFG_GROUP_END(ice);

    CFG_GROUP()
    CFG_PROP(bool, useUplinkEstimate, false);
    CFG_PROP(bool, logUplinkEstimates, false);
    CFG_PROP(uint32_t, uplinkEstimatesPaddingIntervalMs, 10);
    CFG_PROP(uint32_t, uplinkEstimatesPaddingPackets, 15);
    CFG_PROP(uint32_t, uplinkEstimatesPaddingIterations, 500);
    CFG_PROP(bool, logDownlinkEstimates, true);
    CFG_PROP(std::string, packetLogLocation, "");
    CFG_PROP(double, packetOverhead, 0.1);
    CFG_GROUP_END(bwe);

    CFG_GROUP()
    CFG_PROP(uint16_t, singlePort, 10500);
    CFG_PROP(uint32_t, sharedPorts, 1);
    CFG_GROUP_END(recording)

    struct RtcpConfig
    {
        uint32_t mtu = 1440;
        uint64_t reportInterval = utils::Time::ms * 500;
    } rtcp; // can be made configurable later

    struct RecordingRtcpConfig
    {
        uint32_t mtu = 1440;
        uint64_t reportInterval = utils::Time::ms * 2500;
    } recordingRtcp; // can be made configurable later

    uint32_t mtu = 1480;
};

} // namespace config
