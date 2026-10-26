// SPDX-License-Identifier: GPL-2.0-or-later
// Desktop-only Windows functions referenced by statically linked SDL3 subsystems that the UWP app
// never reaches. Xbox OS lacks those desktop DLLs, so __imp_<name> pointers are defined locally
// and check-imports.ps1 verifies that zero non-UWP DLL imports remain in the final binary.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

HWND WINAPI CreateWindowExWFails(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                 HINSTANCE, LPVOID) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
}
LRESULT WINAPI DefWindowProcWFails(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL WINAPI DestroyWindowFails(HWND) {
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}
BOOL WINAPI EnumWindowsFinds(WNDENUMPROC, LPARAM) { return TRUE; }
DWORD WINAPI GetWindowThreadProcessIdFails(HWND, LPDWORD process) {
    if (process) *process = 0;
    return 0;
}
int WINAPI MessageBoxAFails(HWND, LPCSTR, LPCSTR, UINT) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
BOOL WINAPI PostMessageWFails(HWND, UINT, WPARAM, LPARAM) {
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}
BOOL WINAPI PostThreadMessageWFails(DWORD, UINT, WPARAM, LPARAM) {
    SetLastError(ERROR_INVALID_THREAD_ID);
    return FALSE;
}
ATOM WINAPI RegisterClassWFails(const WNDCLASSW*) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}
BOOL WINAPI UnregisterClassWFails(LPCWSTR, HINSTANCE) {
    SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
    return FALSE;
}
HRESULT WINAPI SHGetFolderPathWFails(HWND, int, HANDLE, DWORD, LPWSTR path) {
    if (path) *path = 0;
    return E_NOTIMPL;
}
HINSTANCE WINAPI ShellExecuteWFails(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) {
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_NOASSOC));
}

} // namespace

#define BB_DESKTOP_SHIM(name, stub) extern "C" decltype(&name) __imp_##name = stub;
BB_DESKTOP_SHIM(CreateWindowExW, CreateWindowExWFails)
BB_DESKTOP_SHIM(DefWindowProcW, DefWindowProcWFails)
BB_DESKTOP_SHIM(DestroyWindow, DestroyWindowFails)
BB_DESKTOP_SHIM(EnumWindows, EnumWindowsFinds)
BB_DESKTOP_SHIM(GetWindowThreadProcessId, GetWindowThreadProcessIdFails)
BB_DESKTOP_SHIM(MessageBoxA, MessageBoxAFails)
BB_DESKTOP_SHIM(PostMessageW, PostMessageWFails)
BB_DESKTOP_SHIM(PostThreadMessageW, PostThreadMessageWFails)
BB_DESKTOP_SHIM(RegisterClassW, RegisterClassWFails)
BB_DESKTOP_SHIM(UnregisterClassW, UnregisterClassWFails)
BB_DESKTOP_SHIM(SHGetFolderPathW, SHGetFolderPathWFails)
BB_DESKTOP_SHIM(ShellExecuteW, ShellExecuteWFails)

extern "C" int vulkan_smoke(void) {
    return 21;
}
