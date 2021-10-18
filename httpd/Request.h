#pragma once

#include "utils/StringBuilder.h"
#include <cassert>
#include <string>
#include <unordered_map>
namespace httpd
{

enum class Method
{
    GET,
    POST,
    DELETE,
    PATCH,
    OPTIONS
};

struct Request
{
    explicit Request(const char* methodString) : _method(Method::GET)
    {
        if (strncmp(methodString, "GET", 3) == 0)
        {
            _method = Method::GET;
        }
        else if (strncmp(methodString, "POST", 4) == 0)
        {
            _method = Method::POST;
        }
        else if (strncmp(methodString, "DELETE", 6) == 0)
        {
            _method = Method::DELETE;
        }
        else if (strncmp(methodString, "PATCH", 5) == 0)
        {
            _method = Method::PATCH;
        }
        else if (strncmp(methodString, "OPTIONS", 7) == 0)
        {
            _method = Method::OPTIONS;
        }
        else
        {
            assert(false);
        }
        _methodString = methodString;
    }

    Method _method;
    std::string _methodString;
    std::string _url;
    std::unordered_map<std::string, std::string> _headers;
    utils::StringBuilder<8192> _body;
};

} // namespace httpd
