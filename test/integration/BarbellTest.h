#pragma once
#include "IntegrationTest.h"

struct BarbellTest : public IntegrationTest
{
    BarbellTest();
    void SetUp() override;
    void TearDown() override;

    std::string _smbConfig1;
    std::string _smbConfig2;
    std::vector<transport::SocketAddress> _smb2interfaces;
};
