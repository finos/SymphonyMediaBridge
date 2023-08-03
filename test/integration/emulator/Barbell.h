#pragma once
#include "nlohmann/json.hpp"
#include "test/integration/emulator/Httpd.h"
#include <string>

namespace emulator
{
std::string newIdString();

class Barbell
{
public:
    Barbell(emulator::HttpdFactory* httpd);
    Barbell(emulator::HttpdFactory* httpd, const std::string& barbellId);

    std::string allocate(const std::string& baseUrl, const std::string& conferenceId, bool controlling);
    void remove(const std::string& baseUrl);
    void configure(const std::string& body);
    const std::string& getId() const { return _id; }

private:
    emulator::HttpdFactory* _httpd;
    std::string _id;
    nlohmann::json _offer;
    std::string _baseUrl;
    std::string _conferenceId;
};

} // namespace emulator
