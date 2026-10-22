// wtsapi32.dll for vrcompositor.exe. Its IsWorkstationLocked() reads WTSSessionInfoEx (and WTSIsRemoteSession) through
// WTSQuerySessionInformationA and treats a failed query as "locked"; Wine fails both classes, so the compositor ran
// the HMD in display-locked mode and overwrote app poses. This answers both for a local, unlocked console session.
#include <windows.h>
#include <wtsapi32.h>

static BOOL alloc_copy(const void *data, DWORD size, void **out, DWORD *bytes)
{
    void *p = HeapAlloc(GetProcessHeap(), 0, size);
    if (!p) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memcpy(p, data, size);
    *out = p;
    if (bytes) {
        *bytes = size;
    }
    return TRUE;
}

static BOOL query(DWORD session, WTS_INFO_CLASS cls, void **out, DWORD *bytes, BOOL wide)
{
    if (!out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (session == WTS_CURRENT_SESSION) {
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
    }
    switch (cls) {
    case WTSIsRemoteSession: {
        BYTE remote = 0;
        return alloc_copy(&remote, 1, out, bytes);
    }
    case WTSSessionId:
        return alloc_copy(&session, sizeof(session), out, bytes);
    case WTSConnectState: {
        INT state = WTSActive;
        return alloc_copy(&state, sizeof(state), out, bytes);
    }
    case WTSSessionInfoEx:
        if (wide) {
            WTSINFOEXW info = { 0 };
            info.Level = 1;
            info.Data.WTSInfoExLevel1.SessionId = session;
            info.Data.WTSInfoExLevel1.SessionState = WTSActive;
            info.Data.WTSInfoExLevel1.SessionFlags = WTS_SESSIONSTATE_UNLOCK;
            return alloc_copy(&info, sizeof(info), out, bytes);
        } else {
            WTSINFOEXA info = { 0 };
            info.Level = 1;
            info.Data.WTSInfoExLevel1.SessionId = session;
            info.Data.WTSInfoExLevel1.SessionState = WTSActive;
            info.Data.WTSInfoExLevel1.SessionFlags = WTS_SESSIONSTATE_UNLOCK;
            return alloc_copy(&info, sizeof(info), out, bytes);
        }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

BOOL WINAPI WTSQuerySessionInformationA(HANDLE server, DWORD session, WTS_INFO_CLASS cls, LPSTR *buffer, DWORD *bytes)
{
    return query(session, cls, (void **)buffer, bytes, FALSE);
}

BOOL WINAPI WTSQuerySessionInformationW(HANDLE server, DWORD session, WTS_INFO_CLASS cls, LPWSTR *buffer, DWORD *bytes)
{
    return query(session, cls, (void **)buffer, bytes, TRUE);
}

void WINAPI WTSFreeMemory(PVOID memory)
{
    HeapFree(GetProcessHeap(), 0, memory);
}

BOOL WINAPI WTSRegisterSessionNotification(HWND hwnd, DWORD flags)
{
    return TRUE;
}

BOOL WINAPI WTSUnRegisterSessionNotification(HWND hwnd)
{
    return TRUE;
}

BOOL WINAPI WTSRegisterSessionNotificationEx(HANDLE server, HWND hwnd, DWORD flags)
{
    return TRUE;
}

BOOL WINAPI WTSUnRegisterSessionNotificationEx(HANDLE server, HWND hwnd)
{
    return TRUE;
}

BOOL WINAPI WTSQueryUserToken(ULONG session, PHANDLE token)
{
    if (!token) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, token);
}
