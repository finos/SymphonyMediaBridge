#include "external/http.h"
#include "nlohmann/json.hpp"
#include <string>

namespace emulator
{
class HttpRequest
{
public:
    ~HttpRequest();

    void awaitResponse(uint64_t timeout);

    bool isPending() const { return _status == HTTP_STATUS_PENDING; }
    bool hasFailed() const { return _status == HTTP_STATUS_FAILED; }
    bool isSuccess() const { return _status == HTTP_STATUS_COMPLETED; }

    std::string getResponse() const;

    nlohmann::json getJsonBody() const;

    int getCode() const { return _request->status_code; }

protected:
    HttpRequest() : _request(nullptr), _status(HTTP_STATUS_PENDING), _prevSize(0) {}
    http_t* _request;

private:
    http_status_t _status;
    size_t _prevSize;
};

class HttpPostRequest : public HttpRequest
{
public:
    HttpPostRequest(const char* url, const char* body);
};

class HttpPatchRequest : public HttpRequest
{
public:
    HttpPatchRequest(const char* url, const char* body)
    {
        _request = http_patch(url, body, body ? std::strlen(body) : 0, nullptr);
        assert(_request);
    }
};

class HttpGetRequest : public HttpRequest
{
public:
    HttpGetRequest(const char* url)
    {
        _request = http_get(url, nullptr);
        assert(_request);
    }
};

} // namespace emulator
