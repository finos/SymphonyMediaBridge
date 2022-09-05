#pragma once
#include "IntegrationTest.h"

struct BarbellTest : public IntegrationTest
{
    BarbellTest();
    void SetUp() override;
    void TearDown() override;
};