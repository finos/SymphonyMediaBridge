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

    CFG_PROP(uint32_t, defaultLastN, 5);
    CFG_PROP(uint32_t, dropInboundAfterInactive, 3);
    CFG_PROP(uint32_t, maxDefaultLevelBandwidthKbps, 3000);
    CFG_PROP(uint32_t, rtpForwardInterval, 10); // ms

    CFG_GROUP()
    // Value between 0 and 127, where 127 is the lowest audio level and 0 the highest.
    CFG_PROP(uint8_t, silenceThresholdLevel, 127);
    CFG_PROP(uint32_t, lastN, 3);
    CFG_PROP(uint32_t, lastNextra, 2);
    CFG_PROP(uint32_t, activeTalkerSilenceThresholdDb, 18);
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
    CFG_PROP(bool, logDownlinkEstimates, true);
    CFG_PROP(std::string, packetLogLocation, "");
    CFG_PROP(double, packetOverhead, 0.1);
    CFG_PROP(bool, enable, true);
    CFG_GROUP_END(bwe);

    CFG_GROUP() // rate control
    CFG_PROP(bool, enable, true);
    CFG_PROP(uint32_t, floor, 300);
    CFG_PROP(uint32_t, ceiling, 9000);
    CFG_PROP(uint32_t, initialEstimate, 1200);
    CFG_PROP(bool, debugLog, false);
    CFG_PROP(uint64_t, cooldownInterval, 30); // Time until rtcl inactivates after last received video
    CFG_PROP(bool, useUplinkEstimate, true);
    CFG_GROUP_END(rctl)

    CFG_GROUP()
    CFG_PROP(uint16_t, singlePort, 10500);
    CFG_PROP(uint32_t, sharedPorts, 1);
    CFG_GROUP_END(recording)

    CFG_GROUP()
    CFG_PROP(uint32_t, minBitrate, 900);
    CFG_PROP(double, allocFactor, 1.0);
    CFG_GROUP_END(slides)

    CFG_GROUP()
    CFG_PROP(uint32_t, mtu, 1440);

    CFG_GROUP()
    CFG_PROP(uint64_t, interval, utils::Time::ms * 600);
    CFG_PROP(uint64_t, resubmitInterval, utils::Time::sec * 7);
    CFG_GROUP_END(senderReport)

    CFG_GROUP()
    CFG_PROP(uint64_t, delayAfterSR, utils::Time::ms * 400);
    CFG_PROP(uint64_t, idleInterval, utils::Time::sec * 6);
    CFG_GROUP_END(receiveReport)

    CFG_GROUP_END(rtcp) // can be made configurable later

    CFG_GROUP()
    CFG_PROP(uint32_t, mtu, 1440);
    CFG_PROP(uint64_t, reportInterval, utils::Time::ms * 2500);
    CFG_GROUP_END(recordingRtcp)

    CFG_PROP(uint32_t, mtu, 1480);
    CFG_PROP(uint32_t, ipOverhead, 20 + 14);
};

} // namespace config
