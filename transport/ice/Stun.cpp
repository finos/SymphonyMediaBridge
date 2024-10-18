#include "Stun.h"
#include "crypto/SslHelper.h"
#include <cassert>
#include <cstring>
#include <openssl/rand.h>
namespace
{
crypto::Crc32Polynomial crcPolynomial(0x04C11DB7);
}
namespace ice
{

void RAND_nonce(uint8_t* dst, int len)
{
    RAND_bytes(dst, len);
}

StunTransactionIdGenerator::StunTransactionIdGenerator()
    : _generator(reinterpret_cast<std::mt19937_64::result_type>(
          (uint64_t(std::random_device()()) << 32) + std::random_device()())),
      _distribution(0, std::numeric_limits<uint64_t>::max())
{
}

Int96 StunTransactionIdGenerator::next()
{
    const uint32_t value0 = _distribution(_generator);
    const uint32_t value1 = _distribution(_generator);
    const uint32_t value2 = _distribution(_generator);
    return Int96{value0, value1, value2};
}

StunTransactionId::StunTransactionId() : w2(0), w1(0), w0(0) {}

void StunTransactionId::set(Int96 id)
{
    w0 = id.w0;
    w1 = id.w1;
    w2 = id.w2;
}

StunHeader::StunHeader() : method(BindingRequest), length(0), magicCookie(MAGIC_COOKIE) {}

size_t StunHeader::size() const
{
    return length + 20;
}

bool StunHeader::isValid() const
{
    const uint16_t headerMethod = method;
    if (magicCookie != MAGIC_COOKIE || length % 4 != 0 || (headerMethod & 0xC000u) != 0)
    {
        return false;
    }

    const auto stunMethod = getMethod();
    if (stunMethod != BindingRequest && stunMethod != SharedSecretRequest && stunMethod != BindingResponse &&
        stunMethod != SharedSecretResponse && stunMethod != BindingErrorResponse &&
        stunMethod != SharedSecretErrorResponse)
    {
        return false;
    }

    return true;
}

StunHeader::StunClass StunHeader::getClass() const
{
    const uint16_t stunMethod = this->method;
    return static_cast<StunClass>(stunMethod & 0x110u);
}

StunHeader::Method StunHeader::getMethod() const
{
    const uint16_t stunMethod = this->method;
    return static_cast<Method>(stunMethod & 0x3FFFu);
}

void StunHeader::setMethod(Method method)
{
    this->method = method & 0x3FFFu;
}

const StunHeader* StunHeader::fromPtr(const void* p)
{
    return reinterpret_cast<const StunHeader*>(p);
}

// Secure validation that this message has valid CRC and attributes can be traversed
// It does not validate content of attributes.
bool isStunMessage(const void* pkt, size_t length)
{
    if (length < sizeof(StunHeader))
    {
        return false;
    }

    auto msg = StunMessage::fromPtr(pkt);
    if (msg->header.length + sizeof(StunHeader) != length || msg->header.length > sizeof(msg->attributes) ||
        !msg->header.isValid())
    {
        return false;
    }

    const uint8_t* endPointer = reinterpret_cast<const uint8_t*>(pkt) + msg->size();
    for (auto it = msg->cbegin(); it != msg->cend(); ++it)
    {
        if (it->get() >= endPointer)
        {
            return false;
        }
        if (it->type == StunAttribute::FINGERPRINT)
        {
            if (msg->computeFingerprint() != reinterpret_cast<const StunFingerprint&>(*it).value)
            {
                return false;
            }
        }
    }
    return true;
}

bool isRequest(const void* p)
{
    auto* msg = StunHeader::fromPtr(p);
    return msg->isRequest();
}

bool isResponse(const void* p)
{
    auto* msg = StunHeader::fromPtr(p);
    return msg->isResponse();
}

ice::Int96 getStunTransactionId(const void* data, size_t length)
{
    auto msg = StunMessage::fromPtr(data);
    if (!msg->header.isValid() || msg->header.length + sizeof(StunHeader) != length)
    {
        return Int96();
    }

    return msg->header.transactionId.get();
}

StunMessage::StunMessage()
{
    attributes[0] = 0;
}

StunMessage& StunMessage::operator=(const StunMessage& b)
{
    std::memcpy(this, &b, std::min(b.size(), sizeof(StunMessage)));
    return *this;
}

StunMessage& StunMessage::add(const StunAttribute& attr)
{
    std::memcpy(reinterpret_cast<uint8_t*>(&header) + sizeof(header) + header.length, &attr, attr.size());
    header.length = header.length + attr.size();
    return *this;
}

StunMessage::iterator StunMessage::begin()
{
    return iterator(reinterpret_cast<StunAttribute*>(&attributes));
}

StunMessage::const_iterator StunMessage::cbegin() const
{
    return const_iterator(reinterpret_cast<const StunAttribute*>(&attributes));
}

StunMessage::iterator StunMessage::end()
{
    return iterator(const_cast<StunAttribute*>(&(*cend())));
}

StunMessage::const_iterator StunMessage::cend() const
{
    if (header.length > sizeof(attributes))
    {
        return const_iterator(reinterpret_cast<const StunAttribute*>(&attributes)); // invalid message
    }
    return const_iterator(
        reinterpret_cast<const StunAttribute*>(reinterpret_cast<const uint8_t*>(&attributes) + header.length));
}

const StunAttribute* StunMessage::getAttribute(uint16_t type) const
{
    for (auto& a : *this)
    {
        if (a.type == type)
        {
            return &a;
        }
    }
    return nullptr;
}

uint32_t StunMessage::computeFingerprint() const
{
    // polynomial 04 C1 1D B7
    crypto::Crc32 crc(crcPolynomial);
    const uint32_t rfc5389xor = 0x5354554e; // https://tools.ietf.org/html/rfc5389

    auto start = reinterpret_cast<const uint8_t*>(this);
    assert(attributes - start == 20);
    for (auto it = cbegin(); it != cend(); ++it)
    {
        if (it->type == StunAttribute::FINGERPRINT)
        {
            crc.add(start, header.size() - it->size());
            return crc.compute() ^ rfc5389xor;
        }
    }

    crc.add(start, 2);
    const nwuint16_t newLength(header.length.get() + static_cast<uint16_t>(sizeof(StunAttribute) + sizeof(uint32_t)));
    crc.add(&newLength, sizeof(newLength));
    crc.add(start + 2 * sizeof(uint16_t), header.size() - 2 * sizeof(uint16_t));
    return crc.compute() ^ rfc5389xor;
}

// Computes hmac with present or absent INTEGRITY-MESSAGE.
// NOT COMPATIBLE WITH rfc3489bis-06
// If you add any attributes before the MESSAGE-INTEGRITY
// attribute, you must recompute HMAC.
// pwd is a=ice-pwd: from SDP
void StunMessage::computeHMAC(crypto::HMAC& hmacComputer, uint8_t* hmac20b) const
{
    hmacComputer.reset();
    const auto start = reinterpret_cast<const uint8_t*>(this);

    // if there is no message-integrity attribute yet, we assume that is going to be the next one added
    // fake length will thus include the not yet added integrity-attribute
    int digestableLength = header.length + sizeof(StunHeader);

    for (auto it = cbegin(); it != cend(); ++it)
    {
        if (it->type == StunAttribute::MESSAGE_INTEGRITY)
        {
            digestableLength = reinterpret_cast<const uint8_t*>(&(*it)) - start;
            break;
        }
    }

    const nwuint16_t fakeLength(digestableLength - sizeof(StunHeader) + StunMessageIntegrity::size());
    hmacComputer.add(start, 2);
    hmacComputer.add(&fakeLength, 2);
    hmacComputer.add(start + 4, digestableLength - 4);
    hmacComputer.compute(hmac20b);
}

void StunMessage::addMessageIntegrity(crypto::HMAC& hmacComputer)
{
    StunMessageIntegrity attribute;
    uint8_t hmac[20];
    computeHMAC(hmacComputer, hmac);
    attribute.setHmac(hmac);
    add(attribute);
}

// Verifies that all known attributes are well formed and that fingerprint is valid
bool StunMessage::isValid() const
{
    for (auto& attribute : *this)
    {
        if (&attribute >= &*cend())
        {
            return false;
        }
        else if (attribute.type == StunAttribute::FINGERPRINT)
        {
            if (computeFingerprint() != reinterpret_cast<const StunFingerprint&>(attribute).value)
            {
                return false;
            }
        }
        else if ((attribute.type == StunAttribute::ICE_CONTROLLED ||
                     attribute.type == StunAttribute::ICE_CONTROLLING) &&
            attribute.length != 8)
        {
            return false;
        }
        else if (attribute.type == StunAttribute::PRIORITY && attribute.length != 4)
        {
            return false;
        }
        else if (attribute.type == StunAttribute::MESSAGE_INTEGRITY && attribute.length != 20)
        {
            return false;
        }
        else if (attribute.type == StunAttribute::XOR_MAPPED_ADDRESS &&
            !reinterpret_cast<const StunXorMappedAddress&>(attribute).isValid())
        {
            return false;
        }
        else if (attribute.type == StunAttribute::MAPPED_ADDRESS &&
            !reinterpret_cast<const StunMappedAddress&>(attribute).isValid())
        {
            return false;
        }
        else if (attribute.type == StunAttribute::ERROR_CODE &&
            !reinterpret_cast<const StunError&>(attribute).isValid())
        {
            return false;
        }
        else if (attribute.type == StunAttribute::USE_CANDIDATE && attribute.length != 0)
        {
            return false;
        }
    }
    return true;
}

bool StunMessage::isAuthentic(crypto::HMAC& hmacComputer) const
{
    for (auto& attribute : *this)
    {
        if (attribute.type == StunAttribute::MESSAGE_INTEGRITY)
        {
            if (attribute.length != 20)
            {
                return false;
            }
            uint8_t hmac[20];
            computeHMAC(hmacComputer, hmac);
            return reinterpret_cast<const StunMessageIntegrity&>(attribute).isMatch(hmac);
        }
    }
    return false;
}

void StunMessage::addFingerprint()
{
    add(StunFingerprint(computeFingerprint()));
}

const StunMessage* StunMessage::fromPtr(const void* ptr)
{
    assert((intptr_t)ptr % alignof(StunMessage) == 0);
    return reinterpret_cast<const StunMessage*>(ptr);
}

const uint8_t* StunAttribute::get() const
{
    return reinterpret_cast<const uint8_t*>(this + 1);
}

size_t StunAttribute::size() const
{
    if (length % 4 > 0)
    {
        return length + sizeof(StunAttribute) + 4 - length % 4;
    }
    return length + sizeof(StunAttribute);
}

uint64_t StunAttribute64::get() const
{
    uint64_t result = value1.get();
    result <<= 32;
    return result + value0.get();
}

void StunAttribute64::set(const uint64_t v)
{
    value1 = v >> 32;
    value0 = v & 0xFFFFFFFFu;
}

bool StunXorMappedAddress::isValid() const
{
    const size_t baseSize = sizeof(_port) + sizeof(_family);
    if (_family == Family_v4)
    {
        return length == sizeof(_address.v4) + baseSize;
    }
    else if (_family == Family_v6)
    {
        return length == sizeof(_address.v6) + baseSize;
    }
    return false;
}

void StunXorMappedAddress::setAddress(const transport::SocketAddress& ipAddress, const StunHeader& header)
{
    _port = ipAddress.getPort() ^ (StunHeader::MAGIC_COOKIE >> 16);
    if (ipAddress.getFamily() == AF_INET)
    {
        _family = Family_v4;
        _address.v4 = ipAddress.getIpv4()->sin_addr.s_addr ^ htonl(StunHeader::MAGIC_COOKIE);
        length = 4 + 4;
    }
    else
    {
        _family = Family_v6;

        assert((uint32_t*)&header.magicCookie + 1 == (uint32_t*)&header.transactionId);
        auto mask = reinterpret_cast<const uint8_t*>(&header.magicCookie);
        auto v6Addr = reinterpret_cast<const uint8_t*>(&ipAddress.getIpv6()->sin6_addr);
        for (int i = 0; i < 16; ++i)
        {
            _address.v6[i] = v6Addr[i] ^ mask[i];
        }
        length = 4 + 16;
    }
}

transport::SocketAddress StunXorMappedAddress::getAddress(const StunHeader& header) const
{
    if (_family == Family_v4)
    {
        return transport::SocketAddress(ntohl(_address.v4) ^ StunHeader::MAGIC_COOKIE,
            _port ^ (StunHeader::MAGIC_COOKIE >> 16));
    }
    else
    {
        uint8_t v6[16];
        auto mask = reinterpret_cast<const uint8_t*>(&header.magicCookie);
        for (int i = 0; i < 16; ++i)
        {
            v6[i] = _address.v6[i] ^ mask[i];
        }
        return transport::SocketAddress(v6, _port ^ (StunHeader::MAGIC_COOKIE >> 16));
    }
}

void StunMappedAddress::setAddress(const transport::SocketAddress& ipAddress)
{
    _port = ipAddress.getPort();
    if (ipAddress.getFamily() == AF_INET)
    {
        _family = Family_v4;
        _address.v4 = ipAddress.getIpv4()->sin_addr.s_addr;
        length = 4 + 4;
    }
    else
    {
        _family = Family_v6;
        std::memcpy(_address.v6, &ipAddress.getIpv6()->sin6_addr, 16);
        length = 4 + 16;
    }
}

transport::SocketAddress StunMappedAddress::getAddress() const
{
    if (_family == Family_v4)
    {
        return transport::SocketAddress(ntohl(_address.v4), _port);
    }
    else
    {
        return transport::SocketAddress(_address.v6, _port);
    }
}

bool StunMappedAddress::isValid() const
{
    const size_t baseSize = sizeof(_port) + sizeof(_family);
    if (_family == Family_v4)
    {
        return length == sizeof(_address.v4) + baseSize;
    }
    else if (_family == Family_v6)
    {
        return length == sizeof(_address.v6) + baseSize;
    }
    return false;
}

void StunGenericAttribute::setValue(const std::string& nValue)
{
    setValue(reinterpret_cast<const uint8_t*>(nValue.c_str()), nValue.size());
}

void StunGenericAttribute::setValue(const uint8_t* data, int _length)
{
    std::memcpy(&value, data, _length);
    if (_length % 4 > 0)
    {
        const int padding = 4 - _length % 4;
        std::memset(&value[_length], 0, padding);
    }
    length = _length;
}

std::string StunGenericAttribute::getUtf8() const
{
    const uint16_t size = length;
    return std::string(reinterpret_cast<const char*>(value), 0, size);
}

void StunMessageIntegrity::getHmac(uint8_t data[20])
{
    assert(length == 20);
    std::memcpy(data, value, 20);
}

bool StunMessageIntegrity::isMatch(uint8_t data[20]) const
{
    return 0 == std::memcmp(data, value, 20);
}

void StunMessageIntegrity::setHmac(const uint8_t* data)
{
    std::memcpy(value, data, 20);
}

bool StunUserName::isTargetUser(const char* name) const
{
    auto pos = std::find(value, value + length, ':');
    if (pos != value + length && static_cast<size_t>(pos - value) == std::strlen(name))
    {
        return 0 == std::strncmp(name, reinterpret_cast<const char*>(value), pos - value);
    }
    return false;
}

std::pair<std::string, std::string> StunUserName::getNames() const
{
    const auto userName = getUtf8();
    const auto pos = userName.find(':');
    if (pos == std::string::npos)
    {
        return std::pair<std::string, std::string>();
    }

    return std::pair<std::string, std::string>(userName.substr(0, pos), userName.substr(pos + 1));
}

bool StunError::isValid() const
{
    if (length < sizeof(_code))
    {
        return false;
    }
    return true;
}
} // namespace ice
