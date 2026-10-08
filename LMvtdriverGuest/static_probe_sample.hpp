#pragma once

#include <ntddk.h>

struct LMV_STATIC_PROBE_SAMPLE
{
    ULONGLONG EntryFlags;
    ULONGLONG ExitFlags;
    ULONGLONG Token;
    ULONGLONG Mode;
    ULONG HandlerCalls;
    ULONG ContinueCalls;
};

static_assert(FIELD_OFFSET(LMV_STATIC_PROBE_SAMPLE, EntryFlags) == 0, "静态桩入口标志偏移不符");
static_assert(FIELD_OFFSET(LMV_STATIC_PROBE_SAMPLE, ExitFlags) == 8, "静态桩出口标志偏移不符");
static_assert(FIELD_OFFSET(LMV_STATIC_PROBE_SAMPLE, ContinueCalls) == 0x24, "静态桩继续计数偏移不符");