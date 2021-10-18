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
        std::vector<ice::IceCandidate> _iceCandidates;
        std::pair<std::string, std::string> _iceCredentials;
    };

    struct Dtls
    {
        bool _isDtlsClient;
    };

    TransportDescription() = default;

    TransportDescription(const std::vector<ice::IceCandidate>& iceCandidates,
        const std::pair<std::string, std::string>& iceCredentials,
        const bool isDtlsClient)
        : _ice(Ice{iceCandidates, iceCredentials}),
          _dtls(Dtls{isDtlsClient})
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer, const bool isDtlsClient) : _dtls(Dtls{isDtlsClient})
    {
    }

    TransportDescription(const transport::SocketAddress& localPeer) : _localPeer(localPeer) {}

    utils::Optional<Ice> _ice;
    utils::Optional<transport::SocketAddress> _localPeer;
    utils::Optional<Dtls> _dtls;
};

} // namespace bridge
