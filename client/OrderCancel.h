/* Copyright (C) 2024 Interactive Brokers LLC. All rights reserved. This code is subject to the terms
 * and conditions of the IB API Non-Commercial License or the IB API Commercial License, as applicable. */

#pragma once
#ifndef TWS_API_CLIENT_ORDER_CANCEL_H
#define TWS_API_CLIENT_ORDER_CANCEL_H

#include <string>

#define UNSET_INTEGER INT_MAX

struct OrderCancel
{
    std::string manualOrderCancelTime = "";
    std::string extOperator = "";
    std::string externalUserId = "";
    int manualOrderIndicator = UNSET_INTEGER;
};

#endif
