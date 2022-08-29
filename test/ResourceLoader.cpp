#include "ResourceLoader.h"
#include <TestConfig.h>
#include <fstream>
#include <iterator>

namespace
{

}

std::string ResourceLoader::loadAsString(const std::string& resource)
{
    const std::string resourcePath = TestConfig::getResourcePath(resource);
    std::ifstream input(resourcePath, std::ios::binary);
    if (!input.good())
    {
        const std::string message = std::string("Can't open file: ") + resourcePath;
        throw std::ios_base::failure(message);
    }

    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

nlohmann::json ResourceLoader::loadAsJson(const std::string& resource)
{
    return nlohmann::json::parse(loadAsString(resource));
}