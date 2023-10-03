#pragma once
#include "nlohmann/json.hpp"
#include "test/integration/emulator/CallConfigBuilder.h"
#include "test/integration/emulator/Httpd.h"
#include "transport/RtcTransport.h"
#include "utils/StdExtensions.h"
#include <string>
#include <unordered_set>

namespace emulator
{
std::string newGuuid();
std::string newIdString();

class Conference
{
public:
    explicit Conference(emulator::HttpdFactory* httpd) : _httpd(httpd), _success(false) {}

    void create(const std::string& baseUrl, bool useGlobalPort = true);
    void createFromExternal(const std::string& conferenceId)
    {
        assert(_success == false);
        _success = true;
        _id = conferenceId;
    }

    const std::string& getId() const { return _id; }

    bool isSuccess() const { return _success; }

private:
    emulator::HttpdFactory* const _httpd;
    std::string _id;
    bool _success;
};
} // namespace emulator
