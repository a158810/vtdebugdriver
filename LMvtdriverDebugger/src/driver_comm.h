#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

// ============================================================================
// 驱动通信接口 - 提供给 dbgsys 驱动的 IOCTL 封装
// ============================================================================

// IOCTL 代码定义 (应用层使用)
// 等驱动端确定后同步
#define IOCTL_DBGSYS_READ_KERNEL_MEMORY   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_WRITE_KERNEL_MEMORY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_READ_MSR            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_WRITE_MSR           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_GET_PHYSICAL_ADDR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_READ_PHYSICAL_MEM   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_WRITE_PHYSICAL_MEM  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_HIDE_PROCESS        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DBGSYS_QUERY_PROCESS_INFO  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 驱动通信类
class DriverComm {
public:
    DriverComm();
    ~DriverComm();

    bool Connect(const std::wstring& device_path);
    void Disconnect();
    bool IsConnected() const { return m_hDevice != INVALID_HANDLE_VALUE; }

    // ---- 内核内存 ----
    bool ReadKernelMemory(uint64_t addr, void* buf, size_t size);
    bool WriteKernelMemory(uint64_t addr, const void* buf, size_t size);

    // ---- MSR ----
    bool ReadMSR(uint32_t msr, uint64_t& value);
    bool WriteMSR(uint32_t msr, uint64_t value);

    // ---- 物理内存 ----
    uint64_t GetPhysicalAddress(uint64_t virtual_addr);
    bool ReadPhysicalMemory(uint64_t phys_addr, void* buf, size_t size);
    bool WritePhysicalMemory(uint64_t phys_addr, const void* buf, size_t size);

    // ---- 进程操作 ----
    bool HideProcess(DWORD pid);
    bool QueryProcessInfo(DWORD pid, void* info, size_t size);

    // ---- 通用 IOCTL ----
    bool Ioctl(DWORD code, void* in, size_t in_size, void* out, size_t out_size, DWORD* returned = nullptr);

private:
    HANDLE m_hDevice;
};
