#pragma once
#include "transport/Transport.h"
#include "utils/Optional.h"
#include <string>
#include <utility>
#include <vector>

namespace bridge
{

struct TransportDescription
{
    struct Ice
    {
        std::vector<ice::IceCandidate> iceCandidates;
        std::pair<std::string, std::string> iceCredentials;
    };

    struct Dtls
    {
        bool isDtlsClient;
    };

    TransportDescription() = default;

    TransportDescription(const std::vector<ice::IceCandidate>& iceCandidates,
        const std::pair<std::string, std::string>& iceCredentials,
        const bool isDtlsClient,
        const std::vector<srtp::AesKey>& sdesKeys,
        srtp::Mode srtpMode)
        : ice(Ice{iceCandidates, iceCredentials}),
          dtls(Dtls{isDtlsClient}),
          sdesKeys(sdesKeys),
          srtpMode(srtpMode)
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer,
        const bool isDtlsClient,
        const std::vector<srtp::AesKey>& sdesKeys,
        srtp::Mode srtpMode)
        : localPeer(localPeer),
          dtls(Dtls{isDtlsClient}),
          sdesKeys(sdesKeys),
          srtpMode(srtpMode)
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer,
        const std::vector<srtp::AesKey>& sdesKeys,
        srtp::Mode srtpMode)
        : localPeer(localPeer),
          sdesKeys(sdesKeys),
          srtpMode(srtpMode)
    {
    }

    utils::Optional<Ice> ice;
    utils::Optional<transport::SocketAddress> localPeer;
    Dtls dtls;
    std::vector<srtp::AesKey> sdesKeys;
    srtp::Mode srtpMode;
};

} // namespace bridge
