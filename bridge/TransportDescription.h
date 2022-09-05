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
        const bool isDtlsClient)
        : ice(Ice{iceCandidates, iceCredentials}),
          dtls(Dtls{isDtlsClient})
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer, const bool isDtlsClient)
        : localPeer(localPeer),
          dtls(Dtls{isDtlsClient})
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer) : localPeer(localPeer) {}

    utils::Optional<Ice> ice;
    utils::Optional<transport::SocketAddress> localPeer;
    utils::Optional<Dtls> dtls;
};

} // namespace bridge
