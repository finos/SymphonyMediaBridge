#pragma once

#include "api/Recording.h"
#include "legacyapi/ChannelBundle.h"
#include "legacyapi/Content.h"
#include "utils/Optional.h"
#include <vector>

namespace legacyapi
{

struct Conference
{
    std::string _id;
    std::vector<ChannelBundle> _channelBundles;
    std::vector<Content> _contents;
    utils::Optional<api::Recording> _recording;
};

} // namespace legacyapi
