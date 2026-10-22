// vrlink fixes, patched into driver_vrlink's imports.
// Wine's NotifyRouteChange is unsupported and leaves the handle NULL, so vrlink's network check on vrserver's main
// thread re-arms it on every call: a new (leaked) event plus GetAdaptersAddresses and GetIpNetTable twice, ~14 ms of
// NSI round trips to nsiproxy in winedevice.exe. A pending notification that never fires makes the check a no-op.
#include <windows.h>
#include <string.h>

void trace(const char *fmt, ...);

static DWORD(WINAPI *real_notify_route_change)(HANDLE *, OVERLAPPED *);

static DWORD WINAPI notify_route_change(HANDLE *handle, OVERLAPPED *overlapped)
{
    DWORD rc = real_notify_route_change(handle, overlapped);
    if (rc != ERROR_NOT_SUPPORTED || !handle || !overlapped || !overlapped->hEvent) {
        return rc;
    }
    *handle = overlapped->hEvent;
    return ERROR_IO_PENDING;
}

void patch_import(BYTE *base, const char *dll, const char *name, WORD ordinal, void *fn, void **orig)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) {
        return;
    }
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)base + d->Name, dll) || !d->OriginalFirstThunk) {
            continue;
        }
        IMAGE_THUNK_DATA *names = (void *)(base + d->OriginalFirstThunk), *slots = (void *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                    ? IMAGE_ORDINAL(names->u1.Ordinal) != ordinal
                    : !name || strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name)) {
                continue;
            }
            DWORD old;
            *orig = (void *)slots->u1.Function;
            VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            slots->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            trace("hooked vrlink %s!%s", dll, name ? name : "(ordinal)");
        }
    }
}

void net_hook_attach(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA("driver_vrlink.dll");
    if (!base) {
        return;
    }
    patch_import(base, "iphlpapi.dll", "NotifyRouteChange", 0, notify_route_change, (void **)&real_notify_route_change);
}
