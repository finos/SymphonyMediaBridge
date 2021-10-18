#define HTTP_IMPLEMENTATION
#include "external/http.h"
#include "logger/Logger.h"
#include "utils/SocketAddress.h"
#include "utils/Time.h"
namespace aws
{
transport::SocketAddress getPublicIpv4()
{
    auto request = http_get("http://169.254.169.254/latest/meta-data/public-ipv4", nullptr);
    if (request == nullptr)
    {
        return transport::SocketAddress();
    }

    http_status_t status = HTTP_STATUS_PENDING;
    size_t prev_size = -1;
    auto startTime = utils::Time::getAbsoluteTime();
    while (status == HTTP_STATUS_PENDING)
    {
        status = http_process(request);
        if (prev_size != request->response_size)
        {
            logger::debug("%zu byte(s) received.", "", request->response_size);
            prev_size = request->response_size;
        }
        if (utils::Time::getAbsoluteTime() - startTime > 2 * utils::Time::sec)
        {
            logger::error("Timeout waiting for AWS meta data", "AwsHarvester");
            status = HTTP_STATUS_FAILED;
            break;
        }
        utils::Time::nanoSleep(50 * utils::Time::ms);
    }
    if (status == HTTP_STATUS_FAILED)
    {
        logger::debug("request failed (%d) %s", "", request->status_code, request->reason_phrase);
        http_release(request);
        return transport::SocketAddress();
    }

    logger::debug("Content type: %s\n\n%s", "", request->content_type, (char const*)request->response_data);
    auto address = transport::SocketAddress::parse(static_cast<char const*>(request->response_data));
    http_release(request);

    return address;
}

} // namespace aws