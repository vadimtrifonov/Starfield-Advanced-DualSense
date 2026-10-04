#include <Windows.h>

#include <cstdio>
#include <initializer_list>

namespace
{
    enum class Completion { Cancelled, BeforeCancellation, AfterCancellation };
    Completion completion;
    HANDLE device = GetCurrentProcess();
    OVERLAPPED* pending = nullptr;
    unsigned calls = 0;
    unsigned failures = 0;

    void check(bool condition, const char* message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    }

    BOOL WINAPI requestCancellation(HANDLE handle, OVERLAPPED* operation)
    {
        check(handle == device && operation == pending, "cancel the outstanding operation");
        check(calls++ == 0, "request cancellation before waiting");
        if (completion == Completion::BeforeCancellation) {
            SetLastError(ERROR_NOT_FOUND);
            return FALSE;
        }
        // Deliberately leave the operation pending after cancellation is requested.
        return TRUE;
    }

    BOOL WINAPI awaitCompletion(HANDLE handle, OVERLAPPED* operation, DWORD* transferred, BOOL wait)
    {
        check(handle == device && operation == pending, "wait for the same operation");
        check(calls++ == 1 && wait == TRUE, "wait for terminal completion, not just cancellation");
        pending = nullptr;
        *transferred = completion == Completion::Cancelled ? 0 : 64;
        if (completion == Completion::Cancelled) {
            SetLastError(ERROR_OPERATION_ABORTED);
            return FALSE;
        }
        return TRUE;
    }
}

#define CancelIoEx requestCancellation
#define GetOverlappedResult awaitCompletion
#include "OverlappedIo.h"
#undef GetOverlappedResult
#undef CancelIoEx

int main()
{
    for (const auto result : {Completion::Cancelled, Completion::BeforeCancellation, Completion::AfterCancellation}) {
        OVERLAPPED operation{};
        pending = &operation;
        completion = result;
        calls = 0;
        sds::cancelIoAndWait(device, operation);
        check(calls == 2 && pending == nullptr, "I/O completes before its storage can leave scope");
    }
    return failures ? 1 : 0;
}
