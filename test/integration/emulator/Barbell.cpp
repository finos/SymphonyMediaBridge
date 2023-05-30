#include "test/integration/emulator/Barbell.h"
#include "test/integration/emulator/HttpRequests.h"

namespace emulator
{

Barbell::Barbell(emulator::HttpdFactory* httpd) : _httpd(httpd), _id(newIdString()) {}

std::string Barbell::allocate(const std::string& baseUrl, const std::string& conferenceId, bool controlling)
{
    _baseUrl = baseUrl;
    _conferenceId = conferenceId;
    using namespace nlohmann;
    json body = {{"action", "allocate"},
        {"bundle-transport",
            {
                {"ice-controlling", controlling},
            }}};

    logger::debug("allocate barbell with %s", "BarbellReq", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        baseUrl + "/barbell/" + conferenceId + "/" + _id,
        body.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
        logger::debug("barbell allocated:%s", "BarbellReq", _offer.dump().c_str());
    }
    else
    {
        logger::error("failed to allocate barbell", "BarbellReq");
    }

    return _offer.dump();
}

void Barbell::configure(const std::string& body)
{
    auto requestBody = nlohmann::json::parse(body);
    requestBody["action"] = "configure";

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        _baseUrl + "/barbell/" + _conferenceId + "/" + _id,
        requestBody.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
    }
    else
    {
        logger::error("failed to configure barbell", "BarbellReq");
    }
}

void Barbell::remove(const std::string& baseUrl)
{
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpDeleteRequest>(_httpd,
        _baseUrl + "/barbell/" + _conferenceId + "/" + _id,
        9 * utils::Time::sec,
        responseBody);

    if (!success)
    {
        logger::error("Failed to delete barbell", "BarbellReq");
    }
}

} // namespace emulator
