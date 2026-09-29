#pragma once

#include <ntddk.h>

#define LMV_DEVICE_TYPE 0x8000
#define IOCTL_LMV_QUERY_STATUS CTL_CODE(LMV_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#define LMV_CONTROL_VERSION 1
#define LMV_STATUS_FLAG_SVM_RUNNING 0x00000001u

#define LMV_CPUID_CALL_LEAF 0x4C4D0001u
#define LMV_CPUID_CALL_MAGIC 0x4C4D5654434D414Cull
#define LMV_CPUID_PROTOCOL_VERSION 1u

typedef enum _LMV_CPUID_COMMAND
{
    LmvCpuidCommandQueryStatus = 1,
    LmvCpuidCommandNotifySharedCommand = 2
} LMV_CPUID_COMMAND;

#define LMV_CPUID_RESULT_SUCCESS 0u
#define LMV_CPUID_RESULT_INVALID_ARGUMENT 0xC000000Du
#define LMV_CPUID_RESULT_UNSUPPORTED 0xC00000BBu

typedef struct _LMV_CPUID_RESULT
{
    ULONG Status;
    ULONG Flags;
    ULONG Version;
    ULONG Sequence;
} LMV_CPUID_RESULT, *PLMV_CPUID_RESULT;

typedef struct _LMV_SHARED_COMMAND
{
    ULONG State;
    ULONG CommandId;
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Flags;
    ULONG Reserved;
    ULONG Reserved2;
    ULONGLONG Argument0;
    ULONGLONG Argument1;
    ULONGLONG Argument2;
    ULONGLONG Argument3;
} LMV_SHARED_COMMAND, *PLMV_SHARED_COMMAND;

typedef struct _LMV_SHARED_RESULT
{
    ULONG State;
    ULONG Status;
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Flags;
    ULONG Reserved;
    ULONG Reserved2;
    ULONGLONG Value0;
    ULONGLONG Value1;
    ULONGLONG Value2;
    ULONGLONG Value3;
} LMV_SHARED_RESULT, *PLMV_SHARED_RESULT;

typedef struct _LMV_QUERY_STATUS
{
    ULONG Version;
    ULONG Size;
    ULONG Flags;
    ULONG Reserved;
} LMV_QUERY_STATUS, *PLMV_QUERY_STATUS;
