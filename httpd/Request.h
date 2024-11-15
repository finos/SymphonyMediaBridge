#pragma once

#include "memory/Array.h"
#include "utils/StringBuilder.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
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

class Body : public memory::Array<char, 8192>
{
public:
    utils::Span<const char> getSpan() const { return utils::Span<const char>(data(), size()); }
    std::string build() const { return std::string(data(), size()); }
    // void append(const char* s) { memory::Array<char, 8192>::append(s, strlen(s)); }
};

namespace
{
Method methodFromString(const char* method)
{
    if (std::strncmp(method, "GET", 3) == 0)
    {
        return Method::GET;
    }
    else if (std::strncmp(method, "POST", 4) == 0)
    {
        return Method::POST;
    }
    else if (std::strncmp(method, "DELETE", 6) == 0)
    {
        return Method::DELETE;
    }
    else if (std::strncmp(method, "PATCH", 5) == 0)
    {
        return Method::PATCH;
    }
    else if (std::strncmp(method, "OPTIONS", 7) == 0)
    {
        return Method::OPTIONS;
    }
    else if (std::strncmp(method, "PUT", 3) == 0)
    {
        return Method::PUT;
    }
    else
    {
        assert(false);
        return Method::GET;
    }
}

const char* methodToString(Method method)
{
    switch (method)
    {
    case Method::GET:
        return "GET";
    case Method::POST:
        return "POST";
    case Method::DELETE:
        return "DELETE";
    case Method::PATCH:
        return "PATCH";
    case Method::OPTIONS:
        return "OPTIONS";
    case Method::PUT:
        return "PUT";
    default:
        assert(false);
        return "unknown";
    }
}
} // namespace

struct Request
{
    explicit Request(const char* requestMethod, const char* url) : method(methodFromString(requestMethod)), url(url) {}

    explicit Request(Method requestMethod, const char* url) : method(requestMethod), url(url) {}

    std::string paramsToString() const
    {
        if (params.empty())
        {
            return "";
        }

        std::string r;
        r.reserve(50);
        r += "?";
        for (auto& pair : params)
        {
            if (r.size() > 1)
            {
                r += "&";
            }
            r += pair.first + (pair.second.empty() ? "" : "=" + pair.second);
        }
        return r;
    }

    template <typename T>
    T getHeader(const char*);

    template <>
    std::string getHeader<std::string>(const char* headerName)
    {
        auto it = headers.find(headerName);
        if (it != headers.end())
        {
            return it->second;
        }

        return std::string();
    }

    template <>
    size_t getHeader<size_t>(const char* headerName)
    {
        auto it = headers.find(headerName);
        if (it != headers.end())
        {
            return std::max(0LL, std::atoll(it->second.c_str()));
        }

        return 0;
    }

    template <>
    int64_t getHeader<int64_t>(const char* headerName)
    {
        auto it = headers.find(headerName);
        if (it != headers.end())
        {
            return std::max(0LL, std::atoll(it->second.c_str()));
        }

        return 0;
    }

    const char* getMethodString() const { return methodToString(method); }

    const Method method;
    const std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> params;
    Body body;
};

} // namespace httpd
