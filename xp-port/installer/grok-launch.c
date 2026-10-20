/* Generic launcher for the XP pager.
   Starts xp-pager.exe in the signed-in user's profile, with a new console,
   and does not set GROK_HOME, TERM, or a browser path. */
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "console_font.h"

static void dirname_of(WCHAR *path) {
    WCHAR *slash = NULL;
    WCHAR *p;
    for (p = path; *p != 0; p++) {
        if (*p == L'\\' || *p == L'/') {
            slash = p;
        }
    }
    if (slash != NULL) {
        *slash = 0;
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show) {
    WCHAR self[MAX_PATH];
    WCHAR pager[MAX_PATH];
    WCHAR profile[MAX_PATH];
    WCHAR cmdline[1200];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n;

    (void)instance;
    (void)prev;
    (void)cmd;
    (void)show;

    n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 1;
    }
    dirname_of(self);
    if (lstrlenW(self) + 16 >= MAX_PATH) {
        return 1;
    }
    lstrcpyW(pager, self);
    lstrcatW(pager, L"\\xp-pager.exe");
    if (GetFileAttributesW(pager) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"xp-pager.exe is not in the Grok folder.", L"Grok", MB_ICONERROR);
        return 1;
    }
    if (!xp_set_console_face(pager)) {
        MessageBoxW(NULL, L"Could not set the console font for Grok.", L"Grok", MB_ICONERROR);
        return 1;
    }

    n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lstrcpyW(profile, self);
    }
    if (lstrlenW(pager) + 48 >= (int)(sizeof(cmdline) / sizeof(cmdline[0]))) {
        return 1;
    }
    lstrcpyW(cmdline, L"\"");
    lstrcatW(cmdline, pager);
    lstrcatW(cmdline, L"\" --no-auto-update --no-memory");

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    if (!CreateProcessW(
            pager,
            cmdline,
            NULL,
            NULL,
            FALSE,
            CREATE_NEW_CONSOLE,
            NULL,
            profile,
            &si,
            &pi)) {
        MessageBoxW(NULL, L"Could not start Grok.", L"Grok", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
