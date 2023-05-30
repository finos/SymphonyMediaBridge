#include "test/integration/emulator/Conference.h"
#include "test/integration/emulator/HttpRequests.h"

namespace emulator
{
void Conference::create(const std::string& baseUrl, bool useGlobalPort)
{
    assert(_success == false);

    nlohmann::json responseBody;
    nlohmann::json requestBody = {{"last-n", 9}, {"global-port", useGlobalPort}};

    _success = awaitResponse<HttpPostRequest>(_httpd,
        baseUrl + "/conferences",
        requestBody.dump().c_str(),
        3 * utils::Time::sec,
        responseBody);

    if (_success)
    {
        _id = responseBody["id"].get<std::string>();
    }
}
} // namespace emulator
