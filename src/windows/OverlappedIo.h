#pragma once

#include <Windows.h>

namespace sds
{
    inline void cancelIoAndWait(HANDLE handle, OVERLAPPED& operation) noexcept
    {
        // Cancellation only requests completion; the buffer, event and OVERLAPPED
        // must remain alive even when normal completion wins the cancellation race.
        CancelIoEx(handle, &operation);
        DWORD transferred = 0;
        GetOverlappedResult(handle, &operation, &transferred, TRUE);
    }
}
