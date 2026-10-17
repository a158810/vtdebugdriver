#pragma once

#include "lmv_control_shared.h"

static inline bool LmvKiDrainComplete(ULONGLONG InFlight, ULONG ArmedProcessors, ULONG Flags)
{
    return InFlight == 0 && ArmedProcessors == 0 && !(Flags & LMV_KI_DRAIN_ENABLED);
}

static inline bool LmvKiExitTargetValid(ULONGLONG Target, ULONGLONG ImageBase, ULONGLONG ImageEnd)
{
    return (Target >> 48) == 0xffff && (Target < ImageBase || Target >= ImageEnd);
}
