#include "ConfigReader.h"
#include "logger/Logger.h"
#include "utils/ScopedFileHandle.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace config
{

bool ConfigReader::readFromFile(const std::string& fileName)
{
    logger::info("Reading config file %s", "ConfigReader", fileName.c_str());
    utils::ScopedFileHandle configFile(fopen(fileName.c_str(), "r"));
    if (!configFile.get())
    {
        logger::info("Failed reading config file %s, unable to open file", "ConfigReader", fileName.c_str());
        return false;
    }

    fseek(configFile.get(), 0, SEEK_END);
    const auto fileLength = ftell(configFile.get());
    fseek(configFile.get(), 0, SEEK_SET);
    std::vector<char> fileBuffer(fileLength + 1);
    fread(&fileBuffer[0], 1, fileLength, configFile.get());

    return parse(&fileBuffer[0]);
}

bool ConfigReader::readFromString(const std::string& json) { return parse(json.c_str()); }

bool ConfigReader::parse(const char* buffer)
{
    bool result = true;
    try
    {
        nlohmann::json parsedConfig = nlohmann::json::parse(buffer);

        for (auto property : _properties)
        {
            if (!property->read(parsedConfig))
            {
                logger::error("Config param is missing %s", "ConfigReader", property->getName().c_str());
                result = false;
            }
        }
    } catch (const std::exception& e)
    {
        logger::info("Failed reading config: %s", "ConfigReader", e.what());
        return false;
    }

    return result;
}

}