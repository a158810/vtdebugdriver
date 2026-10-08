#include "stdafx.h"
#include <winioctl.h>
#include "driver_comm.h"
#include <iostream>
#include <cstring>

DriverComm::DriverComm() : m_hDevice(INVALID_HANDLE_VALUE) {}

DriverComm::~DriverComm() {
    Disconnect();
}

bool DriverComm::Connect(const std::wstring& device_path) {
    if (m_hDevice != INVALID_HANDLE_VALUE) return true;

    m_hDevice = CreateFileW(device_path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            0, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);

    if (m_hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "[!] Failed to open device: " << GetLastError() << std::endl;
        return false;
    }
    std::cout << "[+] Connected to driver device." << std::endl;
    return true;
}

void DriverComm::Disconnect() {
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
        std::cout << "[+] Driver disconnected." << std::endl;
    }
}

bool DriverComm::Ioctl(DWORD code, void* in, size_t in_size,
                        void* out, size_t out_size, DWORD* returned) {
    if (m_hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "[!] Not connected to driver." << std::endl;
        return false;
    }
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(m_hDevice, code, in, (DWORD)in_size,
                              out, (DWORD)out_size, &bytes, NULL);
    if (returned) *returned = bytes;
    if (!ok) {
        std::cerr << "[!] IOCTL 0x" << std::hex << code << " failed: "
                  << GetLastError() << std::dec << std::endl;
    }
    return ok != 0;
}

bool DriverComm::ReadKernelMemory(uint64_t addr, void* buf, size_t size) {
    struct Input { uint64_t addr; size_t size; };
    Input input = { addr, size };
    return Ioctl(IOCTL_DBGSYS_READ_KERNEL_MEMORY, &input, sizeof(input), buf, size);
}

bool DriverComm::WriteKernelMemory(uint64_t addr, const void* buf, size_t size) {
    size_t total = sizeof(uint64_t) + sizeof(size_t) + size;
    uint8_t* raw = new uint8_t[total];
    *(uint64_t*)raw = addr;
    *(size_t*)(raw + 8) = size;
    memcpy(raw + 16, buf, size);
    bool result = Ioctl(IOCTL_DBGSYS_WRITE_KERNEL_MEMORY, raw, total, nullptr, 0);
    delete[] raw;
    return result;
}

bool DriverComm::ReadMSR(uint32_t msr, uint64_t& value) {
    return Ioctl(IOCTL_DBGSYS_READ_MSR, &msr, sizeof(msr), &value, sizeof(value));
}

bool DriverComm::WriteMSR(uint32_t msr, uint64_t value) {
    struct Input { uint32_t msr; uint64_t value; };
    Input input = { msr, value };
    return Ioctl(IOCTL_DBGSYS_WRITE_MSR, &input, sizeof(input), nullptr, 0);
}

uint64_t DriverComm::GetPhysicalAddress(uint64_t virtual_addr) {
    uint64_t phys = 0;
    Ioctl(IOCTL_DBGSYS_GET_PHYSICAL_ADDR, &virtual_addr, sizeof(virtual_addr), &phys, sizeof(phys));
    return phys;
}

bool DriverComm::ReadPhysicalMemory(uint64_t phys_addr, void* buf, size_t size) {
    struct Input { uint64_t addr; size_t size; };
    Input input = { phys_addr, size };
    return Ioctl(IOCTL_DBGSYS_READ_PHYSICAL_MEM, &input, sizeof(input), buf, size);
}

bool DriverComm::WritePhysicalMemory(uint64_t phys_addr, const void* buf, size_t size) {
    size_t total = sizeof(uint64_t) + sizeof(size_t) + size;
    uint8_t* raw = new uint8_t[total];
    *(uint64_t*)raw = phys_addr;
    *(size_t*)(raw + 8) = size;
    memcpy(raw + 16, buf, size);
    bool result = Ioctl(IOCTL_DBGSYS_WRITE_PHYSICAL_MEM, raw, total, nullptr, 0);
    delete[] raw;
    return result;
}

bool DriverComm::HideProcess(DWORD pid) {
    return Ioctl(IOCTL_DBGSYS_HIDE_PROCESS, &pid, sizeof(pid), nullptr, 0);
}

bool DriverComm::QueryProcessInfo(DWORD pid, void* info, size_t size) {
    return Ioctl(IOCTL_DBGSYS_QUERY_PROCESS_INFO, &pid, sizeof(pid), info, size);
}
