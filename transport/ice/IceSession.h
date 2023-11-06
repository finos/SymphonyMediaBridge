#pragma once
#include "IceCandidate.h"
#include "Stun.h"
#include "concurrency/ScopedMutexGuard.h"
#include "crypto/SslHelper.h"
#include "memory/Map.h"
#include "utils/SocketAddress.h"
#include <deque>

namespace ice
{

// time unit ms
struct IceConfig
{
    struct
    {
        uint32_t probeTimeout = 10000;
        uint32_t additionalServerTimeout = 2000;
        std::vector<transport::SocketAddress> stunServers;
    } gather;
    uint32_t probeReleasePace = 20;
    uint32_t keepAliveInterval = 10000;
    uint32_t reflexiveProbeTimeout = 15000; // probing towards reflexive
    uint32_t hostProbeTimeout = 5000; // probing towards host address
    uint32_t additionalCandidateTimeout = 2000; // before nominating TCP
    uint32_t connectTimeout = 30000;
    uint32_t RTO = 50; // initial RTO. RFC8489 says 500ms. This is faster in case of loss.
    uint32_t maxRTO = 1000;
    uint32_t probeReplicates = 1;
    uint32_t probeConnectionExpirationTimeout = 5000;
    uint32_t transmitIntervalExtend = 10;
    uint32_t maxCandidateCount = 15;

    std::string software = "slice"; // keep short please.
    transport::SocketAddress publicIpv4;
    transport::SocketAddress publicIpv6;
};
// Abstraction for IceSession to send over a socket
// STUN transaction id is included which makes it easy for you to
// to route the response back to this session if needed.
class IceEndpoint
{
public:
    virtual void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) = 0;

    virtual ice::TransportType getTransportType() const = 0;
    virtual transport::SocketAddress getLocalPort() const = 0;
    virtual void cancelStunTransaction(__uint128_t transactionId) = 0;
};

enum class IceRole
{
    CONTROLLING,
    CONTROLLED
};

enum class IceError
{
    Success = 0,
    RequestTimeout = 408,
    TryAlternate = 300,
    BadRequest = 400,
    Unauthenticated = 401,
    Forbidden = 403,
    MobilityForbidden = 405,
    UnknownAttribute = 420,
    AllocationMismatch = 437,
    StaleNonce = 438,
    AddressFamilyNotSupported = 440,
    WrongCredentials = 441,
    UnsupportedTransportProtocol = 442,
    PeerAddressFamilyMismatch = 443,
    ConnectionAlreadyExists = 446,
    ConnectionTimeoutOrFailure = 447,
    AllocationQuotaReached = 486,
    RoleConflict = 487,
    ServerError = 500,
    InsufficientCapacity = 508,
    FailedCreateTcpEndpoint = 515
};

struct CandidatePairKey
{
    const IceEndpoint* endPoint;
    transport::SocketAddress remoteAddress;
};

typedef std::vector<IceCandidate> IceCandidates;
// Establishes connectivity over one or more sockets
// You will need one IceSession per ice component
// You drive the session by calling onStunPacketReceived and processTimeout
// It is not thread safe
class IceSession
{
public:
    enum class State
    {
        IDLE,
        GATHERING,
        READY,
        CONNECTING,
        CONNECTED,
        FAILED,
        LAST
    };

    class IEvents
    {
    public:
        virtual void onIceStateChanged(IceSession* session, State state) = 0;
        virtual void onIceCompleted(IceSession* session) = 0;
        virtual void onIceCandidateChanged(IceSession* session,
            IceEndpoint* localEndpoint,
            const transport::SocketAddress& remotePort) = 0;
        virtual void onIceDiscardCandidate(IceSession* session,
            IceEndpoint* localEndpoint,
            const transport::SocketAddress& remotePort) = 0;
    };

    IceSession(const IceSession&) = delete;
    IceSession(size_t sessionId,
        const IceConfig& config,
        ice::IceComponent component,
        ice::IceRole role,
        IEvents* eventSink = nullptr);

    void attachLocalEndpoint(IceEndpoint* udpEndpoint);

    bool isAttached(const IceEndpoint* localEndpoint) const;
    void gatherLocalCandidates(const std::vector<transport::SocketAddress>& stunServers, uint64_t timestamp);
    const std::pair<std::string, std::string>& getLocalCredentials() const;
    void setLocalCredentials(const std::pair<std::string, std::string>& credentials);
    IceCandidates getLocalCandidates() const;
    void addLocalCandidate(const IceCandidate& candidate);
    void addLocalCandidate(const transport::SocketAddress& publicAddress, IceEndpoint* udpEndpoint);

    void addLocalTcpCandidate(IceCandidate::Type type,
        int interfaceIndex,
        const transport::SocketAddress& baseAddress,
        const transport::SocketAddress& publicAddress,
        TcpType tcpType);

    const IceCandidate& addRemoteCandidate(const IceCandidate& udpCandidate);
    void addRemoteCandidate(const IceCandidate& tcpCandidate, IceEndpoint* tcpEndpoint);

    void setRemoteCredentials(const std::string& ufrag, const std::string& pwd);
    void setRemoteCredentials(const std::pair<std::string, std::string>& credentials);

    void probeRemoteCandidates(IceRole role, uint64_t timestamp);
    IceCandidates getRemoteCandidates() const { return _remoteCandidates; }
    static void generateCredentialString(StunTransactionIdGenerator& idGenerator, char* targetBuffer, int length);

    std::pair<IceCandidate, IceCandidate> getSelectedPair() const;
    uint64_t getSelectedPairRtt() const;

    void onPacketReceived(IceEndpoint* localEndpoint, const transport::SocketAddress& remotePort, uint64_t timestamp);

    void onStunPacketReceived(IceEndpoint* localEndpoint,
        const transport::SocketAddress& remotePort,
        const void* data,
        size_t len,
        uint64_t timestamp);

    void onTcpDisconnect(IceEndpoint* localEndpoint);
    void onTcpRemoved(const IceEndpoint* localEndpoint);

    bool isRequestAuthentic(const void* data, size_t len) const;
    bool isResponseAuthentic(const void* data, size_t len) const;

    int64_t nextTimeout(uint64_t timestamp) const;
    int64_t processTimeout(uint64_t timestamp);

    State getState() const { return _state.load(); }
    IceRole getRole() const { return _credentials.role; }

    void stop();

    bool isValidSource(uint64_t timestamp, const transport::SocketAddress& address) const;

private:
    class CandidatePair;
    struct SessionCredentials
    {
        SessionCredentials(ice::IceRole role_, uint64_t tieBreaker_) : role(role_), tieBreaker(tieBreaker_) {}

        std::pair<std::string, std::string> local;
        std::pair<std::string, std::string> remote;
        IceRole role;
        const uint64_t tieBreaker;
    };
    struct EndpointInfo
    {
        EndpointInfo(IceEndpoint* iceEndpoint, int preference_) : endpoint(iceEndpoint), preference(preference_) {}
        EndpointInfo(const EndpointInfo&) = default;

        IceEndpoint* endpoint;
        int preference;
    };

    struct StunTransaction
    {
        StunTransactionId id;
        uint64_t time = 0;
        uint64_t rtt = 0;

        bool acknowledged() const { return rtt != 0; }
    };

    class CandidatePair
    {
    public:
        CandidatePair(const IceConfig& config,
            const EndpointInfo& localEndpoint,
            const IceCandidate& local,
            const IceCandidate& remote,
            StunTransactionIdGenerator& idGenerator,
            const SessionCredentials& credentials,
            crypto::HMAC& hmacComputerRemote,
            const std::string& name,
            bool gathering);

        CandidatePair& operator=(const CandidatePair&) = delete;

        int64_t nextTimeout(uint64_t now) const;
        void processTimeout(uint64_t now);

        void restartProbe(const uint64_t now);
        void send(uint64_t now);
        uint64_t getPriority(IceRole role) const;
        bool isFinished() const { return state == Succeeded || state == Failed; }

        bool hasTransaction(const StunMessage& response) const;
        StunTransaction* findTransaction(const StunMessage& response);
        void onResponse(uint64_t now, const StunMessage& response);
        void onDisconnect();
        void nominate(uint64_t now);
        void freeze();
        void failCandidate(IceError reason);

        IceError getReason() const { return _errorCode; }

        // ns
        uint64_t getRtt() const { return _minRtt; }

        IceCandidate localCandidate;
        const IceCandidate remoteCandidate;
        EndpointInfo localEndpoint;
        const bool gatheringProbe;
        uint64_t startTime;
        uint64_t nextTransmission;
        bool nominated;

        uint64_t receptionTimestamp; // RTC+ICE traffic

        enum State
        {
            Waiting,
            InProgress,
            Succeeded,
            Failed,
            Frozen
        } state;

        StunMessage original;

    private:
        void cancelPendingTransactions();
        void cancelPendingTransactionsBefore(StunTransaction& transaction);

        uint64_t _transmitInterval;
        int _replies;
        IceError _errorCode;
        uint64_t _minRtt;
        const std::string& _name;
        std::deque<StunTransaction> _transactions; // TODO replace with inplace circular container
        StunTransactionIdGenerator& _idGenerator;
        const IceConfig& _config;
        const SessionCredentials& _credentials;
        crypto::HMAC& _hmacComputer;
    };

    CandidatePair* addRemoteTcpCandidate(const IceCandidate& tcpCandidate, IceEndpoint* tcpEndpoint);
    CandidatePair* addProbeForRemoteCandidate(EndpointInfo& localEndpoint, const IceCandidate& remoteCandidate);
    void sortCheckList();

    CandidatePair* findCandidatePair(const IceEndpoint* localEndpoint,
        const StunMessage& response,
        const transport::SocketAddress& remotePort);
    CandidatePair* findCandidatePair(const IceEndpoint* localEndpoint, const transport::SocketAddress& remotePort);

    void removeCandidatePair(const CandidatePair* candidatePair);
    void sendResponse(IceEndpoint* localEndpoint,
        const transport::SocketAddress& target,
        int code,
        const StunMessage& msg,
        uint64_t timestamp,
        const std::string& errorPhrase = "");

    bool isGatherComplete(uint64_t now);
    bool isIceComplete(uint64_t now);
    void stateCheck(uint64_t now);
    void nominate(uint64_t now);
    void freezePendingProbes(uint64_t now);
    void releaseFrozenProbes(uint64_t now);
    bool hasNomination() const;
    uint64_t getMaxStunServerCandidateAge(uint64_t now) const;
    IceCandidate::Type inferCandidateType(const transport::SocketAddress& mappedAddress);

    void onRequestReceived(IceEndpoint* localEndpoint,
        const transport::SocketAddress& remotePort,
        const StunMessage& data,
        uint64_t now);
    void onResponseReceived(IceEndpoint* localEndpoint,
        const transport::SocketAddress& remotePort,
        const StunMessage& msg,
        uint64_t now);

    void reportState(State newState);

    const std::string _logId;
    std::vector<std::unique_ptr<CandidatePair>> _candidatePairs;
    std::vector<CandidatePair*> _checklist;
    memory::Map<CandidatePairKey, CandidatePair*, 256> _candidatePairIndex; // after reception from remote
    CandidatePair* _nomination;
    CandidatePair* _preliminaryCandidate;

    const ice::IceComponent _component;

    std::vector<EndpointInfo> _endpoints;
    std::vector<transport::SocketAddress> _stunServers;
    IceCandidates _localCandidates;
    IceCandidates _remoteCandidates;
    uint32_t _tcpProbeCount;

    const IceConfig _config;
    std::atomic<State> _state;
    StunTransactionIdGenerator _idGenerator;
    IEvents* const _eventSink;
    SessionCredentials _credentials;
    uint64_t _sessionStart;
    struct HmacComputers
    {
        crypto::HMAC local;
        crypto::HMAC remote;
    };
    mutable HmacComputers _hmacComputer;

    DBGCHECK_SINGLETHREADED_MUTEX(_mutexGuard);
};

} // namespace ice

namespace std
{
template <>
class hash<ice::CandidatePairKey>
{
public:
    size_t operator()(const ice::CandidatePairKey& key) const
    {
        if (key.endPoint->getTransportType() == ice::TransportType::TCP)
        {
            return utils::FowlerNollVoHash(&key.endPoint, sizeof(key.endPoint));
        }

        CompactKey compactKey;
        compactKey.port = key.remoteAddress.getPort();
        compactKey.endPoint = key.endPoint;
        if (key.remoteAddress.getFamily() == AF_INET6)
        {
            memcpy(&compactKey.ip.v6, &key.remoteAddress.getIpv6()->sin6_addr, sizeof(compactKey.ip.v6));
        }
        else if (key.remoteAddress.getFamily() == AF_INET)
        {
            memcpy(&compactKey.ip.v4, &key.remoteAddress.getIpv4()->sin_addr, sizeof(compactKey.ip.v4));
        }
        else
        {
            assert(false);
        }

        return utils::FowlerNollVoHash(&compactKey, sizeof(compactKey));
    }

private:
    struct CompactKey
    {
        CompactKey() { memset(this, 0, sizeof(CompactKey)); }
        uint16_t port;
        union
        {
            in_addr v4;
            in6_addr v6;
        } ip;

        const ice::IceEndpoint* endPoint;
    };
};

} // namespace std
