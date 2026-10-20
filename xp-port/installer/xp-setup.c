/* Install Grok for any 32-bit Windows XP SP3 machine.
   Run xp-setup.exe from a folder that also contains xp-pager.exe,
   grok-launch.exe, and CascadiaMono.ttf. Files land in Program Files\Grok.
   Shortcuts call grok-launch.exe. User data stays in the user's own profile.
   -uninstall removes the program files and shortcuts, not the user's .grok data.
   -quiet skips the "press enter" pause. -log <path> also writes a log file. */
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shellapi.h>

#include "console_font.h"

static HANDLE g_log = INVALID_HANDLE_VALUE;
static int g_fail = 0;
static int g_reboot = 0;
static int g_quiet = 0;

static void log_raw(const char *text) {
    DWORD wrote = 0;
    HANDLE out;
    int n;
    if (text == NULL) {
        return;
    }
    n = lstrlenA(text);
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE) {
        WriteFile(out, text, (DWORD)n, &wrote, NULL);
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        WriteFile(g_log, text, (DWORD)n, &wrote, NULL);
    }
}

static void log_line(const char *text) {
    log_raw(text);
    log_raw("\r\n");
}

static void log_fmt(const char *fmt, DWORD a, DWORD b) {
    char buf[160];
    wsprintfA(buf, fmt, a, b);
    log_line(buf);
}

static void log_path(const char *label, const WCHAR *path) {
    char narrow[500];
    char line[560];
    int n;
    n = WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, (int)sizeof(narrow) - 1, NULL, NULL);
    if (n <= 0) {
        log_line(label);
        return;
    }
    narrow[sizeof(narrow) - 1] = 0;
    if (lstrlenA(label) + lstrlenA(narrow) + 4 >= (int)sizeof(line)) {
        log_line(label);
        log_line(narrow);
        return;
    }
    wsprintfA(line, "%s%s", label, narrow);
    log_line(line);
}

static void fail(const char *text) {
    log_raw("error=");
    log_line(text);
    g_fail = 1;
}

static int same_path(const WCHAR *a, const WCHAR *b) {
    WCHAR fa[MAX_PATH];
    WCHAR fb[MAX_PATH];
    DWORD na;
    DWORD nb;
    na = GetFullPathNameW(a, MAX_PATH, fa, NULL);
    nb = GetFullPathNameW(b, MAX_PATH, fb, NULL);
    if (na == 0 || nb == 0 || na >= MAX_PATH || nb >= MAX_PATH) {
        return lstrcmpiW(a, b) == 0;
    }
    return lstrcmpiW(fa, fb) == 0;
}

static int join2(WCHAR *dest, int cap, const WCHAR *dir, const WCHAR *name) {
    int n;
    n = lstrlenW(dir);
    if (n + 1 + lstrlenW(name) + 1 >= cap) {
        return 0;
    }
    lstrcpyW(dest, dir);
    if (n > 0 && dest[n - 1] != L'\\') {
        dest[n] = L'\\';
        dest[n + 1] = 0;
    }
    lstrcatW(dest, name);
    return 1;
}

static int folder(int csidl, WCHAR *dest) {
    return SHGetFolderPathW(NULL, csidl, NULL, 0, dest) == S_OK;
}

static int ensure_dir(const WCHAR *path) {
    if (CreateDirectoryW(path, NULL)) {
        return 1;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static DWORD file_size_of(const WCHAR *path) {
    HANDLE file;
    DWORD size;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0xFFFFFFFF;
    }
    size = GetFileSize(file, NULL);
    CloseHandle(file);
    return size;
}

static int copy_payload(const WCHAR *src, const WCHAR *dest) {
    DWORD src_size;
    DWORD dest_size;
    if (same_path(src, dest)) {
        log_path("skip_same ", dest);
        return 1;
    }
    if (CopyFileW(src, dest, FALSE)) {
        log_path("copied ", dest);
        return 1;
    }
    src_size = file_size_of(src);
    dest_size = file_size_of(dest);
    log_fmt("copy_err=%lu src=%lu", GetLastError(), src_size);
    if (src_size != 0xFFFFFFFF && src_size == dest_size) {
        log_path("already ", dest);
        return 1;
    }
    return 0;
}

static int reg_sz(HKEY root, const WCHAR *sub, const WCHAR *name, const WCHAR *value) {
    HKEY hk;
    LONG rc;
    rc = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) {
        return 0;
    }
    rc = RegSetValueExW(
        hk,
        name,
        0,
        REG_SZ,
        (const BYTE *)value,
        (DWORD)((lstrlenW(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static int reg_dword(HKEY root, const WCHAR *sub, const WCHAR *name, DWORD value) {
    HKEY hk;
    LONG rc;
    rc = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) {
        return 0;
    }
    rc = RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static int read_sz(HKEY root, const WCHAR *sub, const WCHAR *name, WCHAR *dest, DWORD chars) {
    HKEY hk;
    DWORD type = 0;
    DWORD bytes = chars * sizeof(WCHAR);
    LONG rc;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) {
        return 0;
    }
    rc = RegQueryValueExW(hk, name, NULL, &type, (BYTE *)dest, &bytes);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(WCHAR)) {
        return 0;
    }
    dest[(bytes / sizeof(WCHAR)) - 1] = 0;
    return 1;
}

static int read_dword(HKEY root, const WCHAR *sub, const WCHAR *name, DWORD *out) {
    HKEY hk;
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    LONG rc;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) {
        return 0;
    }
    rc = RegQueryValueExW(hk, name, NULL, &type, (BYTE *)out, &bytes);
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

/* Register Cascadia Mono in the console TrueType allow-list if it is absent.
   Writes FontAdded=1 only when this run creates the value. */
static int ensure_console_list(void) {
    HKEY hk;
    DWORD index;
    WCHAR name[32];
    WCHAR data[64];
    DWORD name_n;
    DWORD data_n;
    DWORD type;
    int zeros;
    WCHAR slot[16];
    LONG rc;
    const WCHAR *sub = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Console\\TrueTypeFont";

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0, KEY_READ | KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) {
        fail("could not open the console font list");
        return 0;
    }
    for (index = 0;; index++) {
        name_n = (DWORD)(sizeof(name) / sizeof(name[0]));
        data_n = sizeof(data);
        rc = RegEnumValueW(hk, index, name, &name_n, NULL, &type, (BYTE *)data, &data_n);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc != ERROR_SUCCESS) {
            break;
        }
        if (type == REG_SZ && lstrcmpiW(data, L"Cascadia Mono") == 0) {
            RegCloseKey(hk);
            log_line("tt_font=present");
            reg_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontAdded", 0);
            reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontSlot", name);
            return 1;
        }
    }
    slot[0] = 0;
    for (zeros = 1; zeros <= 8; zeros++) {
        int i;
        DWORD got_type = 0;
        DWORD got_n = 0;
        for (i = 0; i < zeros; i++) {
            slot[i] = L'0';
        }
        slot[zeros] = 0;
        if (RegQueryValueExW(hk, slot, NULL, &got_type, NULL, &got_n) == ERROR_FILE_NOT_FOUND) {
            break;
        }
    }
    if (zeros > 8) {
        RegCloseKey(hk);
        fail("console font list has no free slot");
        return 0;
    }
    rc = RegSetValueExW(
        hk,
        slot,
        0,
        REG_SZ,
        (const BYTE *)L"Cascadia Mono",
        (DWORD)((lstrlenW(L"Cascadia Mono") + 1) * sizeof(WCHAR)));
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) {
        fail("could not add Cascadia Mono to the console font list");
        return 0;
    }
    log_path("tt_font_added ", slot);
    reg_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontAdded", 1);
    reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontSlot", slot);
    return 1;
}

static int install_font_file(const WCHAR *src, const WCHAR *fonts_dir) {
    WCHAR dest[MAX_PATH];
    DWORD before;
    int existed;
    if (!join2(dest, MAX_PATH, fonts_dir, L"CascadiaMono.ttf")) {
        fail("font path is too long");
        return 0;
    }
    before = file_size_of(dest);
    existed = (before != 0xFFFFFFFF);
    if (!copy_payload(src, dest)) {
        if (!MoveFileExW(src, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
            log_fmt("font_delay_err=%lu", GetLastError(), 0);
            fail("could not copy CascadiaMono.ttf");
            return 0;
        }
        log_line("font_reboot=1");
        g_reboot = 1;
    } else if (!existed) {
        reg_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontFileAdded", 1);
    } else {
        DWORD previous = 0;
        if (!read_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontFileAdded", &previous) ||
            previous == 0) {
            reg_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontFileAdded", 0);
        }
    }
    if (!reg_sz(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
            L"Cascadia Mono (TrueType)",
            L"CascadiaMono.ttf")) {
        fail("could not register the font file");
        return 0;
    }
    if (AddFontResourceW(dest) > 0) {
        log_line("font_resource=1");
    } else {
        log_fmt("font_resource=0 err=%lu", GetLastError(), 0);
    }
    SendMessageTimeoutW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
    return 1;
}

static int make_shortcut(const WCHAR *lnk, const WCHAR *target, const WCHAR *workdir) {
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    HRESULT hr;
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr) || link == NULL) {
        log_fmt("shortcut_create=%lu", (DWORD)hr, 0);
        return 0;
    }
    link->lpVtbl->SetPath(link, target);
    link->lpVtbl->SetWorkingDirectory(link, workdir);
    link->lpVtbl->SetDescription(link, L"Grok");
    link->lpVtbl->SetIconLocation(link, target, 0);
    link->lpVtbl->SetShowCmd(link, SW_SHOWNORMAL);
    hr = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (FAILED(hr) || file == NULL) {
        link->lpVtbl->Release(link);
        return 0;
    }
    hr = file->lpVtbl->Save(file, lnk, TRUE);
    file->lpVtbl->Release(file);
    link->lpVtbl->Release(link);
    if (FAILED(hr)) {
        log_fmt("shortcut_save=%lu", (DWORD)hr, 0);
        return 0;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnk, NULL);
    log_path("shortcut ", lnk);
    return 1;
}

static void remove_font_slot(void) {
    DWORD added = 0;
    WCHAR slot[32];
    HKEY hk;
    if (!read_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontAdded", &added) || added == 0) {
        log_line("tt_font_kept=1");
        return;
    }
    if (!read_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontSlot", slot, 32)) {
        return;
    }
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Console\\TrueTypeFont",
            0,
            KEY_SET_VALUE,
            &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, slot);
        RegCloseKey(hk);
        log_path("tt_font_removed ", slot);
    }
}

static void delete_file_log(const WCHAR *path) {
    if (DeleteFileW(path)) {
        log_path("deleted ", path);
        return;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if (MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        log_path("delete_on_reboot ", path);
        g_reboot = 1;
        return;
    }
    log_path("delete_failed ", path);
    g_fail = 1;
}

static int do_uninstall(void) {
    WCHAR dir[MAX_PATH];
    WCHAR path[MAX_PATH];
    WCHAR desktop[MAX_PATH];
    WCHAR programs[MAX_PATH];
    DWORD file_added = 0;
    if (!read_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"InstallDir", dir, MAX_PATH)) {
        if (GetModuleFileNameW(NULL, dir, MAX_PATH) == 0) {
            fail("install directory is unknown");
            return 1;
        }
        {
            WCHAR *slash = NULL;
            WCHAR *p;
            for (p = dir; *p != 0; p++) {
                if (*p == L'\\') {
                    slash = p;
                }
            }
            if (slash != NULL) {
                *slash = 0;
            }
        }
    }
    log_path("uninstall ", dir);
    remove_font_slot();
    if (read_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall", L"FontFileAdded", &file_added) &&
        file_added != 0 && folder(CSIDL_FONTS, path)) {
        WCHAR font[MAX_PATH];
        if (join2(font, MAX_PATH, path, L"CascadiaMono.ttf")) {
            RemoveFontResourceW(font);
            delete_file_log(font);
        }
    }
    if (folder(CSIDL_COMMON_DESKTOPDIRECTORY, desktop) && join2(path, MAX_PATH, desktop, L"Grok.lnk")) {
        delete_file_log(path);
    }
    if (folder(CSIDL_COMMON_PROGRAMS, programs) && join2(path, MAX_PATH, programs, L"Grok.lnk")) {
        delete_file_log(path);
    }
    if (join2(path, MAX_PATH, dir, L"xp-pager.exe")) {
        xp_clear_console_face(path);
        delete_file_log(path);
    }
    if (join2(path, MAX_PATH, dir, L"grok-launch.exe")) {
        delete_file_log(path);
    }
    if (join2(path, MAX_PATH, dir, L"CascadiaMono.ttf")) {
        delete_file_log(path);
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Grok");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok\\XpInstall");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Grok");
    if (join2(path, MAX_PATH, dir, L"xp-setup.exe")) {
        delete_file_log(path);
    }
    log_line(g_fail ? "uninstall=failed" : "uninstall=ok");
    if (g_reboot) {
        log_line("reboot=1");
    }
    return g_fail ? 1 : 0;
}

static int do_install(const WCHAR *payload) {
    WCHAR program_files[MAX_PATH];
    WCHAR install_dir[MAX_PATH];
    WCHAR fonts_dir[MAX_PATH];
    WCHAR desktop[MAX_PATH];
    WCHAR programs[MAX_PATH];
    WCHAR src[MAX_PATH];
    WCHAR dest[MAX_PATH];
    WCHAR launch[MAX_PATH];
    WCHAR pager[MAX_PATH];
    const WCHAR *marker = L"SOFTWARE\\Grok\\XpInstall";
    const WCHAR *uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Grok";
    WCHAR uninstall_cmd[MAX_PATH + 40];

    if (!folder(CSIDL_PROGRAM_FILES, program_files)) {
        fail("Program Files folder is missing");
        return 1;
    }
    if (!join2(install_dir, MAX_PATH, program_files, L"Grok")) {
        fail("install path is too long");
        return 1;
    }
    if (!ensure_dir(install_dir)) {
        fail("could not create the Grok folder");
        return 1;
    }
    log_path("dir=", install_dir);
    if (!reg_sz(HKEY_LOCAL_MACHINE, marker, L"InstallDir", install_dir)) {
        fail("could not write the install registry key");
        return 1;
    }

    if (!join2(src, MAX_PATH, payload, L"xp-pager.exe") || !join2(dest, MAX_PATH, install_dir, L"xp-pager.exe")) {
        fail("pager path is too long");
        return 1;
    }
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
        fail("xp-pager.exe is not next to xp-setup.exe");
        return 1;
    }
    if (!copy_payload(src, dest)) {
        fail("could not copy xp-pager.exe");
        return 1;
    }
    lstrcpyW(pager, dest);

    if (!join2(src, MAX_PATH, payload, L"grok-launch.exe") || !join2(launch, MAX_PATH, install_dir, L"grok-launch.exe")) {
        fail("launcher path is too long");
        return 1;
    }
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
        fail("grok-launch.exe is not next to xp-setup.exe");
        return 1;
    }
    if (!copy_payload(src, launch)) {
        fail("could not copy grok-launch.exe");
        return 1;
    }

    if (GetModuleFileNameW(NULL, src, MAX_PATH) == 0 || !join2(dest, MAX_PATH, install_dir, L"xp-setup.exe")) {
        fail("could not locate xp-setup.exe");
        return 1;
    }
    if (!copy_payload(src, dest)) {
        fail("could not copy xp-setup.exe");
        return 1;
    }

    if (!join2(src, MAX_PATH, payload, L"CascadiaMono.ttf") || !join2(dest, MAX_PATH, install_dir, L"CascadiaMono.ttf")) {
        fail("font payload path is too long");
        return 1;
    }
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
        fail("CascadiaMono.ttf is not next to xp-setup.exe");
        return 1;
    }
    if (!copy_payload(src, dest)) {
        fail("could not keep a copy of CascadiaMono.ttf");
        return 1;
    }
    if (!folder(CSIDL_FONTS, fonts_dir) || !install_font_file(dest, fonts_dir)) {
        return 1;
    }
    if (!ensure_console_list()) {
        return 1;
    }
    if (!xp_set_console_face(pager)) {
        fail("could not set the console face for this user");
        return 1;
    }
    log_line("console_face=Cascadia Mono");

    if (FAILED(CoInitialize(NULL))) {
        fail("could not start shortcut setup");
        return 1;
    }
    if (!folder(CSIDL_COMMON_DESKTOPDIRECTORY, desktop) || !join2(dest, MAX_PATH, desktop, L"Grok.lnk") ||
        !make_shortcut(dest, launch, install_dir)) {
        fail("could not create the desktop shortcut");
    }
    if (!folder(CSIDL_COMMON_PROGRAMS, programs) || !join2(dest, MAX_PATH, programs, L"Grok.lnk") ||
        !make_shortcut(dest, launch, install_dir)) {
        fail("could not create the Start menu shortcut");
    }
    CoUninitialize();

    lstrcpyW(uninstall_cmd, L"\"");
    if (!join2(dest, MAX_PATH, install_dir, L"xp-setup.exe")) {
        fail("uninstall command is too long");
        return 1;
    }
    if (lstrlenW(dest) + 24 >= (int)(sizeof(uninstall_cmd) / sizeof(uninstall_cmd[0]))) {
        fail("uninstall command is too long");
        return 1;
    }
    lstrcpyW(uninstall_cmd, L"\"");
    lstrcatW(uninstall_cmd, dest);
    lstrcatW(uninstall_cmd, L"\" -uninstall -quiet");
    reg_sz(HKEY_LOCAL_MACHINE, uninstall, L"DisplayName", L"Grok");
    reg_sz(HKEY_LOCAL_MACHINE, uninstall, L"UninstallString", uninstall_cmd);
    reg_sz(HKEY_LOCAL_MACHINE, uninstall, L"Publisher", L"xAI");
    reg_sz(HKEY_LOCAL_MACHINE, uninstall, L"DisplayVersion", L"1.0.45");
    reg_dword(HKEY_LOCAL_MACHINE, uninstall, L"NoModify", 1);
    reg_dword(HKEY_LOCAL_MACHINE, uninstall, L"NoRepair", 1);
    log_path("launch=", launch);
    log_line(g_reboot ? "reboot=1" : "reboot=0");
    log_line(g_fail ? "install=failed" : "install=ok");
    return g_fail ? 1 : 0;
}

int main(void) {
    int argc = 0;
    LPWSTR *argv;
    int uninstall = 0;
    int i;
    WCHAR payload[MAX_PATH];
    WCHAR log_path_buf[MAX_PATH];
    int have_log = 0;
    DWORD n;
    int rc;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    log_path_buf[0] = 0;
    for (i = 1; argv != NULL && i < argc; i++) {
        if (lstrcmpiW(argv[i], L"-uninstall") == 0) {
            uninstall = 1;
        } else if (lstrcmpiW(argv[i], L"-quiet") == 0) {
            g_quiet = 1;
        } else if (lstrcmpiW(argv[i], L"-log") == 0 && i + 1 < argc) {
            lstrcpynW(log_path_buf, argv[++i], MAX_PATH);
            have_log = 1;
        } else if (lstrcmpiW(argv[i], L"-payload") == 0 && i + 1 < argc) {
            lstrcpynW(payload, argv[++i], MAX_PATH);
        }
    }
    if (argv != NULL) {
        LocalFree(argv);
    }
    if (have_log) {
        g_log = CreateFileW(
            log_path_buf,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (g_log == INVALID_HANDLE_VALUE) {
            log_line("error=could not open the log");
            return 1;
        }
    }
    n = GetModuleFileNameW(NULL, payload, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        fail("could not locate the setup folder");
        rc = 1;
    } else {
        WCHAR *slash = NULL;
        WCHAR *p;
        for (p = payload; *p != 0; p++) {
            if (*p == L'\\') {
                slash = p;
            }
        }
        if (slash != NULL) {
            *slash = 0;
        }
        /* -payload may have been overwritten by the exe directory. Re-read it. */
        argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (i = 1; argv != NULL && i < argc; i++) {
            if (lstrcmpiW(argv[i], L"-payload") == 0 && i + 1 < argc) {
                lstrcpynW(payload, argv[++i], MAX_PATH);
            }
        }
        if (argv != NULL) {
            LocalFree(argv);
        }
        log_line(uninstall ? "mode=uninstall" : "mode=install");
        rc = uninstall ? do_uninstall() : do_install(payload);
    }
    if (!g_quiet) {
        HANDLE in_handle = GetStdHandle(STD_INPUT_HANDLE);
        if (in_handle != NULL && in_handle != INVALID_HANDLE_VALUE &&
            GetFileType(in_handle) == FILE_TYPE_CHAR) {
            log_line("Press Enter to close.");
            {
                char buf[8];
                DWORD got = 0;
                ReadFile(in_handle, buf, 1, &got, NULL);
            }
        }
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
    }
    return rc;
}
