#include <Windows.h>
#include <process.h>
#include <cstdio>
#include <cstring>

extern "C" unsigned int LmvTestDivide(unsigned int Numerator, unsigned int Divisor);
extern "C" void LmvTestDivideFault();

struct DivideResult
{
    unsigned int Value;
    DWORD ExceptionCode;
    ULONG_PTR ExceptionAddress;
};

static LONG CaptureException(EXCEPTION_POINTERS* Pointers, DivideResult* Result)
{
    if (Pointers->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    Result->ExceptionCode = Pointers->ExceptionRecord->ExceptionCode;
    Result->ExceptionAddress = reinterpret_cast<ULONG_PTR>(Pointers->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DivideResult RunDivide(unsigned int Numerator, unsigned int Divisor)
{
    DivideResult result = {};
    __try
    {
        result.Value = LmvTestDivide(Numerator, Divisor);
    }
    __except (CaptureException(GetExceptionInformation(), &result))
    {
    }
    return result;
}

static bool PrintResult(const char* Name, DWORD ThreadId, unsigned int Expected,
                        const DivideResult& Result, bool ExpectSeh)
{
    bool passed = ExpectSeh ? Result.ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO :
        Result.ExceptionCode == 0 && Result.Value == Expected;
    std::printf("[%s] %s TID=%lu value=%u expected=%u SEH=%08lx address=%p\n",
        passed ? "PASS" : "FAIL", Name, ThreadId, Result.Value, Expected,
        Result.ExceptionCode, reinterpret_cast<void*>(Result.ExceptionAddress));
    if (!passed && !ExpectSeh && Result.ExceptionCode != 0)
        std::printf("  Windows SEH caught the fault: no successful debugger repair was observed.\n");
    return passed;
}

struct WorkerState
{
    HANDLE Ready;
    HANDLE Start;
    unsigned int Numerator;
    unsigned int Expected;
    DWORD ThreadId;
    DivideResult Result;
};

static unsigned int __stdcall Worker(void* Context)
{
    WorkerState* state = static_cast<WorkerState*>(Context);
    state->ThreadId = GetCurrentThreadId();
    SetEvent(state->Ready);
    if (WaitForSingleObject(state->Start, INFINITE) != WAIT_OBJECT_0)
        return 1;
    state->Result = RunDivide(state->Numerator, 0);
    return 0;
}

static bool RunThreads(bool Interactive)
{
    HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WorkerState states[2] = {};
    HANDLE threads[2] = {};
    bool ready = start != nullptr;
    for (unsigned int index = 0; index < 2 && ready; ++index)
    {
        states[index].Ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        states[index].Start = start;
        states[index].Numerator = index == 0 ? 100 : 240;
        states[index].Expected = index == 0 ? 50 : 60;
        if (states[index].Ready == nullptr)
        {
            ready = false;
            break;
        }
        threads[index] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Worker, &states[index], 0, nullptr));
        ready = threads[index] != nullptr;
        if (ready)
            ready = WaitForSingleObject(states[index].Ready, INFINITE) == WAIT_OBJECT_0;
    }
    if (ready)
    {
        std::printf("Two workers ready: TID=%lu EAX=100 -> set RCX=2 -> result 50\n",
            states[0].ThreadId);
        std::printf("                   TID=%lu EAX=240 -> set RCX=4 -> result 60\n",
            states[1].ThreadId);
        std::printf("Both use the same fault RIP; identify each event by TID and EAX.\n");
        if (Interactive)
        {
            std::printf("Attach now if needed. Press Enter to release both workers...\n");
            char input[64];
            if (std::fgets(input, sizeof(input), stdin) == nullptr)
                ready = false;
        }
    }
    if (!ready)
        std::printf("[FAIL] worker setup/input failed; any created worker will finish using SEH.\n");
    if (start != nullptr)
        SetEvent(start);
    bool passed = ready;
    for (unsigned int index = 0; index < 2; ++index)
    {
        if (threads[index] != nullptr)
        {
            bool joined = WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0;
            if (!joined)
                ExitProcess(2);
            bool result = PrintResult(index == 0 ? "worker A" : "worker B", states[index].ThreadId,
                states[index].Expected, states[index].Result, !Interactive);
            passed = passed && result;
            CloseHandle(threads[index]);
        }
        if (states[index].Ready != nullptr)
            CloseHandle(states[index].Ready);
    }
    if (start != nullptr)
        CloseHandle(start);
    return passed;
}

static bool RunSelftest()
{
    bool passed = PrintResult("normal divide", GetCurrentThreadId(), 50, RunDivide(100, 2), false);
    passed = PrintResult("normal divide B", GetCurrentThreadId(), 60, RunDivide(240, 4), false) && passed;
    passed = PrintResult("SEH divide", GetCurrentThreadId(), 0, RunDivide(100, 0), true) && passed;
    passed = RunThreads(false) && passed;
    std::printf("selftest: %s (user-mode only; no driver handshake tested)\n", passed ? "PASS" : "FAIL");
    return passed;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("LMvtdriver R3 exception test (x64) PID=%lu main-TID=%lu\n",
        GetCurrentProcessId(), GetCurrentThreadId());
    std::printf("Fault instruction: %p (div ecx), exception=0xC0000094\n",
        reinterpret_cast<void*>(&LmvTestDivideFault));
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
        return RunSelftest() ? 0 : 1;
    if (argc != 1)
    {
        std::printf("Usage: LMvtdriverR3Test.exe [--selftest]\n");
        return 2;
    }
    std::printf("Attach by the printed PID. No faults occur until you select a mode.\n");
    unsigned int round = 0;
    for (;;)
    {
        std::printf("\nRound %u: [s] single repair [e] SEH/detach [t] two workers [q] quit > ", round + 1);
        char input[64];
        if (std::fgets(input, sizeof(input), stdin) == nullptr || input[0] == 'q' || input[0] == 'Q')
            break;
        if (input[0] == 's' || input[0] == 'S')
        {
            ++round;
            std::printf("TID=%lu: EAX=100 EDX=0 ECX=0. Set RCX=2 and continue; expect 50.\n",
                GetCurrentThreadId());
            PrintResult("single repair", GetCurrentThreadId(), 50, RunDivide(100, 0), false);
        }
        else if (input[0] == 'e' || input[0] == 'E')
        {
            ++round;
            std::printf("TID=%lu: detach while stopped; do NOT repair registers. Expect SEH 0xC0000094.\n",
                GetCurrentThreadId());
            PrintResult("SEH/detach", GetCurrentThreadId(), 0, RunDivide(100, 0), true);
        }
        else if (input[0] == 't' || input[0] == 'T')
        {
            ++round;
            RunThreads(true);
        }
        else
            std::printf("Choose s, e, t or q.\n");
    }
    std::printf("Test process exiting. Detach in the debugger before unloading drivers.\n");
    return 0;
}
