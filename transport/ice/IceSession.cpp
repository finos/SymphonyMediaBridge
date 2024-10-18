#include "IceSession.h"
#include "Stun.h"
#include "logger/Logger.h"
#include "utils/ContainerAlgorithms.h"
#include "utils/Time.h"

namespace
{

const ice::IceCandidate* findCandidate(const ice::IceCandidates& candidates, const ice::IceCandidate& candidate)
{
    auto it = std::find_if(candidates.begin(), candidates.end(), [&](const ice::IceCandidate& x) {
        return x.transportType == candidate.transportType && x.address == candidate.address;
    });

    return it == candidates.end() ? nullptr : &(*it);
}

const ice::IceCandidate* findCandidateWithUdpAddress(const ice::IceCandidates& candidates,
    const transport::SocketAddress& address)
{
    auto it = std::find_if(candidates.begin(), candidates.end(), [&](const ice::IceCandidate& x) {
        return x.transportType == ice::TransportType::UDP && x.address == address;
    });

    return it == candidates.end() ? nullptr : &(*it);
}

void addCandidateToListIfNotPresent(ice::IceCandidates& candidates, const ice::IceCandidate& candidate)
{
    if (findCandidate(candidates, candidate) == nullptr)
    {
        candidates.push_back(candidate);
    }
}

} // namespace

namespace ice
{
const char* toString(IceSession::State s);
const char* toString(IceSession::ProbeState s);

uint32_t computeCandidatePriority(IceCandidate candidate, int localInterfacePreference)
{
    return ice::IceCandidate::computeCandidatePriority(candidate.type,
        localInterfacePreference,
        candidate.component,
        candidate.transportType);
}

IceSession::IceSession(size_t sessionId,
    const IceConfig& config,
    ice::IceComponent component,
    const IceRole role,
    IEvents* eventSink)
    : _logId("IceSession-" + std::to_string(sessionId)),
      _nomination(nullptr),
      _preliminaryCandidate(nullptr),
      _component(component),
      _tcpProbeCount(0),
      _config(config),
      _state(State::IDLE),
      _eventSink(eventSink),
      _credentials(role, static_cast<uint64_t>(_idGenerator.next().w0) << 32 | _idGenerator.next().w1),
      _sessionStart(0),
      _connectedCount(0)
{
    char ufrag[14 + 1];
    char pwd[24 + 1]; // length selected to make attribute *4 length

    _localCandidates.reserve(6); // Usually 6 = 3 (IPv4 HOST, IPv4 SRFLX and IPv6 HOST) * 2 (UDP and TCP)
    _remoteCandidates.reserve(8);

    generateCredentialString(_idGenerator, ufrag, sizeof(ufrag) - 1);
    generateCredentialString(_idGenerator, pwd, sizeof(pwd) - 1);
    _credentials.local = std::make_pair<std::string, std::string>(ufrag, pwd);
    _hmacComputer.local.init(_credentials.local.second.c_str(), _credentials.local.second.size());
}

// add most preferred UDP end point first. It will affect prioritization of candidates
void IceSession::attachLocalEndpoint(IceEndpoint* localEndpoint)
{
    if (localEndpoint->getTransportType() != ice::TransportType::UDP)
    {
        assert(false);
        return;
    }

    const int preference = 256 - _endpoints.size();
    _endpoints.push_back(EndpointInfo(localEndpoint, preference));

    const auto address = localEndpoint->getLocalPort();
    if (localEndpoint->getTransportType() == TransportType::UDP &&
        !utils::contains(_localCandidates, [address](const ice::IceCandidate& c) { return c.baseAddress == address; }))
    {
        _localCandidates.emplace_back(IceCandidate(_component,
            localEndpoint->getTransportType(),
            ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::HOST,
                preference,
                _component,
                localEndpoint->getTransportType()),
            address,
            address,
            IceCandidate::Type::HOST));
    }
}

void IceSession::gatherLocalCandidates(const std::vector<transport::SocketAddress>& stunServers,
    const uint64_t timestamp)
{
    _stunServers = stunServers;
    _candidatePairs.reserve(_endpoints.size() * stunServers.size());

    if (stunServers.empty())
    {
        _state = State::READY;
        return;
    }
    reportState(State::GATHERING);

    for (size_t stunIndex = 0; stunIndex < _stunServers.size(); ++stunIndex)
    {
        for (auto& endpoint : _endpoints)
        {
            if (endpoint.endpoint->getLocalPort().getFamily() != _stunServers[stunIndex].getFamily() ||
                endpoint.endpoint->getTransportType() != ice::TransportType::UDP)
            {
                continue;
            }

            auto cit = std::find_if(_localCandidates.cbegin(),
                _localCandidates.cend(),
                [endpoint](const ice::IceCandidate& c) {
                    return endpoint.endpoint->getLocalPort() == c.baseAddress && c.type == IceCandidate::Type::HOST;
                });
            assert(cit != _localCandidates.cend());
            if (cit == _localCandidates.cend())
            {
                continue;
            }

            const auto& localCandidate = *cit;
            _candidatePairs.emplace_back(std::make_unique<CandidatePair>(*this,
                endpoint,
                localCandidate,
                IceCandidate(_component,
                    endpoint.endpoint->getTransportType(),
                    0,
                    _stunServers[stunIndex],
                    _stunServers[stunIndex],
                    IceCandidate::Type::RELAY),
                true));
            auto& ct = _candidatePairs.back();
            ct->original.header.setMethod(StunHeader::BindingRequest);
        }
    }

    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->gatheringProbe && candidatePair->state == ProbeState::Waiting)
        {
            candidatePair->send(timestamp);
        }
    }
}

void IceSession::sortCheckList()
{
    _checklist.clear();
    for (auto& candidatePair : _candidatePairs)
    {
        if (!candidatePair->gatheringProbe)
        {
            _checklist.push_back(candidatePair.get());
        }
    }

    const auto role = _credentials.role;
    std::sort(_checklist.begin(), _checklist.end(), [role](CandidatePair* a, CandidatePair* b) {
        return a->getPriority(role) > b->getPriority(role);
    });
}

void IceSession::probeRemoteCandidates(const IceRole role, uint64_t timestamp)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_sessionStart == 0)
    {
        _credentials.role = role;
        _sessionStart = timestamp;
    }
    if (_credentials.remote.second.empty())
    {
        return;
    }

    for (auto& remoteCandidate : _remoteCandidates)
    {
        if (remoteCandidate.transportType == TransportType::UDP)
        {
            for (auto& endpoint : _endpoints)
            {
                if (endpoint.endpoint->getTransportType() == TransportType::UDP)
                {
                    addProbeForRemoteCandidate(endpoint, remoteCandidate);
                }
            }
        }
    }

    sortCheckList();

    auto transmitTime = timestamp;
    for (auto* probe : _checklist)
    {
        if (probe->state != ProbeState::Waiting)
        {
            continue;
        }

        if (probe->localEndpoint.endpoint->getTransportType() == TransportType::TCP && probe != _checklist.front())
        {
            transmitTime += _config.probeReleasePace * utils::Time::ms;
        }
        if (probe->nominated)
        {
            probe->nextTransmission = timestamp;
        }
        else
        {
            probe->nextTransmission = transmitTime;
        }
        transmitTime += _config.probeReleasePace * utils::Time::ms;
    }

    reportState(State::CONNECTING);
    processTimeout(timestamp);
}

IceSession::CandidatePair* IceSession::addProbeForRemoteCandidate(const EndpointInfo& endpoint,
    const IceCandidate& remoteCandidate)
{
    const auto endpointAddress = endpoint.endpoint->getLocalPort();
    if (endpointAddress.getFamily() != remoteCandidate.address.getFamily() ||
        endpoint.endpoint->getTransportType() != remoteCandidate.transportType)
    {
        return nullptr;
    }

    if (findCandidatePair(endpoint.endpoint, remoteCandidate.address))
    {
        logger::debug("probe already created %s %s",
            _logId.c_str(),
            remoteCandidate.address.toString().c_str(),
            ice::toString(remoteCandidate.transportType).c_str());
        return nullptr; // there is already a probe
    }

    if (remoteCandidate.transportType == TransportType::UDP)
    {
        auto localCandidateIt =
            std::find_if(_localCandidates.begin(), _localCandidates.end(), [endpointAddress](const IceCandidate& c) {
                return endpointAddress == c.baseAddress && c.type == IceCandidate::Type::HOST;
            });

        if (localCandidateIt == _localCandidates.end())
        {
            logger::error("Failed to add probe. Endpoint address %s is not in local candidates list",
                _logId.c_str(),
                endpointAddress.toString().c_str());
            return nullptr;
        }

        _candidatePairs.emplace_back(
            std::make_unique<CandidatePair>(*this, endpoint, *localCandidateIt, remoteCandidate, false));
    }
    else
    {
        _candidatePairs.emplace_back(std::make_unique<CandidatePair>(*this,
            endpoint,
            IceCandidate(IceComponent::RTP,
                TransportType::TCP,
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::HOST,
                    endpoint.preference,
                    IceComponent::RTP,
                    TransportType::TCP),
                endpointAddress,
                endpointAddress,
                IceCandidate::Type::HOST),
            remoteCandidate,
            false));
    }
    auto& candidatePair = _candidatePairs.back();

    StunMessage& iceProbe(candidatePair->original);
    iceProbe.header.transactionId.set(_idGenerator.next());
    iceProbe.header.setMethod(StunHeader::BindingRequest);
    iceProbe.add(StunGenericAttribute(StunAttribute::SOFTWARE, _config.software));
    iceProbe.add(StunPriority(static_cast<uint32_t>(ice::IceCandidate::computeCandidatePriority(remoteCandidate.type,
        endpoint.preference,
        _component,
        remoteCandidate.transportType))));

    logger::info("added candidate pair %s-%s HOST-%s %s",
        _logId.c_str(),
        endpointAddress.toFixedString().c_str(),
        maybeMasked(remoteCandidate.address).c_str(),
        ice::toString(remoteCandidate.type).c_str(),
        ice::toString(remoteCandidate.transportType).c_str());

    return candidatePair.get();
}

void IceSession::addLocalCandidate(const IceCandidate& candidate)
{
    if (!utils::contains(_localCandidates, [candidate](const IceCandidate& x) {
            return x.address == candidate.address && x.baseAddress == candidate.baseAddress &&
                x.transportType == candidate.transportType;
        }))
    {
        _localCandidates.push_back(candidate);
        logger::debug("added local candidate %s-%s %s",
            _logId.c_str(),
            candidate.baseAddress.toString().c_str(),
            candidate.address.toString().c_str(),
            ice::toString(candidate.type).c_str());
    }

    std::sort(_localCandidates.begin(), _localCandidates.end(), [](const IceCandidate& a, const IceCandidate& b) {
        return a.priority > b.priority;
    });
}

void IceSession::addLocalTcpCandidate(IceCandidate::Type type,
    const int interfaceIndex,
    const transport::SocketAddress& baseAddress,
    const transport::SocketAddress& address,
    TcpType tcpType)
{
    IceCandidate candidate(_component,
        TransportType::TCP,
        ice::IceCandidate::computeCandidatePriority(type, 128 - interfaceIndex, _component, TransportType::TCP),
        address,
        baseAddress,
        type,
        tcpType);

    addLocalCandidate(candidate);
}

void IceSession::addLocalCandidate(const transport::SocketAddress& publicAddress, IceEndpoint* localEndpoint)
{
    for (auto& endpointInfo : _endpoints)
    {
        if (endpointInfo.endpoint == localEndpoint)
        {
            addLocalCandidate(IceCandidate(_component,
                localEndpoint->getTransportType(),
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::SRFLX,
                    endpointInfo.preference,
                    _component,
                    localEndpoint->getTransportType()),
                publicAddress,
                endpointInfo.endpoint->getLocalPort(),
                IceCandidate::Type::SRFLX));
            return;
        }
    }
}

void IceSession::removeLocalCandidate(const IceCandidate& localCandidate)
{
    for (auto it = _localCandidates.begin(); it != _localCandidates.end(); ++it)
    {
        if (it->address == localCandidate.address)
        {
            _localCandidates.erase(it);
            return;
        }
    }
}

const IceCandidate& IceSession::addRemoteCandidate(const IceCandidate& candidate)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    auto* remoteCandidate = findCandidate(_remoteCandidates, candidate);

    if (remoteCandidate)
    {
        return *remoteCandidate;
    }

    _remoteCandidates.push_back(candidate);
    logger::debug("added remote candidate %s %s %s",
        _logId.c_str(),
        candidate.address.toString().c_str(),
        ice::toString(candidate.type).c_str(),
        ice::toString(candidate.transportType).c_str());

    return _remoteCandidates.back();
}

void IceSession::addRemoteTcpPassiveCandidate(const IceCandidate& candidate, IceEndpoint* tcpEndpoint)
{
    assert(candidate.transportType == TransportType::TCP);
    addRemoteTcpCandidateAndCreatePair(candidate, tcpEndpoint);
}

IceSession::CandidatePair* IceSession::addRemoteTcpCandidateAndCreatePair(const IceCandidate& candidate,
    IceEndpoint* tcpEndpoint)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (isAttached(tcpEndpoint))
    {
        return nullptr;
    }

    _remoteCandidates.push_back(candidate);
    logger::debug("added remote TCP candidate %s %s %s",
        _logId.c_str(),
        candidate.address.toString().c_str(),
        ice::toString(candidate.type).c_str(),
        ice::toString(candidate.transportType).c_str());

    EndpointInfo endpointInfo(tcpEndpoint, 128 - _tcpProbeCount++);
    _endpoints.push_back(endpointInfo);
    return addProbeForRemoteCandidate(endpointInfo, candidate);
}

void IceSession::removeRemoteCandidate(const IceCandidate& remoteCandidate)
{
    for (auto it = _remoteCandidates.begin(); it != _remoteCandidates.end(); ++it)
    {
        if (it->address == remoteCandidate.address)
        {
            _remoteCandidates.erase(it);
            return;
        }
    }
}

std::vector<IceCandidate> IceSession::getLocalCandidates() const
{
    return _localCandidates;
}

std::pair<IceCandidate, IceCandidate> IceSession::getSelectedPair() const
{
    if (_nomination)
    {
        return std::make_pair(_nomination->localCandidate, _nomination->remoteCandidate);
    }
    else if (_preliminaryCandidate)
    {
        return std::make_pair(_preliminaryCandidate->localCandidate, _preliminaryCandidate->remoteCandidate);
    }

    return std::make_pair<IceCandidate, IceCandidate>(IceCandidate(), IceCandidate());
}

uint64_t IceSession::getSelectedPairRtt() const
{
    for (auto candidatePair : _checklist)
    {
        if (candidatePair->state == ProbeState::Succeeded && candidatePair->nominated)
        {
            return candidatePair->getRtt();
        }
    }
    return 0;
}

bool IceSession::isRequestAuthentic(const void* data, size_t len) const
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    const auto* stunMessage = StunMessage::fromPtr(data);

    if (stunMessage && stunMessage->isValid() && stunMessage->header.isRequest() &&
        stunMessage->isAuthentic(_hmacComputer.local))
    {
        const auto* attribute = stunMessage->getAttribute<StunUserName>(StunAttribute::USERNAME);
        return attribute && attribute->isTargetUser(_credentials.local.first.c_str());
    }

    return false;
}

bool IceSession::isResponseAuthentic(const void* data, size_t len) const
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    const auto* stunMessage = StunMessage::fromPtr(data);
    return stunMessage && stunMessage->isValid() && stunMessage->header.isResponse() &&
        stunMessage->isAuthentic(_hmacComputer.remote);
}

bool IceSession::isIceAuthentic(const void* data, size_t len) const
{
    if (ice::isResponse(data))
    {
        return isResponseAuthentic(data, len);
    }

    if (ice::isRequest(data))
    {
        return isRequestAuthentic(data, len);
    }

    return false;
}

bool IceSession::processValidStunTcpRequest(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const StunMessage& msg,
    int remoteCandidatePriority,
    uint64_t now)
{
    assert(localEndpoint->getTransportType() == TransportType::TCP);

    if (isAttached(localEndpoint))
    {
        sendResponse(localEndpoint, remotePort, 0, msg, now);
        return true;
    }

    IceCandidate remoteCandidate(_component,
        localEndpoint->getTransportType(),
        remoteCandidatePriority,
        remotePort,
        remotePort,
        IceCandidate::Type::PRFLX);

    auto* candidatePair = addRemoteTcpCandidateAndCreatePair(remoteCandidate, localEndpoint);
    if (candidatePair)
    {
        candidatePair->accept();
        sendResponse(localEndpoint, remotePort, 0, msg, now);
        candidatePair->receptionTimestamp = now;
        candidatePair->send(now);
    }

    if (_state == State::CONNECTING)
    {
        sortCheckList();
    }

    return candidatePair != nullptr;
}

bool IceSession::processValidStunUdpRequest(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const StunMessage& msg,
    int remoteCandidatePriority,
    uint64_t now)
{
    assert(localEndpoint->getTransportType() == TransportType::UDP);

    auto* pair = findCandidatePair(localEndpoint, remotePort);
    if (pair)
    {
        // Only send response after accept! Otherwise we could drop early media
        pair->accept();
        sendResponse(localEndpoint, remotePort, 0, msg, now);
        return true;
    }

    const auto* remoteCandidate = findCandidateWithUdpAddress(_remoteCandidates, remotePort);
    if (remoteCandidate)
    {
        const auto* localEndpointInfo = findEndpointInfo(localEndpoint);
        assert(localEndpointInfo);
        if (!localEndpointInfo)
        {
            // We can't accept endpoint that are not attached.
            logger::error("Receive stun from unattached endpoint. %s %s",
                _logId.c_str(),
                toString(localEndpoint->getTransportType()).c_str(),
                localEndpoint->getLocalPort().toFixedString().c_str());

            sendResponse(localEndpoint, remotePort, StunError::Code::ServerError, msg, now, "Server Error");
            return false;
        }

        // Most likely we are receiving STUN from SRFLX or HOST when we are on READY state
        auto* candidatePair = addProbeForRemoteCandidate(*localEndpointInfo, *remoteCandidate);
        if (!candidatePair)
        {
            logger::error("Failed to create candidate pair %s %s %s - %s",
                _logId.c_str(),
                toString(localEndpointInfo->endpoint->getTransportType()).c_str(),
                toString(remoteCandidate->type).c_str(),
                localEndpointInfo->endpoint->getLocalPort().toFixedString().c_str(),
                maybeMasked(remoteCandidate->address).c_str());

            sendResponse(localEndpoint, remotePort, StunError::Code::ServerError, msg, now, "Server Error");
            return false;
        }

        candidatePair->accept();
        candidatePair->nextTransmission = now;
        candidatePair->receptionTimestamp = now;

        // Only send response after accept! Otherwise we could drop early media
        sendResponse(localEndpoint, remotePort, 0, msg, now);
        sortCheckList();
        processTimeout(now);
        return true;
    }

    if (_state == State::CONNECTED || _remoteCandidates.size() >= _config.maxCandidateCount)
    {
        // When we see a new candidate and the ICE is CONNECTED, this can be an early sign that network state.
        // Also try to remove unviable when maxCandidateCount is reached to try to find space for this new candidate
        removeUnviableRemoteCandidates(now);
    }

    if (_remoteCandidates.size() >= _config.maxCandidateCount)
    {
        logger::info("too many PRFLX candidates %u, %s",
            _logId.c_str(),
            _config.maxCandidateCount,
            maybeMasked(remotePort).c_str());

        // Although we are not able to accept this candidate (at least not now)
        // we should send probe response anyway as this candidate will not be
        // probed from SMB -> client, so client should not use it but it can
        // decide to keep it as backup and we might be able to accept it later
        sendResponse(localEndpoint, remotePort, 0, msg, now);
        return false;
    }

    IceCandidate newRemoteCandidate(_component,
        localEndpoint->getTransportType(),
        remoteCandidatePriority,
        remotePort,
        remotePort,
        IceCandidate::Type::PRFLX);

    newRemoteCandidate = addRemoteCandidate(newRemoteCandidate);

    for (auto& endpoint : _endpoints)
    {
        auto* candidatePair = addProbeForRemoteCandidate(endpoint, newRemoteCandidate);
        if (candidatePair)
        {
            candidatePair->nextTransmission = now;
            candidatePair->receptionTimestamp = now;

            if (endpoint.endpoint == localEndpoint)
            {
                candidatePair->accept();
                sendResponse(localEndpoint, remotePort, 0, msg, now);
            }
        }
    }

    sortCheckList();
    processTimeout(now);
    return true;
}

void IceSession::processValidStunRequest(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const StunMessage& msg,
    uint64_t now)
{
    logger::debug("%s probe from %s -> %s",
        _logId.c_str(),
        ice::toString(localEndpoint->getTransportType()).c_str(),
        remotePort.toString().c_str(),
        localEndpoint->getLocalPort().toString().c_str());

    int remoteCandidatePriority = 0;
    const auto prioAttribute = msg.getAttribute<StunPriority>(StunAttribute::PRIORITY);
    if (prioAttribute)
    {
        remoteCandidatePriority = prioAttribute->value;
    }

    bool successfullyProcessed = false;

    if (localEndpoint->getTransportType() == TransportType::TCP)
    {
        successfullyProcessed =
            processValidStunTcpRequest(localEndpoint, remotePort, msg, remoteCandidatePriority, now);
    }
    else if (localEndpoint->getTransportType() == TransportType::UDP)
    {
        successfullyProcessed =
            processValidStunUdpRequest(localEndpoint, remotePort, msg, remoteCandidatePriority, now);
    }

    if (successfullyProcessed && _credentials.role == IceRole::CONTROLLED)
    {
        const auto useCandidate = msg.getAttribute(StunAttribute::USE_CANDIDATE);
        auto useCandidatePair = useCandidate ? findCandidatePair(localEndpoint, remotePort) : nullptr;
        if (useCandidatePair)
        {
            if (_nomination != useCandidatePair)
            {
                logger::debug("remote nominated %s-%s",
                    _logId.c_str(),
                    useCandidatePair->localCandidate.address.toString().c_str(),
                    useCandidatePair->remoteCandidate.address.toString().c_str());
                if (useCandidatePair->state == ProbeState::Succeeded)
                {
                    if (_nomination)
                    {
                        // Aggressive nomination
                        // Then we will clean the old nominated flag
                        _nomination->nominated = false;
                    }

                    useCandidatePair->nominate(now);
                    _nomination = useCandidatePair;
                    _preliminaryCandidate = useCandidatePair;

                    if (_state == State::CONNECTED && _eventSink)
                    {
                        // aggressive nomination
                        _eventSink->onIceCandidateChanged(this,
                            useCandidatePair->localEndpoint.endpoint,
                            useCandidatePair->remoteCandidate.address);
                    }
                }
            }
            processTimeout(now);
        }
    }
}

void IceSession::onRequestReceived(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const StunMessage& msg,
    const uint64_t now)
{
    if (_state >= State::FAILED)
    {
        // SMB does not have a way to restart ICE. Then we should not respond more to probes after we have a failed
        // state otherwise peer might think the ICE connection is still good and then does not try any recover
        // mechanism. So if it is FAILED, peer MUST fail too
        return;
    }

    if (!msg.isValid() || msg.getAttribute(StunAttribute::MESSAGE_INTEGRITY) == nullptr ||
        msg.getAttribute(StunAttribute::USERNAME) == nullptr)
    {
        sendResponse(localEndpoint, remotePort, StunError::Code::BadRequest, msg, now, "Bad Request");
        return;
    }

    const auto userNames = msg.getAttribute<StunUserName>(StunAttribute::USERNAME)->getNames();
    if (userNames.first != _credentials.local.first)
    {
        sendResponse(localEndpoint,
            remotePort,
            StunError::Code::Unauthorized,
            msg,
            now,
            "Unknown user " + userNames.first + ":" + userNames.second);
        return;
    }
    if (!msg.isAuthentic(_hmacComputer.local))
    {
        sendResponse(localEndpoint, remotePort, StunError::Code::Unauthorized, msg, now, "Unauthorized");
        return;
    }
    const auto* peerControlling = msg.getAttribute<StunAttribute64>(StunAttribute::ICE_CONTROLLING);
    const auto* peerControlled = msg.getAttribute<StunAttribute64>(StunAttribute::ICE_CONTROLLED);
    if (_credentials.role == ice::IceRole::CONTROLLING && peerControlling)
    {
        if (_credentials.tieBreaker >= peerControlling->get())
        {
            sendResponse(localEndpoint, remotePort, StunError::Code::RoleConflict, msg, now, "Role Conflict");
            return;
        }
        else
        {
            _credentials.role = ice::IceRole::CONTROLLED;
        }
    }
    else if (_credentials.role == ice::IceRole::CONTROLLED && peerControlled)
    {
        if (_credentials.tieBreaker < peerControlled->get())
        {
            sendResponse(localEndpoint, remotePort, StunError::Code::RoleConflict, msg, now, "Role Conflict");
            return;
        }
        else
        {
            _credentials.role = ice::IceRole::CONTROLLING;
        }
    }

    processValidStunRequest(localEndpoint, remotePort, msg, now);
}

void IceSession::onResponseReceived(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const StunMessage& msg,
    const uint64_t now)
{
    if (_state != State::GATHERING && _state != State::CONNECTING && _state != State::CONNECTED)
    {
        return;
    }

    if (_state != State::GATHERING)
    {
        if (!msg.isAuthentic(_hmacComputer.remote))
        {
            return;
        }
    }

    const auto candidatePair = findCandidatePair(localEndpoint, msg, remotePort);
    if (!candidatePair)
    {
        return; // not mine or old transaction. This happens after restartProbe as we clear transactions list.
    }

    const auto* errorCode = msg.getAttribute<StunError>(StunAttribute::ERROR_CODE);
    if (errorCode != nullptr)
    {
        logger::warn("ice error response %u, %s", _logId.c_str(), errorCode->getCode(), errorCode->getPhrase().c_str());
        if (errorCode->getCode() == StunError::Code::RoleConflict)
        {
            if (_credentials.role == ice::IceRole::CONTROLLED)
            {
                _credentials.role = ice::IceRole::CONTROLLING;
            }
            else
            {
                _credentials.role = ice::IceRole::CONTROLLED;
            }
            logger::info("assumed new role %s",
                _logId.c_str(),
                _credentials.role == ice::IceRole::CONTROLLING ? "controlling" : "controlled");
            candidatePair->onResponse(now, msg);
            sortCheckList();
            stateCheck(now);
        }
        else
        {
            candidatePair->onResponse(now, msg);
        }
        return;
    }

    auto addressAttribute = msg.getAttribute<ice::StunXorMappedAddress>(StunAttribute::XOR_MAPPED_ADDRESS);
    if (!addressAttribute)
    {
        candidatePair->state = ProbeState::Failed;
        return;
    }

    const auto mappedAddress = addressAttribute->getAddress(msg.header);
    candidatePair->onResponse(now, msg);
    if (candidatePair->state == ProbeState::Failed)
    {
        return;
    }

    candidatePair->accept();

    logger::debug("response from %s, rtt %" PRIu64 "ms",
        _logId.c_str(),
        remotePort.toString().c_str(),
        candidatePair->getRtt() / utils::Time::ms);

    if (candidatePair->localCandidate.address != mappedAddress)
    {
        if (candidatePair->gatheringProbe)
        {
            addLocalCandidate(IceCandidate(_component,
                localEndpoint->getTransportType(),
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::SRFLX,
                    candidatePair->localEndpoint.preference,
                    _component,
                    localEndpoint->getTransportType()),
                mappedAddress,
                candidatePair->localCandidate.baseAddress,
                IceCandidate::Type::SRFLX));
        }
        else
        {
            const auto candidateType = inferCandidateType(mappedAddress);
            candidatePair->localCandidate.address = mappedAddress;
            candidatePair->localCandidate.type = candidateType;
            candidatePair->localCandidate.priority = ice::IceCandidate::computeCandidatePriority(candidateType,
                candidatePair->localEndpoint.preference,
                _component,
                localEndpoint->getTransportType());
            if (candidateType == IceCandidate::Type::PRFLX &&
                candidatePair->localEndpoint.endpoint->getTransportType() == ice::TransportType::UDP)
            {
                addLocalCandidate(candidatePair->localCandidate);
            }
            sortCheckList();
        }
    }
    // could add remote candidate if remotePort seems to be elsewhere, but unlikely

    if (candidatePair->state == ProbeState::Succeeded &&
        (!_preliminaryCandidate || _preliminaryCandidate->state != ProbeState::Succeeded ||
            _preliminaryCandidate->getPriority(_credentials.role) < candidatePair->getPriority(_credentials.role)))
    {
        _preliminaryCandidate = candidatePair;
        if (_eventSink)
        {
            _eventSink->onIceCandidateChanged(this, localEndpoint, remotePort);
        }
    }
    stateCheck(now);
}

void IceSession::onStunPacketReceived(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);

    if (!ice::isStunMessage(data, len))
    {
        return;
    }

    if (_state == State::IDLE)
    {
        logger::debug("received STUN in idle state", _logId.c_str());
        return;
    }

    auto* msg = StunMessage::fromPtr(data);
    const auto method = msg->header.getMethod();
    if (method == StunHeader::BindingResponse || method == StunHeader::BindingErrorResponse)
    {
        if (!msg->isValid())
        {
            logger::debug("corrupt ICE response from %s", _logId.c_str(), remotePort.toString().c_str());
            return;
        }
        onPacketReceived(localEndpoint, remotePort, timestamp);
        onResponseReceived(localEndpoint, remotePort, *msg, timestamp);
    }
    else if (method == StunHeader::BindingRequest)
    {
        onPacketReceived(localEndpoint, remotePort, timestamp);
        onRequestReceived(localEndpoint, remotePort, *msg, timestamp);
    }
}

// only call this with valid packets of RTC related protocols
void IceSession::onPacketReceived(IceEndpoint* localEndpoint,
    const transport::SocketAddress& remotePort,
    uint64_t timestamp)
{
    auto* candidatePair = _candidatePairIndex.getItem(CandidatePairKey{localEndpoint, remotePort});

    if (!candidatePair)
    {
        for (auto& item : _candidatePairs)
        {
            if (item->localEndpoint.endpoint == localEndpoint &&
                (localEndpoint->getTransportType() == TransportType::TCP ||
                    item->remoteCandidate.address == remotePort))
            {
                candidatePair = item.get();
                _candidatePairIndex.emplace(CandidatePairKey{localEndpoint, remotePort}, candidatePair);
                break;
            }
        }
    }

    if (candidatePair)
    {
        candidatePair->receptionTimestamp = timestamp;
    }
}

void IceSession::onTcpDisconnect(IceEndpoint* localEndpoint)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    auto candidatePairIt = std::find_if(_candidatePairs.begin(),
        _candidatePairs.end(),
        [localEndpoint](
            const std::unique_ptr<CandidatePair>& probe) { return probe->localEndpoint.endpoint == localEndpoint; });
    if (candidatePairIt != _candidatePairs.end())
    {
        candidatePairIt->get()->onDisconnect();
    }
}

void IceSession::onTcpRemoved(const IceEndpoint* localEndpoint)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);

    for (auto it = _candidatePairs.rbegin(); it != _candidatePairs.rend(); ++it)
    {
        auto candidatePair = it->get();
        if (candidatePair->localEndpoint.endpoint == localEndpoint)
        {
            removeCandidatePair(it->get());
        }
    }

    auto endpointIt = std::find_if(_endpoints.begin(),
        _endpoints.end(),
        [localEndpoint](const EndpointInfo& iceEndpoint) { return iceEndpoint.endpoint == localEndpoint; });
    if (endpointIt != _endpoints.end())
    {
        _endpoints.erase(endpointIt);
    }
}

void IceSession::sendResponse(IceEndpoint* localEndpoint,
    const transport::SocketAddress& target,
    int code,
    const StunMessage& msg,
    const uint64_t timestamp,
    const std::string& errorPhrase)
{
    StunMessage response;
    response.header.transactionId = msg.header.transactionId;
    response.header.setMethod(code == 0 ? StunHeader::BindingResponse : StunHeader::BindingErrorResponse);
    response.add(StunGenericAttribute(StunAttribute::SOFTWARE, _config.software));
    response.add(StunXorMappedAddress(target, response.header));
    if (code != 0 && code != 200)
    {
        response.add(StunError(code, errorPhrase));
    }

    response.addMessageIntegrity(_hmacComputer.local);
    response.addFingerprint();
    localEndpoint->sendStunTo(target, response.header.transactionId.get(), &response, response.size(), timestamp);
}

const IceSession::EndpointInfo* IceSession::findEndpointInfo(const IceEndpoint* localEndpoint) const
{
    for (auto& endp : _endpoints)
    {
        if (endp.endpoint == localEndpoint)
        {
            return &endp;
        }
    }

    return nullptr;
}

bool IceSession::isAttached(const IceEndpoint* localEndpoint) const
{
    return !!findEndpointInfo(localEndpoint);
}

IceSession::CandidatePair* IceSession::findCandidatePair(const IceEndpoint* localEndpoint,
    const StunMessage& msg,
    const transport::SocketAddress& remotePort)
{
    auto* candidatePair = _candidatePairIndex.getItem(CandidatePairKey{localEndpoint, remotePort});
    if (candidatePair && candidatePair->hasTransaction(msg))
    {
        return candidatePair;
    }

    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->localEndpoint.endpoint == localEndpoint &&
            (localEndpoint->getTransportType() == TransportType::TCP ||
                candidatePair->remoteCandidate.address == remotePort) &&
            candidatePair->hasTransaction(msg))
        {
            return candidatePair.get();
        }
    }

    return nullptr;
}

// will try response index first, then linear search in candidate pairs
IceSession::CandidatePair* IceSession::findCandidatePair(const IceEndpoint* endpoint,
    const transport::SocketAddress& responder)
{
    auto* candidatePair = _candidatePairIndex.getItem(CandidatePairKey{endpoint, responder});
    if (candidatePair)
    {
        return candidatePair;
    }

    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->localEndpoint.endpoint == endpoint &&
            (endpoint->getTransportType() == TransportType::TCP || candidatePair->remoteCandidate.address == responder))
        {
            return candidatePair.get();
        }
    }

    return nullptr;
}

bool IceSession::isGatherComplete(uint64_t now)
{
    if (_state != State::GATHERING)
    {
        return true;
    }

    int unfinishedCount = 0;
    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->gatheringProbe && !candidatePair->isFinished())
        {
            ++unfinishedCount;
        }
    }
    return unfinishedCount == 0 ||
        getMaxStunServerCandidateAge(now) > _config.gather.additionalServerTimeout * utils::Time::ms;
}

uint64_t IceSession::getMaxStunServerCandidateAge(uint64_t now) const
{
    int64_t oldestSrflxCandidate = 0;
    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->gatheringProbe && candidatePair->state == ProbeState::Succeeded &&
            candidatePair->remoteCandidate.type == IceCandidate::Type::RELAY)
        {
            oldestSrflxCandidate = std::max(oldestSrflxCandidate, utils::Time::diff(candidatePair->startTime, now));
        }
    }
    return oldestSrflxCandidate;
}

/*
    ICE is complete when we have candidates (may create candidates only on incoming probes),
    and those candidates are all success or fail.
*/
bool IceSession::isIceComplete(const uint64_t now)
{
    size_t failCount = 0;
    int64_t probeAge = 60 * utils::Time::sec;
    for (auto candidatePair : _checklist)
    {
        if (candidatePair->state == ProbeState::Succeeded && candidatePair->nominated)
        {
            return true;
        }
        else if (candidatePair->state == ProbeState::Failed)
        {
            ++failCount;
            probeAge =
                std::max(int64_t(0), std::min(utils::Time::diff(candidatePair->nextTransmission, now), probeAge));
        }
    }

    if (probeAge < static_cast<int64_t>(_config.additionalCandidateTimeout * utils::Time::ms) &&
        utils::contains(_localCandidates, [](const IceCandidate& c) {
            return c.transportType == TransportType::TCP && c.tcpType == TcpType::PASSIVE;
        }))
    {
        return false;
    }
    if (utils::Time::diffGT(_sessionStart, now, _config.connectTimeout * utils::Time::ms))
    {
        return true;
    }

    // if all candidates failed, it could be that we just had unreachable HOST candidates. Give it some time for a PRFLX
    // to show up.
    if (failCount > 0 && failCount == _checklist.size() &&
        probeAge > static_cast<int64_t>(_config.additionalCandidateTimeout * utils::Time::ms))
    {
        logger::debug("all candidates failed", _logId.c_str());
        return true;
    }
    return false;
}

bool IceSession::hasNomination() const
{
    return _nomination && _nomination->state == ProbeState::Succeeded;
}

void IceSession::nominate(const uint64_t now)
{
    if (_credentials.role == IceRole::CONTROLLED)
    {
        return;
    }

    const bool hasTcpServerEndpoints = utils::contains(_localCandidates,
        [](const IceCandidate& c) { return c.transportType == TransportType::TCP && c.tcpType == TcpType::PASSIVE; });

    CandidatePair* nominee = nullptr;
    for (auto ct : _checklist)
    {
        if (ct->nominated)
        {
            return;
        }
        if (!nominee && ct->state == ProbeState::Succeeded)
        {
            // debatable if we should latch first detected UDP despite others having higher prio
            if (ct->remoteCandidate.type == IceCandidate::Type::HOST &&
                ct->localCandidate.transportType == TransportType::UDP)
            {
                nominee = ct;
            }
            else if (ct->localCandidate.transportType == TransportType::UDP && _connectedCount > 0)
            {
                // If _connectedCount > 0 that means we are nominating a new candidate and the old one
                // has become unviable. We should have a completed and sorted list at this point
                // Then lets nominate one as quick as possible to reduce media interruptions
                nominee = ct;
            }
            else if (!hasTcpServerEndpoints)
            {
                nominee = ct;
            }
            else if (utils::Time::diffGT(ct->startTime, now, _config.additionalCandidateTimeout * utils::Time::ms))
            {
                nominee = ct;
            }
        }
    }

    if (nominee)
    {
        nominee->nominated = true;
        _nomination = nominee;
        _preliminaryCandidate = nominee;
        logger::debug("nominated %s-%s from %zu pending candidates",
            _logId.c_str(),
            nominee->localCandidate.baseAddress.toString().c_str(),
            nominee->remoteCandidate.address.toString().c_str(),
            _checklist.size());
        nominee->send(now);
        return;
    }
}

void IceSession::stateCheck(const uint64_t now)
{
    if (_state == State::GATHERING && isGatherComplete(now))
    {
        for (auto& candidatePair : _candidatePairs)
        {
            if (candidatePair->state == ProbeState::InProgress)
            {
                candidatePair->state = ProbeState::Failed; // timeout
            }
        }
        reportState(State::READY);
    }
    else if (_state == State::CONNECTING)
    {
        nominate(now);
        if (isIceComplete(now))
        {
            if (hasNomination())
            {
                _state = State::CONNECTED;
                ++_connectedCount;
                pruneCandidatesAfterConnected(now);
            }
            else
            {
                _state = State::FAILED;
                cancelAllAfterFail(now);
            }

            if (_eventSink)
            {
                if (_state == State::CONNECTED)
                {
                    _eventSink->onIceCandidateChanged(this,
                        _nomination->localEndpoint.endpoint,
                        _nomination->remoteCandidate.address);
                }

                _eventSink->onIceStateChanged(this, _state);
                _eventSink->onIceCompleted(this);
            }
        }
    }
}

void IceSession::pruneCandidatesAfterConnected(uint64_t now)
{
    // this is not implements Freeing Candidates like RFC8446 suggests on 8.4
    // Instead we free the failed candidates and TCP candidates
    // and we keep the successful UDP as backup in frozen state.
    // It is entirely up to the client to continue probing them to keep them 'viable'.
    assert(_state == IceSession::State::CONNECTED);

    // We are going to clear all remote candidates without candidatePairs
    _remoteCandidates.clear();
    auto itEnd = _candidatePairs.end();

    for (auto it = _candidatePairs.begin(); it != itEnd;)
    {
        auto* candidatePair = it->get();
        if (candidatePair->nominated)
        {
            addCandidateToListIfNotPresent(_remoteCandidates, candidatePair->remoteCandidate);
            ++it;
            continue;
        }

        if (candidatePair->localCandidate.transportType == TransportType::TCP)
        {
            candidatePair->freeze();
            candidatePair->state = ProbeState::Failed;
            onCandidatePairRemoved(candidatePair);
            std::iter_swap(it, --itEnd);
            continue;
        }

        if (candidatePair->state == ProbeState::Failed)
        {
            logger::info("remove failed candidate: %s %s %s - %s",
                _logId.c_str(),
                toString(candidatePair->localEndpoint.endpoint->getTransportType()).c_str(),
                toString(candidatePair->remoteCandidate.type).c_str(),
                candidatePair->localCandidate.address.toFixedString().c_str(),
                maybeMasked(candidatePair->remoteCandidate.address).c_str());

            onCandidatePairRemoved(candidatePair);
            std::iter_swap(it, --itEnd);
            continue;
        }

        if (candidatePair->state == ProbeState::Succeeded)
        {
            // If succeeded AND not nominated. Stop probing
            candidatePair->freeze();
        }

        addCandidateToListIfNotPresent(_remoteCandidates, candidatePair->remoteCandidate);
        ++it;
    }

    _candidatePairs.erase(itEnd, _candidatePairs.end());
}

void IceSession::cancelAllAfterFail(uint64_t now)
{
    assert(_state == IceSession::State::FAILED);
    for (auto& candidatePair : _candidatePairs)
    {
        candidatePair->freeze();
        candidatePair->state = ProbeState::Failed;
    }
}

void IceSession::removeUnviableRemoteCandidates(uint64_t now)
{
    _remoteCandidates.clear();

    auto itEnd = _candidatePairs.end();

    for (auto it = _candidatePairs.begin(); it != itEnd;)
    {
        auto* candidatePair = it->get();

        // Let's keep the nominated regardless the current state
        if (candidatePair->nominated)
        {
            addCandidateToListIfNotPresent(_remoteCandidates, candidatePair->remoteCandidate);
            ++it;
            continue;
        }

        if (candidatePair->isViable(now))
        {
            addCandidateToListIfNotPresent(_remoteCandidates, candidatePair->remoteCandidate);
            ++it;
            continue;
        }

        logger::info("Remove unviable candidate pair: %s %s %s - %s",
            _logId.c_str(),
            toString(candidatePair->localEndpoint.endpoint->getTransportType()).c_str(),
            toString(candidatePair->remoteCandidate.type).c_str(),
            candidatePair->localCandidate.address.toFixedString().c_str(),
            maybeMasked(candidatePair->remoteCandidate.address).c_str());

        candidatePair->state = ProbeState::Failed;
        std::iter_swap(it, --itEnd);
        onCandidatePairRemoved(candidatePair);
    }

    _candidatePairs.erase(itEnd, _candidatePairs.end());
}

void IceSession::restartProbes(uint64_t now)
{
    assert(_state == IceSession::State::CONNECTING);

    logger::info("restarting probes", _logId.c_str());
    auto transmitTime = now;
    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->state == ProbeState::Frozen)
        {
            candidatePair->restartProbe(now, true);
            candidatePair->nextTransmission = transmitTime;
            if (transmitTime == now)
            {
                candidatePair->send(now);
            }

            transmitTime += _config.probeReleasePace * utils::Time::ms;
        }
    }
}

// -1 means no more timeouts
int64_t IceSession::nextTimeout(const uint64_t now) const
{
    if (_state == State::FAILED || _state == State::IDLE || _state == State::READY)
    {
        return -1;
    }

    int64_t minTimeout = _config.keepAliveInterval * utils::Time::ms;
    for (auto& candidatePair : _candidatePairs)
    {
        const auto timeout = candidatePair->nextTimeout(now);
        if (timeout < 0)
        {
            continue;
        }

        minTimeout = std::min(minTimeout, timeout);
    }

    if (_state == State::GATHERING)
    {
        auto oldestStunServerCandidate = getMaxStunServerCandidateAge(now);
        if (oldestStunServerCandidate > 0)
        {
            minTimeout = std::min(minTimeout, static_cast<int64_t>(_config.maxRTO * utils::Time::ms));
        }
    }
    if (_state == State::CONNECTING)
    {
        minTimeout = std::min(minTimeout, static_cast<int64_t>(_config.maxRTO * utils::Time::ms));
    }

    return minTimeout;
}

int64_t IceSession::processTimeout(const uint64_t now)
{
    if (_state == State::IDLE || _state >= State::FAILED)
    {
        return -1;
    }

    DBGCHECK_SINGLETHREADED(_mutexGuard);
    bool hadInProgressBeforeProcess = false;
    bool hasInProgressAfterProcess = false;
    for (auto it = _candidatePairs.rbegin(); it != _candidatePairs.rend(); ++it)
    {
        auto* candidatePair = it->get();

        const auto currentState = candidatePair->state;
        hadInProgressBeforeProcess = hadInProgressBeforeProcess || currentState == ProbeState::InProgress;
        candidatePair->processTimeout(now);
        hasInProgressAfterProcess = hasInProgressAfterProcess || candidatePair->state == ProbeState::InProgress;

        if (currentState == ProbeState::InProgress && candidatePair->state == ProbeState::Failed)
        {
            if (candidatePair->localCandidate.transportType == ice::TransportType::TCP)
            {
                removeCandidatePair(candidatePair);
            }
        }
        else if (_nomination == candidatePair && candidatePair->nominatedInProgressNextTimedout(now) == 0)
        {
            logger::info("found nominated pair in state %s", _logId.c_str(), toString(candidatePair->state));
            reportState(State::CONNECTING);
            _sessionStart = now;
            candidatePair->nominated = false;
            _nomination = nullptr; // re-probe
            sortCheckList();
            restartProbes(now);
        }
        else if (!_nomination && _state == State::CONNECTED && currentState == ProbeState::InProgress &&
            candidatePair->state == ProbeState::Succeeded)
        {
            sortCheckList();
            if (candidatePair == _checklist.front())
            {
                _preliminaryCandidate = candidatePair;
                if (_eventSink)
                {
                    _eventSink->onIceCandidateChanged(this,
                        candidatePair->localEndpoint.endpoint,
                        candidatePair->remoteCandidate.address);
                }
            }
        }
    }

    const bool lastInProgressHasCompleted = hadInProgressBeforeProcess && !hasInProgressAfterProcess;
    if (lastInProgressHasCompleted && _state == State::CONNECTED)
    {
        removeUnviableRemoteCandidates(now);
    }

    stateCheck(now);
    return nextTimeout(now);
}

const std::pair<std::string, std::string>& IceSession::getLocalCredentials() const
{
    return _credentials.local;
}

// only used if you need same credentials for multiple sessions
void IceSession::setLocalCredentials(const std::pair<std::string, std::string>& credentials)
{
    _credentials.local = credentials;
    _hmacComputer.local.init(credentials.second.c_str(), credentials.second.size());
}

void IceSession::setRemoteCredentials(const std::string& ufrag, const std::string& pwd)
{
    setRemoteCredentials(std::make_pair(ufrag, pwd));
}

void IceSession::setRemoteCredentials(const std::pair<std::string, std::string>& credentials)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    _credentials.remote = credentials;
    if (credentials.second.empty())
    {
        return;
    }

    _hmacComputer.remote.init(credentials.second.c_str(), credentials.second.size());
};

// targetBuffer must be length + 1 for null termination
void IceSession::generateCredentialString(StunTransactionIdGenerator& idGenerator, char* targetBuffer, int length)
{
    const char* approvedLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "0123456789"
                                  "+/";
    const int COUNT = std::strlen(approvedLetters);
    Int96 id;
    for (int i = 0; i < length; i++)
    {
        if (i % 3 == 0)
        {
            id = idGenerator.next();
            targetBuffer[i] = approvedLetters[(id.w0 & 0x3Fu) % COUNT];
        }
        else if (i % 3 == 1)
        {
            targetBuffer[i] = approvedLetters[(id.w1 & 0x3Fu) % COUNT];
        }
        else if (i % 3 == 2)
        {
            targetBuffer[i] = approvedLetters[(id.w2 & 0x3Fu) % COUNT];
        }
    }
    targetBuffer[length] = '\0';
}

IceSession::CandidatePair::CandidatePair(IceSession& iceSession,
    const EndpointInfo& endpoint,
    const IceCandidate& local,
    const IceCandidate& remote,
    bool gathering)
    : localCandidate(local),
      remoteCandidate(remote),
      localEndpoint(endpoint),
      gatheringProbe(gathering),
      startTime(0),
      nextTransmission(0),
      nominated(false),
      accepted(false),
      receptionTimestamp(0),
      state(ProbeState::Waiting),
      _iceSession(iceSession),
      _transmitInterval(iceSession._config.RTO),
      _replies(0),
      _errorCode(IceError::Success),
      _minRtt(utils::Time::minute)
{
}

uint64_t IceSession::CandidatePair::getPriority(const IceRole role) const
{
    const bool diff = (role == IceRole::CONTROLLED ? remoteCandidate.priority > localCandidate.priority
                                                   : remoteCandidate.priority < localCandidate.priority);

    const auto compoundPriority =
        (static_cast<uint64_t>(std::min(localCandidate.priority, remoteCandidate.priority)) << 32) +
        std::max(localCandidate.priority, remoteCandidate.priority) * 2 + (diff ? 1 : 0);

    return compoundPriority;
}

// returns -1 when no more timeouts are needed
int64_t IceSession::CandidatePair::nextTimeout(uint64_t now) const
{
    if (state == ProbeState::Failed || state == ProbeState::Frozen)
    {
        return -1;
    }

    if (state == ProbeState::Succeeded && gatheringProbe)
    {
        return -1;
    }

    const int64_t maybeTimeout = nominatedInProgressNextTimedout(now);
    if (maybeTimeout > -1)
    {
        return std::max(int64_t(0), std::min(maybeTimeout, utils::Time::diff(now, nextTransmission)));
    }

    return std::max(int64_t(0), utils::Time::diff(now, nextTransmission));
}

void IceSession::CandidatePair::restartProbe(const uint64_t now, bool releaseNomination)
{
    _transactions.clear();
    _replies = 0;
    state = ProbeState::InProgress;
    nominated = nominated && !releaseNomination;
}

int64_t IceSession::CandidatePair::nominatedInProgressNextTimedout(const uint64_t now) const
{
    if (nominated)
    {
        if (state == ProbeState::Succeeded)
        {
            return -1;
        }

        if (state == ProbeState::InProgress)
        {
            const auto timeoutMs =
                std::min(_minRtt * 4, _iceSession._config.nominatedInProgressMaxTimeout * utils::Time::ms);
            return std::max(int64_t(0), utils::Time::diff(now, startTime + timeoutMs));
        }
    }

    return -1;
}

void IceSession::CandidatePair::send(const uint64_t now)
{
    if (!gatheringProbe && _iceSession._credentials.remote.second.empty())
    {
        // cannot send yet as we have not receive credentials
        nextTransmission = now + 50 * utils::Time::ms;
        return;
    }

    if (_transactions.empty())
    {
        if (localCandidate.transportType == ice::TransportType::UDP)
        {
            _transmitInterval = _iceSession._config.RTO * utils::Time::ms;
            if (_replies > 0)
            {
                _transmitInterval =
                    std::max(_transmitInterval, _minRtt + utils::Time::ms * _iceSession._config.transmitIntervalExtend);
            }
            _transmitInterval = std::min(_transmitInterval, _iceSession._config.maxRTO * utils::Time::ms);
        }
        else
        {
            _transmitInterval = _iceSession._config.keepAliveInterval * utils::Time::ms;
        }
        nextTransmission = now + _transmitInterval;
        startTime = now;
    }
    else if (state == ProbeState::Succeeded)
    {
        _transmitInterval = std::min(_transmitInterval * 2, _iceSession._config.keepAliveInterval * utils::Time::ms);

        if (utils::Time::diffGE(nextTransmission, now, _transmitInterval))
        {
            nextTransmission = now;
        }
        else
        {
            nextTransmission = nextTransmission + _transmitInterval;
        }
    }
    else
    {
        if (localCandidate.transportType == ice::TransportType::UDP)
        {
            _transmitInterval = std::min(_transmitInterval * 2, _iceSession._config.maxRTO * utils::Time::ms);
        }
        nextTransmission = nextTransmission + _transmitInterval;
    }

    StunTransaction transaction;
    auto stunMessage = original;
    stunMessage.header.transactionId.set(_iceSession._idGenerator.next());
    transaction.id = stunMessage.header.transactionId;
    stunMessage.add(StunGenericAttribute(StunAttribute::USERNAME,
        _iceSession._credentials.remote.first + ":" + _iceSession._credentials.local.first));
    stunMessage.add(
        StunAttribute64(_iceSession._credentials.role == IceRole::CONTROLLING ? StunAttribute::ICE_CONTROLLING
                                                                              : StunAttribute::ICE_CONTROLLED,
            _iceSession._credentials.tieBreaker));
    if (nominated && _iceSession._credentials.role == IceRole::CONTROLLING)
    {
        stunMessage.add(StunAttribute(StunAttribute::USE_CANDIDATE));
    }

    if (!gatheringProbe)
    {
        stunMessage.addMessageIntegrity(_iceSession._hmacComputer.remote);
        stunMessage.addFingerprint();
    }

    transaction.time = now;
    if (state == ProbeState::Waiting)
    {
        state = ProbeState::InProgress;
    }

    _transactions.push_back(transaction);
    const size_t pendingTransactionLimit = std::max(uint64_t(1), utils::Time::sec * 4 / _transmitInterval);
    while (_transactions.size() > pendingTransactionLimit)
    {
        if (localEndpoint.endpoint)
        {
            auto& frontTransaction = _transactions.front();
            if (!frontTransaction.acknowledged())
            {
                localEndpoint.endpoint->cancelStunTransaction(frontTransaction.id.get());
            }
        }
        _transactions.pop_front();
    }

    for (uint32_t i = 0; i < (state != ProbeState::Succeeded ? _iceSession._config.probeReplicates : 1); ++i)
    {
        if (logger::_logLevel == logger::Level::DBG)
        {
            logger::debug("%s probing %s %s-%s -> %s, pending transactions %zu",
                _iceSession._logId.c_str(),
                toString(localEndpoint.endpoint->getTransportType()).c_str(),
                toString(localCandidate.type).c_str(),
                localEndpoint.endpoint->getLocalPort().toString().c_str(),
                (localCandidate.baseAddress != localCandidate.address) ? localCandidate.address.toString().c_str() : "",
                remoteCandidate.address.toString().c_str(),
                pendingTransactionCount());
        }

        localEndpoint.endpoint->sendStunTo(remoteCandidate.address,
            transaction.id.get(),
            &stunMessage,
            stunMessage.size(),
            now);
    }
}

void IceSession::CandidatePair::onResponse(uint64_t now, const StunMessage& response)
{
    auto transaction = findTransaction(response);
    if (!transaction)
    {
        return;
    }

    cancelPendingTransactionsBefore(*transaction);

    if (_transactions.size() == 1 && localEndpoint.endpoint->getTransportType() == ice::TransportType::TCP)
    {
        localCandidate.baseAddress = localEndpoint.endpoint->getLocalPort();
        if (localCandidate.address.getPort() == 0)
        {
            localCandidate.address.setPort(localCandidate.baseAddress.getPort());
        }
    }

    transaction->rtt = std::max(int64_t(0), utils::Time::diff(transaction->time, now));
    _minRtt = std::min(transaction->rtt, _minRtt);

    auto errorAttribute = response.getAttribute<StunError>(StunAttribute::ERROR_CODE);
    if (errorAttribute)
    {
        if (errorAttribute->getCode() == StunError::Code::RoleConflict)
        {
            state = ProbeState::Waiting;
            nominated = false;
            nextTransmission = now;
            _replies = 0;
        }
        else
        {
            failCandidate(static_cast<IceError>(errorAttribute->getCode()));
            state = ProbeState::Failed;
            ++_replies;
        }
        return;
    }

    if (++_replies > 0)
    {
        if (nominated || _iceSession._state <= ice::IceSession::State::CONNECTING)
        {
            state = ProbeState::Succeeded;
        }
        else
        {
            freeze();
        }
    }
}

void IceSession::CandidatePair::onDisconnect()
{
    failCandidate(IceError::ConnectionTimeoutOrFailure);
    return;
}

void IceSession::CandidatePair::nominate(uint64_t now)
{
    nominated = true;
    state = ProbeState::Succeeded;
    nextTransmission = now;
}

void IceSession::CandidatePair::accept()
{
    if (!accepted)
    {
        accepted = true;
        auto* eventSink = _iceSession._eventSink;
        if (eventSink)
        {
            eventSink->onIceCandidateAccepted(&_iceSession, localEndpoint.endpoint, remoteCandidate);
        }
    }
}

void IceSession::CandidatePair::freeze()
{
    state = ProbeState::Frozen;
    cancelPendingTransactions();
}

void IceSession::CandidatePair::failCandidate(IceError reason)
{
    logger::debug("candidate failed %s-%s, reason %u",
        _iceSession._logId.c_str(),
        localCandidate.address.toString().c_str(),
        remoteCandidate.address.toString().c_str(),
        static_cast<std::underlying_type_t<IceError>>(reason));
    state = ProbeState::Failed;
    _errorCode = reason;
    cancelPendingTransactions();
}

void IceSession::CandidatePair::cancelPendingTransactions()
{
    if (localEndpoint.endpoint)
    {
        return;
    }
    for (auto& transaction : _transactions)
    {
        if (!transaction.acknowledged())
        {
            localEndpoint.endpoint->cancelStunTransaction(transaction.id.get());
        }
    }
    _transactions.clear();
}

/**
 * Cancelling any request sent before this acknowledged one ensures that minimum
 * count of pending requests are registered at the end point.
 */
void IceSession::CandidatePair::cancelPendingTransactionsBefore(StunTransaction& transaction)
{
    while (!_transactions.empty() && &_transactions.front() != &transaction && !_transactions.front().acknowledged())
    {
        localEndpoint.endpoint->cancelStunTransaction(_transactions.front().id.get());
        _transactions.pop_front();
    }
}

uint64_t IceSession::CandidatePair::pendingRequestAge(const uint64_t now) const
{
    if (_transactions.empty())
    {
        return 0;
    }

    auto lastTransaction = _transactions.back();
    if (lastTransaction.acknowledged())
    {
        return 0;
    }

    return std::max(int64_t(0), static_cast<int64_t>(now - lastTransaction.time));
}

size_t IceSession::CandidatePair::pendingTransactionCount() const
{
    size_t count = 0;
    for (auto it = _transactions.rbegin(); it != _transactions.rend(); ++it)
    {
        if (it->acknowledged())
        {
            return count;
        }
        ++count;
    }

    return count;
}

void IceSession::CandidatePair::processTimeout(const uint64_t now)
{
    if (state == ProbeState::Succeeded && gatheringProbe)
    {
        return;
    }
    if (gatheringProbe && state == ProbeState::InProgress &&
        utils::Time::diffGT(startTime, now, _iceSession._config.gather.probeTimeout * utils::Time::ms))
    {
        failCandidate(IceError::RequestTimeout);
        return;
    }

    if (state == ProbeState::Failed)
    {
        return;
    }

    if (state == ProbeState::Succeeded)
    {
        if (nominated &&
            (utils::Time::diffGE(receptionTimestamp, now, _iceSession._config.hostProbeTimeout * utils::Time::ms) ||
                pendingTransactionCount() > 2))
        {
            logger::info("restart prob due to lack of inbound traffic", _iceSession._logId.c_str());
            restartProbe(now, false);
            send(now);
            return;
        }

        const auto requestAge = pendingRequestAge(now);
        if (requestAge > _minRtt + _iceSession._config.RTO * utils::Time::ms)
        {
            logger::debug("request unacknowledged for %" PRIu64 "ms",
                _iceSession._logId.c_str(),
                requestAge / utils::Time::ms);
            _transmitInterval = std::max(_iceSession._config.RTO * utils::Time::ms, _minRtt);
            nextTransmission = now;
            send(now);
        }
    }

    if (nextTimeout(now) == 0)
    {
        send(now);
    }

    if (state == ProbeState::InProgress)
    {
        if (remoteCandidate.type == IceCandidate::Type::HOST &&
            utils::Time::diffGT(startTime, now, _iceSession._config.hostProbeTimeout * utils::Time::ms))
        {
            failCandidate(IceError::RequestTimeout);
        }
        else if (utils::Time::diffGT(startTime, now, _iceSession._config.reflexiveProbeTimeout * utils::Time::ms))
        {
            failCandidate(IceError::RequestTimeout);
        }
    }
}

bool IceSession::CandidatePair::hasTransaction(const StunMessage& response) const
{
    const auto id = response.header.transactionId.get();
    for (auto& transaction : _transactions)
    {
        if (transaction.id.get() == id)
        {
            return true;
        }
    }
    return false;
}

IceSession::StunTransaction* IceSession::CandidatePair::findTransaction(const StunMessage& response)
{
    const auto id = response.header.transactionId.get();
    for (auto& transaction : _transactions)
    {
        if (transaction.id.get() == id)
        {
            return &transaction;
        }
    }
    return nullptr;
}

void IceSession::stop()
{
    reportState(State::IDLE);
}

void IceSession::reportState(State newState)
{
    const auto oldState = _state;
    _state = newState;
    if (_eventSink && oldState != newState)
    {
        _eventSink->onIceStateChanged(this, newState);
        logger::debug("%s", _logId.c_str(), toString(newState));
    }
}

IceCandidate::Type IceSession::inferCandidateType(const transport::SocketAddress& mappedAddress)
{
    for (auto& candidate : _localCandidates)
    {
        if (candidate.address == mappedAddress)
        {
            return candidate.type;
        }
    }
    return ice::IceCandidate::Type::PRFLX;
}

void IceSession::onCandidatePairRemoved(CandidatePair* candidatePair)
{
    for (auto it = _checklist.begin(); it != _checklist.end(); ++it)
    {
        if ((*it) == candidatePair)
        {
            _checklist.erase(it);
            break;
        }
    }

    _candidatePairIndex.erase(
        CandidatePairKey{candidatePair->localEndpoint.endpoint, candidatePair->remoteCandidate.address});

    candidatePair->accepted = false;

    if (_eventSink)
    {
        _eventSink->onIceDiscardCandidate(this,
            candidatePair->localEndpoint.endpoint,
            candidatePair->remoteCandidate.address);
    }
}

void IceSession::removeCandidatePair(CandidatePair* cp)
{
    std::unique_ptr<ice::IceSession::CandidatePair> candidateRemoved;
    for (auto it = _candidatePairs.begin(); it != _candidatePairs.end(); ++it)
    {
        if (it->get() == cp)
        {
            candidateRemoved = std::move(*it);
            _candidatePairs.erase(it);
            break;
        }
    }

    if (candidateRemoved)
    {
        onCandidatePairRemoved(cp);
    }
}

const char* toString(IceSession::State s)
{
    static const char* iceStateStrings[] = {"IDLE", "GATHERING", "READY", "CONNECTING", "CONNECTED", "FAILED"};
    return iceStateStrings[static_cast<int>(s)];
}

const char* toString(IceSession::ProbeState s)
{
    static const char* stateStrings[] = {"Waiting", "InProgress", "Succeeded", "Failed", "Frozen"};
    return stateStrings[static_cast<int>(s)];
}
} // namespace ice
