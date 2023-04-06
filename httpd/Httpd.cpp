#include "httpd/Httpd.h"
#include "httpd/HttpRequestHandler.h"
#include "httpd/Request.h"
#include "logger/Logger.h"
#include "utils/SocketAddress.h"
#include <cstring>
#include <microhttpd.h>

namespace
{

struct Context
{
    Context() : _request(nullptr), _response(nullptr) {}
    httpd::Request* _request;
    httpd::Response* _response;
};

int32_t getHeaders(void* cls, MHD_ValueKind, const char* key, const char* value)
{
    auto request = reinterpret_cast<httpd::Request*>(cls);
    request->_headers.emplace(key, value);
    return MHD_YES;
}

int32_t getParams(void* cls, MHD_ValueKind, const char* key, const char* value)
{
    auto request = reinterpret_cast<httpd::Request*>(cls);
    if (value)
    {
        request->_params.emplace(key, value);
    }
    else
    {
        request->_params.emplace(key, std::string());
    }

    return MHD_YES;
}

int32_t addCorsHeaders(const httpd::Request& request, MHD_Response* response)
{
    int32_t result = MHD_YES;
    result = MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    if (result != MHD_YES)
    {
        return result;
    }
    result = MHD_add_response_header(response, "Access-Control-Max-Age", "86400");
    if (result != MHD_YES)
    {
        return result;
    }
    const auto requestMethod = request._headers.find("Access-Control-Request-Method");
    if (requestMethod != request._headers.cend())
    {
        result = MHD_add_response_header(response, "Access-Control-Allow-Methods", requestMethod->second.c_str());
        if (result != MHD_YES)
        {
            return result;
        }
    }

    const auto requestHeaders = request._headers.find("Access-Control-Request-Headers");
    if (requestHeaders != request._headers.cend())
    {
        result = MHD_add_response_header(response, "Access-Control-Allow-Headers", requestHeaders->second.c_str());
        if (result != MHD_YES)
        {
            return result;
        }
    }
    return MHD_YES;
}

void httpdPanicCallback(void* cls, const char* file, unsigned int line, const char* reason)
{
    logger::error("MHD error %s at %s, %d", "httpd", reason, file, line);
}

int32_t answerCallback(void* cls,
    MHD_Connection* connection,
    const char* url,
    const char* method,
    const char*,
    const char* uploadData,
    size_t* uploadDataSize,
    void** conCls)
{
    auto context = reinterpret_cast<Context*>(*conCls);

    if (strncmp(method, "POST", 4) == 0 || strncmp(method, "PATCH", 5) == 0 || strncmp(method, "PUT", 3) == 0)
    {
        if (!context)
        {
            context = new Context();
            context->_request = new httpd::Request(method);
            *conCls = context;
            return MHD_YES;
        }

        if (*uploadDataSize != 0)
        {
            auto request = context->_request;
            if (!request)
            {
                return MHD_NO;
            }

            request->_body.append(uploadData, *uploadDataSize);
            request->_url = url;
            *uploadDataSize = 0;
            return MHD_YES;
        }
        // else have complete body
    }
    else
    {
        if (!context)
        {
            context = new Context();
            context->_request = new httpd::Request(method);
            *conCls = context;
            return MHD_YES;
        }
        else
        {
            auto request = context->_request;
            if (!request)
            {
                return MHD_NO;
            }
            request->_url = url;
        }
    }

    auto request = context->_request;
    if (!request)
    {
        return MHD_NO;
    }

    auto httpRequestHandler = reinterpret_cast<httpd::HttpRequestHandler*>(cls);
    MHD_get_connection_values(connection, MHD_HEADER_KIND, (MHD_KeyValueIterator)(&getHeaders), request);
    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, (MHD_KeyValueIterator)(&getParams), request);
    context->_response = new httpd::Response(httpRequestHandler->onRequest(*request));

    MHD_Response* mhdResponse;
    if (context->_response->_body.empty())
    {
        mhdResponse = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
        if (!mhdResponse)
        {
            logger::error("failed to create empty response. %s", "Httpd", url);
        }
    }
    else
    {
        mhdResponse = MHD_create_response_from_buffer(context->_response->_body.length(),
            const_cast<char*>(context->_response->_body.data()),
            MHD_RESPMEM_PERSISTENT);
        if (!mhdResponse)
        {
            logger::error("failed to create response. %s, body %s", "Httpd", url, context->_response->_body.c_str());
        }
        for (const auto& header : context->_response->_headers)
        {
            const auto addHeaderResult =
                MHD_add_response_header(mhdResponse, header.first.c_str(), header.second.c_str());
            if (addHeaderResult == MHD_NO)
            {
                logger::error("failed to add header to response %s,header %s:%s,  body %s",
                    "Httpd",
                    url,
                    header.first.c_str(),
                    header.second.c_str(),
                    context->_response->_body.c_str());
            }
        }
    }

    auto addCorsResult = addCorsHeaders(*request, mhdResponse);
    if (addCorsResult != MHD_YES)
    {
        logger::error("failed to add cors headers %s, body %s", "Httpd", url, context->_response->_body.c_str());
    }

    const auto mhdQueueResponse =
        MHD_queue_response(connection, static_cast<uint32_t>(context->_response->_statusCode), mhdResponse);
    if (mhdQueueResponse != MHD_YES)
    {
        logger::error("failed to queue response %s, body %s", "Httpd", url, context->_response->_body.c_str());
    }

    MHD_destroy_response(mhdResponse);
    return mhdQueueResponse;
}

void requestCompletedCallback(void*, MHD_Connection*, void** conCls, MHD_RequestTerminationCode requestTerminationCode)
{

    if (requestTerminationCode != MHD_REQUEST_TERMINATED_COMPLETED_OK)
    {
        logger::warn("Request completed with termination code %d", "Httpd", requestTerminationCode);
    }

    auto context = reinterpret_cast<Context*>(*conCls);
    if (!context)
    {
        return;
    }

    if (context->_request)
    {
        delete context->_request;
    }

    if (context->_response)
    {
        delete context->_response;
    }

    delete context;

    *conCls = nullptr;
}

void errorLogger(void*, const char* fmt, va_list ap)
{
    logger::errorImmediate(fmt, "MHD", ap);
}

void connectChangeCallback(void* cls,
    struct MHD_Connection* connection,
    void** socket_context,
    enum MHD_ConnectionNotificationCode toe)
{
    static std::atomic_int connectionCount;
    if (toe == MHD_CONNECTION_NOTIFY_STARTED)
    {
        ++connectionCount;
        // logger::info("connection started. %d %p", "Httpd", connectionCount.load(), connection);
    }
    else
    {
        --connectionCount;
        // logger::info("connection stopped. %d %p", "Httpd", connectionCount.load(), connection);
    }
}
} // namespace

namespace httpd
{

struct Httpd::OpaqueDaemon
{
    explicit OpaqueDaemon(MHD_Daemon* _impl) : _impl(_impl) {}

    ~OpaqueDaemon()
    {
        if (_impl)
        {
            MHD_stop_daemon(_impl);
        }
    }

    MHD_Daemon* _impl;
};

Httpd::Httpd(HttpRequestHandler& httpRequestHandler) : _daemon(nullptr), _httpRequestHandler(httpRequestHandler)
{
    MHD_set_panic_func(httpdPanicCallback, nullptr);
}

Httpd::~Httpd()
{
    delete _daemon;
}

bool Httpd::start(const transport::SocketAddress& socketAddress)
{
#ifdef __APPLE__
    const uint32_t flags = MHD_USE_POLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG;
#else
    const uint32_t flags = MHD_USE_EPOLL_INTERNAL_THREAD | MHD_USE_ERROR_LOG;
#endif

    static const uint16_t discardPort = 9;

    auto daemon = MHD_start_daemon(flags,
        discardPort,
        nullptr,
        nullptr,
        (MHD_AccessHandlerCallback)&answerCallback,
        &_httpRequestHandler,
        MHD_OPTION_EXTERNAL_LOGGER,
        errorLogger,
        this,
        MHD_OPTION_SOCK_ADDR,
        socketAddress.getIpv4(),
        MHD_OPTION_NOTIFY_COMPLETED,
        requestCompletedCallback,
        nullptr,
        MHD_OPTION_THREAD_POOL_SIZE,
        6,
        MHD_OPTION_NOTIFY_CONNECTION,
        connectChangeCallback,
        nullptr,
        MHD_OPTION_CONNECTION_TIMEOUT,
        120,
        MHD_OPTION_END);

    if (!daemon)
    {
        return false;
    }

    _daemon = new OpaqueDaemon(daemon);
    return true;
}

} // namespace httpd
