#include "IceSession.h"
#include "Stun.h"
#include "logger/Logger.h"
#include "utils/ContainerAlgorithms.h"
#include "utils/Time.h"
namespace ice
{
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
      _component(component),
      _tcpProbeCount(0),
      _config(config),
      _state(State::IDLE),
      _eventSink(eventSink),
      _credentials(role, static_cast<uint64_t>(_idGenerator.next() & ~(0ull))),
      _sessionStart(0)
{
    char ufrag[14 + 1];
    char pwd[24 + 1]; // length selected to make attribute *4 length

    generateCredentialString(_idGenerator, ufrag, sizeof(ufrag) - 1);
    generateCredentialString(_idGenerator, pwd, sizeof(pwd) - 1);
    _credentials.local = std::make_pair<std::string, std::string>(ufrag, pwd);
}

// add most preferred UDP end point first. It will affect prioritization of candidates
void IceSession::attachLocalEndpoint(IceEndpoint* endpoint)
{
    if (endpoint->getTransportType() != ice::TransportType::UDP)
    {
        assert(false);
        return;
    }

    const int preference = 256 - _endpoints.size();
    _endpoints.push_back(EndpointInfo(endpoint, preference));

    const auto address = endpoint->getLocalPort();
    if (endpoint->getTransportType() == TransportType::UDP &&
        !utils::contains(_localCandidates, [address](const ice::IceCandidate& c) { return c.baseAddress == address; }))
    {
        _localCandidates.emplace_back(IceCandidate(_component,
            endpoint->getTransportType(),
            ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::HOST,
                preference,
                _component,
                endpoint->getTransportType()),
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
            _candidatePairs.emplace_back(std::make_unique<CandidatePair>(_config,
                endpoint,
                localCandidate,
                IceCandidate(_component,
                    endpoint.endpoint->getTransportType(),
                    0,
                    _stunServers[stunIndex],
                    _stunServers[stunIndex],
                    IceCandidate::Type::RELAY),
                _idGenerator,
                _credentials,
                _logId,
                true));
            auto& ct = _candidatePairs.back();
            ct->original.header.setMethod(StunHeader::BindingRequest);
        }
    }

    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->gatheringProbe && candidatePair->state == CandidatePair::Waiting)
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
    _credentials.role = role;
    _sessionStart = timestamp;
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

void IceSession::addProbeForRemoteCandidate(EndpointInfo& endpoint, const IceCandidate& remoteCandidate)
{
    const auto endpointAddress = endpoint.endpoint->getLocalPort();
    if (endpointAddress.getFamily() != remoteCandidate.address.getFamily())
    {
        return;
    }

    if (remoteCandidate.transportType == TransportType::UDP)
    {
        IceCandidate& localCandidate =
            *std::find_if(_localCandidates.begin(), _localCandidates.end(), [endpointAddress](const IceCandidate& c) {
                return endpointAddress == c.baseAddress && c.type == IceCandidate::Type::HOST;
            });

        _candidatePairs.emplace_back(std::make_unique<CandidatePair>(_config,
            endpoint,
            localCandidate,
            remoteCandidate,
            _idGenerator,
            _credentials,
            _logId,
            false));
    }
    else
    {
        _candidatePairs.emplace_back(std::make_unique<CandidatePair>(_config,
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
            _idGenerator,
            _credentials,
            _logId,
            false));
    }
    auto& candidatePair = _candidatePairs.back();

    StunMessage& iceProbe(candidatePair->original);
    iceProbe.header.transactionId.set(_idGenerator.next());
    iceProbe.header.setMethod(StunHeader::BindingRequest);
    iceProbe.add(StunGenericAttribute(StunAttribute::SOFTWARE, _config.software));
    iceProbe.add(
        StunPriority(static_cast<uint32_t>(ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::PRFLX,
            endpoint.preference,
            _component,
            remoteCandidate.transportType))));

    logger::debug("added candidate pair %s-%s",
        _logId.c_str(),
        endpointAddress.toString().c_str(),
        remoteCandidate.address.toString().c_str());
}

void IceSession::addLocalCandidate(const IceCandidate& candidate)
{
    if (!utils::contains(_localCandidates, [candidate](const IceCandidate& x) {
            return x.address == candidate.address && x.baseAddress == candidate.baseAddress &&
                x.transportType == candidate.transportType;
        }))
    {
        _localCandidates.push_back(candidate);
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

void IceSession::addLocalCandidate(const transport::SocketAddress& publicAddress, IceEndpoint* endpoint)
{
    for (auto& endpointInfo : _endpoints)
    {
        if (endpointInfo.endpoint == endpoint)
        {
            addLocalCandidate(IceCandidate(_component,
                endpoint->getTransportType(),
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::SRFLX,
                    endpointInfo.preference,
                    _component,
                    endpoint->getTransportType()),
                publicAddress,
                endpointInfo.endpoint->getLocalPort(),
                IceCandidate::Type::SRFLX));
            return;
        }
    }
}

const IceCandidate& IceSession::addRemoteCandidate(const IceCandidate& candidate)
{
    auto it = std::find_if(_remoteCandidates.cbegin(), _remoteCandidates.cend(), [candidate](const IceCandidate& x) {
        return x.transportType == candidate.transportType && x.address == candidate.address;
    });

    if (it != _remoteCandidates.cend())
    {
        return *it;
    }

    _remoteCandidates.push_back(candidate);
    return _remoteCandidates.back();
}

void IceSession::addRemoteCandidate(const IceCandidate& candidate, IceEndpoint* tcpEndpoint)
{
    if (isAttached(tcpEndpoint))
    {
        return;
    }

    _remoteCandidates.push_back(candidate);

    EndpointInfo endpointInfo(tcpEndpoint, 128 - _tcpProbeCount++);
    _endpoints.push_back(endpointInfo);
    addProbeForRemoteCandidate(endpointInfo, candidate);
}

std::vector<IceCandidate> IceSession::getLocalCandidates() const
{
    return _localCandidates;
}

std::pair<IceCandidate, IceCandidate> IceSession::getSelectedPair() const
{
    for (auto candidatePair : _checklist)
    {
        if (candidatePair->state == CandidatePair::Succeeded && candidatePair->nominated)
        {
            return std::make_pair(candidatePair->localCandidate, candidatePair->remoteCandidate);
        }
    }
    return std::make_pair<IceCandidate, IceCandidate>(IceCandidate(), IceCandidate());
}

uint64_t IceSession::getSelectedPairRtt() const
{
    for (auto candidatePair : _checklist)
    {
        if (candidatePair->state == CandidatePair::Succeeded && candidatePair->nominated)
        {
            return candidatePair->getRtt();
        }
    }
    return 0;
}

bool IceSession::isRequestAuthentic(const void* data, size_t len) const
{
    const auto* stunMessage = StunMessage::fromPtr(data);

    if (stunMessage && stunMessage->isValid() && stunMessage->header.isRequest() &&
        stunMessage->isAuthentic(_credentials.local.second))
    {
        const auto* attribute = stunMessage->getAttribute<StunUserName>(StunAttribute::USERNAME);
        return attribute && attribute->isTargetUser(_credentials.local.first.c_str());
    }

    return false;
}

bool IceSession::isResponseAuthentic(const void* data, size_t len) const
{
    const auto* stunMessage = StunMessage::fromPtr(data);
    return stunMessage && stunMessage->isValid() && stunMessage->header.isResponse() &&
        stunMessage->isAuthentic(_credentials.remote.second);
}

void IceSession::onRequestReceived(IceEndpoint* endpoint,
    const transport::SocketAddress& sender,
    const StunMessage& msg,
    const uint64_t now)
{
    if (!msg.isValid() || msg.getAttribute(StunAttribute::MESSAGE_INTEGRITY) == nullptr ||
        msg.getAttribute(StunAttribute::USERNAME) == nullptr)
    {
        sendResponse(endpoint, sender, StunError::Code::BadRequest, msg, now, "Bad Request");
        return;
    }

    const auto userNames = msg.getAttribute<StunUserName>(StunAttribute::USERNAME)->getNames();
    if (userNames.first != _credentials.local.first)
    {
        sendResponse(endpoint,
            sender,
            StunError::Code::Unauthorized,
            msg,
            now,
            "Unknown user " + userNames.first + ":" + userNames.second);
        return;
    }
    if (!msg.isAuthentic(_credentials.local.second))
    {
        sendResponse(endpoint, sender, StunError::Code::Unauthorized, msg, now, "Unauthorized");
        return;
    }
    const auto* peerControlling = msg.getAttribute<StunAttribute64>(StunAttribute::ICE_CONTROLLING);
    const auto* peerControlled = msg.getAttribute<StunAttribute64>(StunAttribute::ICE_CONTROLLED);
    if (_credentials.role == ice::IceRole::CONTROLLING && peerControlling)
    {
        if (_credentials.tieBreaker >= peerControlling->get())
        {
            sendResponse(endpoint, sender, StunError::Code::RoleConflict, msg, now, "Role Conflict");
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
            sendResponse(endpoint, sender, StunError::Code::RoleConflict, msg, now, "Role Conflict");
            return;
        }
        else
        {
            _credentials.role = ice::IceRole::CONTROLLING;
        }
    }

    logger::debug("probe from %s", _logId.c_str(), sender.toString().c_str());
    sendResponse(endpoint, sender, 0, msg, now);
    if (_eventSink)
    {
        _eventSink->onIcePreliminary(this, endpoint, sender);
    }

    int remoteCandidatePriority = 0;
    const auto prioAttribute = msg.getAttribute<StunPriority>(StunAttribute::PRIORITY);
    if (prioAttribute)
    {
        remoteCandidatePriority = prioAttribute->value;
    }
    if (endpoint->getTransportType() == TransportType::TCP && !isAttached(endpoint))
    {
        IceCandidate remoteCandidate(_component,
            endpoint->getTransportType(),
            remoteCandidatePriority,
            sender,
            sender,
            IceCandidate::Type::PRFLX);
        addRemoteCandidate(remoteCandidate, endpoint);
        auto& candidatePair = _candidatePairs.back();
        if (_state == State::CONNECTING)
        {
            candidatePair->send(now);
            sortCheckList();
        }
    }

    if (endpoint->getTransportType() == TransportType::UDP && (_state == State::CONNECTING || _state == State::READY) &&
        !utils::contains(_remoteCandidates,
            [sender](const IceCandidate& x) { return x.address == sender && x.transportType == TransportType::UDP; }))
    {
        IceCandidate remoteCandidate(_component,
            endpoint->getTransportType(),
            remoteCandidatePriority,
            sender,
            sender,
            IceCandidate::Type::PRFLX);
        remoteCandidate = addRemoteCandidate(remoteCandidate);
        if (_state == State::CONNECTING)
        {
            for (auto& localEndpoint : _endpoints)
            {
                addProbeForRemoteCandidate(localEndpoint, remoteCandidate);
            }
            sortCheckList();
            processTimeout(now);
        }
    }

    auto useCandidate = msg.getAttribute(StunAttribute::USE_CANDIDATE);
    if (_credentials.role == IceRole::CONTROLLED && useCandidate)
    {
        for (auto& candidatePair : _candidatePairs)
        {
            if (candidatePair->remoteCandidate.address == sender && candidatePair->localEndpoint.endpoint == endpoint)
            {
                logger::debug("remote nominated %s-%s",
                    _logId.c_str(),
                    candidatePair->localCandidate.address.toString().c_str(),
                    candidatePair->remoteCandidate.address.toString().c_str());
                candidatePair->nominate(now);
                processTimeout(now);
                break;
            }
        }
    }
}

void IceSession::onResponseReceived(IceEndpoint* endpoint,
    const transport::SocketAddress& sender,
    const StunMessage& msg,
    const uint64_t now)
{
    if (_state != State::GATHERING && _state != State::CONNECTING && _state != State::CONNECTED)
    {
        return;
    }
    auto userNameAttribute = msg.getAttribute<StunUserName>(StunAttribute::USERNAME);
    if (userNameAttribute)
    {
        const auto userNames = userNameAttribute->getNames();
        if (userNames.second != _credentials.local.first || userNames.first != _credentials.remote.first)
        {
            logger::debug("Unrecognized user name in ICE response from %s, %s",
                _logId.c_str(),
                sender.toString().c_str(),
                (userNames.first + ":" + userNames.second).c_str());
            return;
        }
    }

    if (_state != State::GATHERING)
    {
        if (!msg.isAuthentic(_credentials.remote.second))
        {
            return;
        }
    }

    const auto candidatePair = findCandidatePair(endpoint, msg, sender);
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
        candidatePair->state = CandidatePair::Failed;
        return;
    }

    const auto mappedAddress = addressAttribute->getAddress(msg.header);
    candidatePair->onResponse(now, msg);
    if (candidatePair->state == CandidatePair::Failed)
    {
        return;
    }

    logger::debug("response from %s, rtt %" PRIu64 "ms",
        _logId.c_str(),
        sender.toString().c_str(),
        candidatePair->minRtt / utils::Time::ms);

    if (candidatePair->localCandidate.address != mappedAddress)
    {
        if (candidatePair->gatheringProbe)
        {
            addLocalCandidate(IceCandidate(_component,
                endpoint->getTransportType(),
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::SRFLX,
                    candidatePair->localEndpoint.preference,
                    _component,
                    endpoint->getTransportType()),
                mappedAddress,
                candidatePair->localCandidate.baseAddress,
                IceCandidate::Type::SRFLX));
        }
        else
        {
            candidatePair->localCandidate.address = mappedAddress;
            candidatePair->localCandidate.type = IceCandidate::Type::PRFLX;
            candidatePair->localCandidate.priority =
                ice::IceCandidate::computeCandidatePriority(IceCandidate::Type::PRFLX,
                    candidatePair->localEndpoint.preference,
                    _component,
                    endpoint->getTransportType());
            addLocalCandidate(candidatePair->localCandidate);
        }
    }
    // could add remote candidate if sender seems to be elsewhere, but unlikely

    stateCheck(now);
    if (_eventSink)
    {
        _eventSink->onIcePreliminary(this, endpoint, sender);
    }
}

void IceSession::onPacketReceived(IceEndpoint* socketEndpoint,
    const transport::SocketAddress& sender,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);

    if (!ice::isStunMessage(data, len))
    {
        return;
    }

    auto* msg = StunMessage::fromPtr(data);
    const auto method = msg->header.getMethod();
    if (method == StunHeader::BindingResponse || method == StunHeader::BindingErrorResponse)
    {
        if (!msg->isValid())
        {
            logger::debug("corrupt ICE response from %s", _logId.c_str(), sender.toString().c_str());
            return;
        }
        onResponseReceived(socketEndpoint, sender, *msg, timestamp);
    }
    else if (method == StunHeader::BindingRequest)
    {
        onRequestReceived(socketEndpoint, sender, *msg, timestamp);
    }
}

void IceSession::onTcpDisconnect(IceEndpoint* endpoint)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    auto candidatePairIt = std::find_if(_candidatePairs.begin(),
        _candidatePairs.end(),
        [endpoint](const std::unique_ptr<CandidatePair>& probe) { return probe->localEndpoint.endpoint == endpoint; });
    if (candidatePairIt != _candidatePairs.end())
    {
        candidatePairIt->get()->onDisconnect();
    }
}

void IceSession::sendResponse(IceEndpoint* endpoint,
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
    else if (!_credentials.remote.first.empty())
    {
        response.add(
            StunGenericAttribute(StunAttribute::USERNAME, _credentials.local.first + ":" + _credentials.remote.first));
    }

    response.addMessageIntegrity(_credentials.local.second);
    response.addFingerprint();
    endpoint->sendStunTo(target, response.header.transactionId.get(), &response, response.size(), timestamp);
}

bool IceSession::isAttached(const IceEndpoint* endpoint) const
{
    for (auto& endp : _endpoints)
    {
        if (endp.endpoint == endpoint)
        {
            return true;
        }
    }
    return false;
}

IceSession::CandidatePair* IceSession::findCandidatePair(const IceEndpoint* endpoint,
    const StunMessage& msg,
    const transport::SocketAddress& responder)
{
    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->localEndpoint.endpoint == endpoint &&
            (endpoint->getTransportType() == TransportType::TCP ||
                candidatePair->remoteCandidate.address == responder) &&
            candidatePair->hasTransaction(msg))
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
    uint64_t oldestSrflxCandidate = 0;
    for (auto& candidatePair : _candidatePairs)
    {
        if (candidatePair->gatheringProbe && candidatePair->state == CandidatePair::Succeeded &&
            candidatePair->remoteCandidate.type == IceCandidate::Type::RELAY)
        {
            oldestSrflxCandidate = std::max(oldestSrflxCandidate, now - candidatePair->startTime);
        }
    }
    return oldestSrflxCandidate;
}

bool IceSession::isIceComplete(const uint64_t now)
{
    size_t failCount = 0;
    int64_t probeAge = 60 * utils::Time::sec;
    for (auto candidatePair : _checklist)
    {
        if (candidatePair->state == CandidatePair::Succeeded && candidatePair->nominated)
        {
            return true;
        }
        else if (candidatePair->state == CandidatePair::Failed)
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
    if (now - _sessionStart > _config.connectTimeout * utils::Time::ms)
    {
        return true;
    }

    if (failCount > 0 && failCount == _checklist.size())
    {
        return true;
    }
    return false;
}

bool IceSession::hasNomination() const
{
    for (auto ct : _checklist)
    {
        if (ct->state == CandidatePair::Succeeded && ct->nominated)
        {
            return true;
        }
    }
    return false;
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
        if (!nominee && ct->state == CandidatePair::Succeeded)
        {
            // debateable if we should latch first detected UDP despite others having higher prio
            if (ct->remoteCandidate.type == IceCandidate::Type::HOST &&
                ct->localCandidate.transportType == TransportType::UDP)
            {
                nominee = ct;
            }
            else if (!hasTcpServerEndpoints)
            {
                nominee = ct;
            }
            else if (now - ct->startTime > _config.additionalCandidateTimeout * utils::Time::ms)
            {
                nominee = ct;
            }
        }
    }

    if (nominee)
    {
        nominee->nominated = true;
        nominee->original.add(StunAttribute(StunAttribute::USE_CANDIDATE));
        logger::debug("nominated %s-%s from %zu pending candidates",
            _logId.c_str(),
            nominee->localCandidate.baseAddress.toString().c_str(),
            nominee->remoteCandidate.address.toString().c_str(),
            _checklist.size());
        nominee->restartProbe(now);
        return;
    }
}

void IceSession::stateCheck(const uint64_t now)
{
    if (_state == State::GATHERING && isGatherComplete(now))
    {
        for (auto& candidatePair : _candidatePairs)
        {
            if (candidatePair->state == CandidatePair::InProgress)
            {
                candidatePair->state = CandidatePair::Failed; // timeout
            }
        }
        reportState(State::READY);
    }
    else if (_state == State::CONNECTING)
    {
        nominate(now);
        if (isIceComplete(now))
        {
            _state = (hasNomination() ? State::CONNECTED : State::FAILED);
            freezePendingProbes();
            if (_eventSink)
            {
                _eventSink->onIceStateChanged(this, _state);
                _eventSink->onIceCompleted(this);
            }
        }
    }
    else if (_state == State::CONNECTED)
    {
        freezePendingProbes();
    }
}

void IceSession::freezePendingProbes()
{
    for (auto& candidatePair : _candidatePairs)
    {
        if (!candidatePair->nominated && candidatePair->state < CandidatePair::State::Failed)
        {
            candidatePair->state = CandidatePair::State::Frozen;
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
    if (_state == State::CONNECTED)
    {
        for (auto& candidatePair : _candidatePairs)
        {
            if (candidatePair->nominated)
            {
                candidatePair->processTimeout(now);
                break;
            }
        }
        // TODO cleanup failed candidates to release memory
    }
    else
    {
        for (auto& candidatePair : _candidatePairs)
        {
            candidatePair->processTimeout(now);
        }
    }

    stateCheck(now);
    // TODO cleanup failed pairs

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
}

void IceSession::setRemoteCredentials(const std::string& ufrag, const std::string& pwd)
{
    _credentials.remote = std::make_pair(ufrag, pwd);
}

void IceSession::setRemoteCredentials(const std::pair<std::string, std::string>& credentials)
{
    _credentials.remote = credentials;
};

// targetBuffer must be length + 1 for null termination
void IceSession::generateCredentialString(StunTransactionIdGenerator& idGenerator, char* targetBuffer, int length)
{
    const char* approvedLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "0123456789"
                                  "+/";
    const int COUNT = std::strlen(approvedLetters);
    __uint64_t id = 0;
    for (int i = 0; i < length; i++)
    {
        if (i % 10 == 0)
        {
            id = idGenerator.next();
        }
        else
        {
            id >>= 6;
        }

        targetBuffer[i] = approvedLetters[(id & 0x3Fu) % COUNT];
    }
    targetBuffer[length] = '\0';
}

IceSession::CandidatePair::CandidatePair(const IceConfig& config,
    const EndpointInfo& endpoint,
    const IceCandidate& local,
    const IceCandidate& remote,
    StunTransactionIdGenerator& idGenerator,
    const SessionCredentials& credentials,
    const std::string& name,
    bool gathering)
    : localCandidate(local),
      remoteCandidate(remote),
      localEndpoint(endpoint),
      gatheringProbe(gathering),
      startTime(0),
      nextTransmission(0),
      transmitInterval(config.RTO),
      replies(0),
      nominated(false),
      errorCode(IceError::Success),
      minRtt(utils::Time::minute),
      state(Waiting),
      _name(name),
      _idGenerator(idGenerator),
      _config(config),
      _credentials(credentials)
{
}

uint64_t IceSession::CandidatePair::getPriority(IceRole role) const
{
    const auto diff = ((role == IceRole::CONTROLLED) ^ (remoteCandidate.priority > localCandidate.priority)) ? 1 : 0;
    auto compoundPriority = (static_cast<uint64_t>(std::min(localCandidate.priority, remoteCandidate.priority)) << 32) +
        std::max(localCandidate.priority, remoteCandidate.priority) * 2 + diff;

    return compoundPriority;
}

// returns -1 when no more timeouts are needed
int64_t IceSession::CandidatePair::nextTimeout(uint64_t now) const
{
    if (state == State::Failed || state == State::Frozen)
    {
        return -1;
    }

    if (state == Succeeded && replies > 10 && !nominated)
    {
        return -1;
    }
    if (state == Succeeded && gatheringProbe)
    {
        return -1;
    }

    int64_t timeout = nextTransmission - now;
    if (timeout > 0)
    {
        return timeout;
    }
    else
    {
        return 0;
    }
}

void IceSession::CandidatePair::restartProbe(const uint64_t now)
{
    _transactions.clear();
    state = InProgress;
    send(now);
}

void IceSession::CandidatePair::send(const uint64_t now)
{
    if (!gatheringProbe && _credentials.remote.second.empty())
    {
        // cannot send yet as we have not receive credentials
        nextTransmission = now + 30 * utils::Time::ms;
        return;
    }

    if (_transactions.empty())
    {
        transmitInterval =
            (localCandidate.transportType == ice::TransportType::UDP ? _config.RTO : _config.keepAliveInterval) *
            utils::Time::ms;
        nextTransmission = now + transmitInterval;
        startTime = now;
    }
    else if (state == Succeeded && replies > 10 && nominated)
    {
        transmitInterval = _config.keepAliveInterval * utils::Time::ms;
        if (utils::Time::diffGE(nextTransmission, now, transmitInterval * 2))
        {
            nextTransmission = now;
        }
        else
        {
            nextTransmission = nextTransmission + transmitInterval;
        }
    }
    else
    {
        if (localCandidate.transportType == ice::TransportType::UDP)
        {
            transmitInterval = std::min(transmitInterval * 2, _config.maxRTO * utils::Time::ms);
            const auto maxRttTransaction = std::max_element(_transactions.cbegin(),
                _transactions.cend(),
                [](const StunTransaction& cta, const StunTransaction& ctb) { return cta.rtt > ctb.rtt; });
            transmitInterval =
                std::max(transmitInterval, std::min(_config.maxRTO * utils::Time::ms, maxRttTransaction->rtt));
        }
        nextTransmission = nextTransmission + transmitInterval;
    }

    StunTransaction transaction;
    auto stunMessage = original;
    stunMessage.header.transactionId.set(_idGenerator.next());
    transaction.id = stunMessage.header.transactionId;
    stunMessage.add(
        StunGenericAttribute(StunAttribute::USERNAME, _credentials.remote.first + ":" + _credentials.local.first));
    stunMessage.add(StunAttribute64(_credentials.role == IceRole::CONTROLLING ? StunAttribute::ICE_CONTROLLING
                                                                              : StunAttribute::ICE_CONTROLLED,
        _credentials.tieBreaker));

    if (!gatheringProbe)
    {
        stunMessage.addMessageIntegrity(_credentials.remote.second);
        stunMessage.addFingerprint();
    }

    transaction.time = now;
    if (state == Waiting)
    {
        state = InProgress;
    }
    _transactions.push_back(transaction);
    if (_transactions.size() > 8)
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

    for (uint32_t i = 0; i < (state != Succeeded ? _config.probeReplicates : 1); ++i)
    {
        logger::debug("%s probing %s",
            _name.c_str(),
            localEndpoint.endpoint->getLocalPort().toString().c_str(),
            remoteCandidate.address.toString().c_str());

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

    if (_transactions.size() == 1 && localEndpoint.endpoint->getTransportType() == ice::TransportType::TCP)
    {
        localCandidate.baseAddress = localEndpoint.endpoint->getLocalPort();
        if (localCandidate.address.getPort() == 0)
        {
            localCandidate.address.setPort(localCandidate.baseAddress.getPort());
        }
    }

    transaction->rtt = now - transaction->time;
    minRtt = std::min(transaction->rtt, minRtt);

    auto errorAttribute = response.getAttribute<StunError>(StunAttribute::ERROR_CODE);
    if (errorAttribute)
    {
        if (errorAttribute->getCode() == StunError::Code::RoleConflict)
        {
            state = CandidatePair::Waiting;
            nominated = false;
            nextTransmission = now;
            replies = 0;
        }
        else
        {
            failCandidate();
            state = CandidatePair::Failed;
            errorCode = static_cast<IceError>(errorAttribute->getCode());
            ++replies;
        }
        return;
    }

    if (++replies > 0)
    {
        state = CandidatePair::Succeeded;
    }
}

void IceSession::CandidatePair::onDisconnect()
{
    failCandidate();
    errorCode = IceError::ConnectionTimeoutOrFailure;
    return;
}

void IceSession::CandidatePair::nominate(uint64_t now)
{
    nominated = true;
    state = State::Succeeded;
    nextTransmission = now;
}

void IceSession::CandidatePair::freeze()
{
    state = State::Frozen;
    cancelPendingTransactions();
}

void IceSession::CandidatePair::failCandidate()
{
    state = State::Failed;
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

bool IceSession::CandidatePair::isRecent(uint64_t now) const
{
    if (gatheringProbe && state == InProgress)
    {
        return true;
    }

    return state == InProgress && utils::Time::diffLT(startTime, now, _config.hostProbeTimeout * utils::Time::ms / 2);
}

void IceSession::CandidatePair::processTimeout(const uint64_t now)
{
    if (state == Succeeded && gatheringProbe)
    {
        return;
    }
    if (gatheringProbe && state == InProgress && now - startTime > _config.gather.probeTimeout * utils::Time::ms)
    {
        failCandidate();
        errorCode = IceError::RequestTimeout;
        return;
    }

    if (state == Failed)
    {
        return;
    }

    if (nextTimeout(now) == 0)
    {
        send(now);
    }
    if (state == InProgress)
    {
        if (remoteCandidate.type == IceCandidate::Type::HOST &&
            now - startTime > _config.hostProbeTimeout * utils::Time::ms)
        {
            failCandidate();
        }
        else if (now - startTime > _config.reflexiveProbeTimeout * utils::Time::ms)
        {
            failCandidate();
        }
    }
}

// ns
uint64_t IceSession::CandidatePair::getRtt() const
{
    uint64_t rtt = utils::Time::sec * 20;
    for (auto& transaction : _transactions)
    {
        if (transaction.rtt > 0)
        {
            rtt = std::min(rtt, transaction.rtt);
        }
    }

    return rtt;
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

std::string IceSession::CandidatePair::getLoggableId() const
{
    return localCandidate.address.toString() + "-" + remoteCandidate.address.toString();
}

void IceSession::stop()
{
    reportState(State::IDLE);
}

void IceSession::reportState(State newState)
{
    auto oldState = _state.exchange(newState, std::memory_order::memory_order_relaxed);
    if (_eventSink && oldState != newState)
    {
        _eventSink->onIceStateChanged(this, _state);
    }
}
} // namespace ice
