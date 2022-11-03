#pragma once

#include "config/ConfigReader.h"
#include "utils/Time.h"
#include <string>
namespace config
{

extern const char* g_LoadTestConfigFile;

class LoadTestConfig : public ConfigReader
{
public:
    CFG_PROP(std::string, ip, "");
    CFG_PROP(uint16_t, port, 8080);
    CFG_PROP(std::string, address, "localhost");
    CFG_PROP(uint16_t, numClients, 100);
};

} // namespace config
