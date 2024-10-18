#pragma once

#include "utils/ByteOrder.h"
#include "utils/SocketAddress.h"
#include "utils/TlvIterator.h"
#include <codecvt>
#include <random>
#include <type_traits>

namespace crypto
{
class HMAC;
}

namespace ice
{
const size_t MTU = 1280; // rfc states 576B for ipv4 and 1280 for ipv6

struct Int96
{
    uint32_t w2 = 0;
    uint32_t w1 = 0;
    uint32_t w0 = 0;

    bool operator==(const Int96& s) const { return w0 == s.w0 && w1 == s.w1 && w2 == s.w2; }
    bool operator!=(const Int96& s) const { return w0 != s.w0 || w1 != s.w1 || w2 != s.w2; }
};

struct StunTransactionId
{
    nwuint32_t w2; // msb
    nwuint32_t w1;
    nwuint32_t w0;

    StunTransactionId();
    Int96 get() const { return {w2.get(), w1.get(), w0.get()}; }
    void set(Int96 v);
};

struct StunHeader
{
    enum
    {
        MAGIC_COOKIE = 0x2112A442u
    };
    enum Method
    {
        Invalid = 0,
        BindingRequest = 1,
        SharedSecretRequest = 2,
        BindingResponse = 0x101u,
        SharedSecretResponse = 0x102u,
        BindingErrorResponse = 0x111u,
        SharedSecretErrorResponse = 0x112u
    };
    enum StunClass
    {
        Request = 0,
        Indication = 0x10,
        ResponseSuccess = 0x100,
        ResponseError = 0x110
    };

    StunHeader();

    static const StunHeader* fromPtr(const void* p);
    static StunHeader* fromPtr(void* p)
    {
        return const_cast<StunHeader*>(StunHeader::fromPtr(static_cast<const void*>(p)));
    }

    StunClass getClass() const;
    Method getMethod() const;
    void setMethod(Method method);
    size_t size() const;
    bool isValid() const;
    bool isResponse() const { return getClass() >= ResponseSuccess; }
    bool isRequest() const { return getClass() == Request; }

    nwuint16_t method;
    nwuint16_t length;
    const nwuint32_t magicCookie;
    StunTransactionId transactionId;
};

static_assert(sizeof(StunHeader) == 20, "Invalid StunHeader struct packing");

struct StunAttribute
{
    enum
    {
        INVALID = 0,
        MAPPED_ADDRESS = 1,
        USERNAME = 6u,
        MESSAGE_INTEGRITY = 8u,
        ERROR_CODE = 9u,
        REALM = 0x14,
        NONCE = 0x15u,
        XOR_MAPPED_ADDRESS = 0x20u,
        PRIORITY = 0x24u,
        USE_CANDIDATE = 0x25u,
        SOFTWARE = 0x8022u,
        ALTERNATE_SERVER = 0x8023u,
        FINGERPRINT = 0x8028u,
        ICE_CONTROLLED = 0x8029u,
        ICE_CONTROLLING = 0x802au
    };

    nwuint16_t type;
    nwuint16_t length; // in bytes

    StunAttribute() : type(INVALID), length(0) {}
    explicit StunAttribute(uint16_t type) : type(type), length(0) {}

    size_t size() const;
    const uint8_t* get() const;
};

// This attribute is dependant on the message header when using IPv6.
// Before reading or writing the address, the header transactionid must be set and cannot change afterwards.
class StunXorMappedAddress : public StunAttribute
{
public:
    enum
    {
        Family_v4 = 1,
        Family_v6
    };
    StunXorMappedAddress() : StunAttribute(XOR_MAPPED_ADDRESS) {}
    explicit StunXorMappedAddress(const transport::SocketAddress& address, const StunHeader& header)
        : StunAttribute(XOR_MAPPED_ADDRESS)
    {
        setAddress(address, header);
    }

    void setAddress(const transport::SocketAddress& address, const StunHeader& header);
    transport::SocketAddress getAddress(const StunHeader& header) const;
    uint16_t getFamily() const { return _family; }
    bool isValid() const;

private:
    nwuint16_t _family;
    nwuint16_t _port;
    union
    {
        uint32_t v4;
        uint8_t v6[16];
    } _address;
};

struct StunMappedAddress : public StunAttribute
{
    enum
    {
        Family_v4 = 1,
        Family_v6
    };

    StunMappedAddress() : StunAttribute(MAPPED_ADDRESS) {}

    // transaction id of stunHeader must be set before
    void setAddress(const transport::SocketAddress& address);
    transport::SocketAddress getAddress() const;
    uint16_t getFamily() const { return _family; }
    bool isValid() const;

private:
    nwuint16_t _family;
    nwuint16_t _port;
    union
    {
        uint32_t v4;
        uint8_t v6[16];
    } _address;
};

struct StunGenericAttribute : public StunAttribute
{
    uint8_t value[600];

    StunGenericAttribute() {}
    explicit StunGenericAttribute(uint16_t type) : StunAttribute(type) { value[0] = 0; }
    StunGenericAttribute(uint16_t type, const std::string& value) : StunAttribute(type) { setValue(value); }
    StunGenericAttribute(const StunGenericAttribute&) = delete;
    StunGenericAttribute& operator=(const StunGenericAttribute&) = delete;

    void setValue(const std::string& value);
    void setValue(const uint8_t* data, int length);

    std::string getUtf8() const;
};

struct StunAttribute64 : public StunAttribute
{
    StunAttribute64() { length = 2 * sizeof(value0); }
    StunAttribute64(uint16_t type, uint64_t value_)
        : StunAttribute(type),
          value1(value_ >> 32),
          value0(value_ & 0xFFFFFFFFu)
    {
        length = sizeof(value_);
    }

    uint64_t get() const;
    void set(uint64_t v);

private:
    nwuint32_t value1;
    nwuint32_t value0;
};

struct StunAttribute32 : public StunAttribute
{
    StunAttribute32() { length = sizeof(value); }
    StunAttribute32(uint16_t type, uint32_t value_) : StunAttribute(type), value(value_) { length = sizeof(value); }

    nwuint32_t value;
};

struct StunPriority : public StunAttribute32
{
    explicit StunPriority(uint32_t v) : StunAttribute32(StunAttribute::PRIORITY, v) {}
};

struct StunFingerprint : public StunAttribute32
{
    explicit StunFingerprint(uint32_t v) : StunAttribute32(StunAttribute::FINGERPRINT, v) {}
};

struct StunControlled : public StunAttribute64
{
    explicit StunControlled(uint64_t tieBreaker) : StunAttribute64(StunAttribute::ICE_CONTROLLED, tieBreaker) {}
};

struct StunControlling : public StunAttribute64
{
    explicit StunControlling(uint64_t tieBreaker) : StunAttribute64(StunAttribute::ICE_CONTROLLED, tieBreaker) {}
};
class StunMessageIntegrity : public StunAttribute
{
public:
    StunMessageIntegrity() : StunAttribute(MESSAGE_INTEGRITY)
    {
        length = 20;
        value[0] = '\0';
    };

    void getHmac(uint8_t data[20]);
    void setHmac(const uint8_t* data); // 20 bytes
    bool isMatch(uint8_t data[20]) const;

    static size_t size() { return sizeof(value) + sizeof(StunAttribute); }

private:
    uint8_t value[20];
};

class StunError : public StunAttribute
{
public:
    enum Code
    {
        BadRequest = 400,
        Unauthorized = 401,
        RoleConflict = 487,
        ServerError = 500,
    };
    StunError(int code, std::string phrase) : StunAttribute(ERROR_CODE)
    {
        std::memset(_phrase, 0, sizeof(_phrase));
        assert(phrase.size() < sizeof(_phrase) - 1);
        _code = ((code / 100) << 8) + (code % 100);
        std::strncpy(_phrase, phrase.c_str(), sizeof(_phrase));
        int newLength = 4 + phrase.size();
        length = ((newLength % 4 > 0) ? newLength + (4 - newLength % 4) : newLength);
    }

    std::string getPhrase() const { return std::string(_phrase, 0, length - sizeof(_code)); }
    int getCode() const { return getClass() * 100 + (_code & 0x7F); }
    int getClass() const { return (_code & 0x700) >> 8; };
    bool isValid() const;

private:
    nwuint32_t _code;
    char _phrase[500];
};

class StunUserName : public StunGenericAttribute
{
public:
    bool isTargetUser(const char* name) const;
    std::pair<std::string, std::string> getNames() const;
};

class StunTransactionIdGenerator
{
public:
    StunTransactionIdGenerator();
    Int96 next();

private:
    std::mt19937_64 _generator;
    std::uniform_int_distribution<uint64_t> _distribution;
};

class StunMessage
{
public:
    StunMessage();
    typedef utils::TlvIterator<StunAttribute> iterator;
    typedef utils::TlvIterator<const StunAttribute> const_iterator;
    StunMessage& add(const StunAttribute& attr);
    iterator begin();
    const_iterator cbegin() const;
    const_iterator begin() const { return cbegin(); };
    iterator end();
    const_iterator cend() const;
    const_iterator end() const { return cend(); };

    uint32_t computeFingerprint() const;

    void addMessageIntegrity(crypto::HMAC& hmacComputer);
    void addFingerprint();
    static const StunMessage* fromPtr(const void* ptr);
    size_t size() const { return header.length + sizeof(header); }
    const StunAttribute* getAttribute(uint16_t type) const;
    template <typename T>
    const T* getAttribute(uint16_t type) const
    {
        static_assert(std::is_base_of<StunAttribute, T>::value, "can only convert to subclasses of StunAttribute");
        const StunAttribute* attribute = getAttribute(type);
        if (attribute)
        {
            assert((intptr_t)attribute % alignof(uint32_t) == 0);
            return reinterpret_cast<const T*>(attribute);
        }
        return nullptr;
    }

    bool isValid() const;
    bool isAuthentic(crypto::HMAC& hmacComputer) const;

    StunMessage& operator=(const StunMessage& b);

    StunHeader header;
    uint8_t attributes[MTU];

private:
    void computeHMAC(crypto::HMAC& hmacComputer, uint8_t* hash20b) const;
};

bool isStunMessage(const void* data, size_t length);
ice::Int96 getStunTransactionId(const void* data, size_t length);
bool isRequest(const void* p);
bool isResponse(const void* p);

} // namespace ice
