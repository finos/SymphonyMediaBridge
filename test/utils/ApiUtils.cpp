#include "api/utils.h"
#include <gtest/gtest.h>

using IceState = ice::IceSession::State;
using DtlsState = transport::SrtpClient::State;

using namespace api::utils;

TEST(ApiUtils, IceStateSerialization)
{
    for (int state = 0; state < (int)IceState::LAST; state++)
    {
        auto serialized = toString((IceState)state);
        IceState deserialized = stringToIceState(serialized);
        logger::info("(int)IceState: %d, serialized as %s, deserialized as %d",
            "IceState.SerializeAndDeserialize",
            state,
            serialized,
            static_cast<std::underlying_type_t<IceState>>(deserialized));
        ASSERT_EQ(state, (int)deserialized);
    }
}

TEST(ApiUtils, DtlsStateSerialization)
{
    for (int state = 0; state < (int)DtlsState::LAST; state++)
    {
        auto serialized = toString((DtlsState)state);
        DtlsState deserialized = stringToDtlsState(serialized);
        logger::info("(int)DtlsState: %d, serialized as %s, deserialized as %d",
            "DtlsState.SerializeAndDeserialize",
            state,
            serialized,
            static_cast<std::underlying_type_t<IceState>>(deserialized));
        ASSERT_EQ(state, (int)deserialized);
    }
}
