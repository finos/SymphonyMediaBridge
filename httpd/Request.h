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
    OPTIONS,
    PUT
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
        else if (strncmp(methodString, "PUT", 3) == 0)
        {
            _method = Method::PUT;
        }
        else
        {
            assert(false);
        }
        _methodString = methodString;
    }

    explicit Request(Method method) : _method(method)
    {
        switch (method)
        {
        case Method::GET:
            _methodString = "GET";
            break;
        case Method::POST:
            _methodString = "POST";
            break;
        case Method::DELETE:
            _methodString = "DELETE";
            break;
        case Method::PATCH:
            _methodString = "PATCH";
            break;
        case Method::OPTIONS:
            _methodString = "OPTIONS";
            break;
        case Method::PUT:
            _methodString = "PUT";
            break;
        default:
            assert(false);
        }
    }

    std::string paramsToString() const
    {
        if (_params.empty())
        {
            return "";
        }

        std::string r;
        r.reserve(50);
        r += "?";
        for (auto& pair : _params)
        {
            if (r.size() > 1)
            {
                r += "&";
            }
            r += pair.first + (pair.second.empty() ? "" : "=" + pair.second);
        }
        return r;
    }

    Method _method;
    std::string _methodString;
    std::string _url;
    std::unordered_map<std::string, std::string> _headers;
    std::unordered_map<std::string, std::string> _params;
    utils::StringBuilder<8192> _body;
};

} // namespace httpd
