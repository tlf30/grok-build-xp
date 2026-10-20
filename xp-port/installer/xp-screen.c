/* Attach to the installed pager console and write the visible rows.
   Looks for an xp-pager.exe whose path ends in \Grok\xp-pager.exe, so a
   lab copy at C:\xp-pager.exe is left alone. Does not close that process. */
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define PSAPI_VERSION 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdarg.h>

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void emit(const char *text, int n) {
    DWORD wrote = 0;
    HANDLE std_out;
    if (text == NULL || n <= 0) {
        return;
    }
    std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (std_out != NULL && std_out != INVALID_HANDLE_VALUE) {
        WriteFile(std_out, text, (DWORD)n, &wrote, NULL);
    }
    if (g_out != INVALID_HANDLE_VALUE) {
        WriteFile(g_out, text, (DWORD)n, &wrote, NULL);
    }
}

static void emit_line(const char *fmt, ...) {
    char line[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = wvsprintfA(line, fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(line)) {
        emit(line, n);
    }
}

static int ends_with_i(const WCHAR *path, const WCHAR *tail) {
    int np = lstrlenW(path);
    int nt = lstrlenW(tail);
    if (np < nt) {
        return 0;
    }
    return lstrcmpiW(path + (np - nt), tail) == 0;
}

static DWORD find_installed_pager(void) {
    HANDLE snap;
    PROCESSENTRY32W pe;
    DWORD found = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE process;
            WCHAR path[MAX_PATH];
            DWORD n;
            if (lstrcmpiW(pe.szExeFile, L"xp-pager.exe") != 0) {
                continue;
            }
            process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (process == NULL) {
                continue;
            }
            n = GetModuleFileNameExW(process, NULL, path, MAX_PATH);
            CloseHandle(process);
            if (n == 0 || n >= MAX_PATH) {
                continue;
            }
            if (ends_with_i(path, L"\\Grok\\xp-pager.exe") && pe.th32ProcessID > found) {
                found = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

typedef BOOL(WINAPI *FnGetCurrentConsoleFont)(HANDLE, BOOL, PCONSOLE_FONT_INFO);
typedef COORD(WINAPI *FnGetConsoleFontSize)(HANDLE, DWORD);

static void log_font(HANDLE con) {
    HMODULE kernel;
    FnGetCurrentConsoleFont get_font;
    FnGetConsoleFontSize get_size;
    CONSOLE_FONT_INFO info;
    COORD size;
    kernel = GetModuleHandleA("kernel32.dll");
    get_font = (FnGetCurrentConsoleFont)GetProcAddress(kernel, "GetCurrentConsoleFont");
    get_size = (FnGetConsoleFontSize)GetProcAddress(kernel, "GetConsoleFontSize");
    if (get_font == NULL) {
        emit_line("font_api=0\r\n");
        return;
    }
    ZeroMemory(&info, sizeof(info));
    if (!get_font(con, FALSE, &info)) {
        emit_line("font_ok=0 err=%lu\r\n", GetLastError());
        return;
    }
    emit_line("font_index=%lu\r\n", info.nFont);
    if (get_size != NULL) {
        size = get_size(con, info.nFont);
        emit_line("font_px=%d,%d\r\n", (int)size.X, (int)size.Y);
    }
}

static void dump_window(HANDLE con, int *saw_grok) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    SHORT top;
    SHORT left;
    int rows;
    int width;
    int i;
    *saw_grok = 0;
    if (!GetConsoleScreenBufferInfo(con, &info)) {
        emit_line("info_err=%lu\r\n", GetLastError());
        return;
    }
    emit_line(
        "size=%d,%d window=%d,%d-%d,%d\r\n",
        (int)info.dwSize.X,
        (int)info.dwSize.Y,
        (int)info.srWindow.Left,
        (int)info.srWindow.Top,
        (int)info.srWindow.Right,
        (int)info.srWindow.Bottom);
    top = info.srWindow.Top;
    left = info.srWindow.Left;
    rows = (int)info.srWindow.Bottom - (int)info.srWindow.Top + 1;
    width = (int)info.srWindow.Right - (int)info.srWindow.Left + 1;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > 30) {
        rows = 30;
    }
    if (width < 1) {
        width = 1;
    }
    if (width > 160) {
        width = 160;
    }
    for (i = 0; i < rows; i++) {
        WCHAR chars[180];
        char utf8[800];
        char prefix[8];
        DWORD got = 0;
        COORD coord;
        int nbytes;
        int j;
        int prefix_n;
        coord.X = left;
        coord.Y = (SHORT)(top + i);
        if (!ReadConsoleOutputCharacterW(con, chars, (DWORD)width, coord, &got)) {
            emit_line("read_err=%lu\r\n", GetLastError());
            return;
        }
        for (j = 0; j < (int)got; j++) {
            if (chars[j] < 32 || chars[j] == 127) {
                chars[j] = L'.';
            }
        }
        nbytes = WideCharToMultiByte(CP_UTF8, 0, chars, (int)got, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
        if (nbytes < 0) {
            nbytes = 0;
        }
        while (nbytes > 0 && utf8[nbytes - 1] == ' ') {
            nbytes--;
        }
        utf8[nbytes] = 0;
        if (strstr(utf8, "Grok") != NULL) {
            *saw_grok = 1;
        }
        prefix_n = wsprintfA(prefix, "%02d:", i);
        emit(prefix, prefix_n);
        emit(utf8, nbytes);
        emit("\r\n", 2);
    }
}

int main(int argc, char **argv) {
    const char *log_path = (argc >= 2) ? argv[1] : "screen.txt";
    DWORD pid = 0;
    int tries;
    HANDLE con;
    int saw = 0;
    UINT old_cp;

    g_out = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    FreeConsole();
    for (tries = 0; tries < 40; tries++) {
        pid = find_installed_pager();
        if (pid != 0) {
            break;
        }
        Sleep(500);
    }
    emit_line("pid=%lu\r\n", pid);
    if (pid == 0) {
        emit_line("result=missing\r\n");
        if (g_out != INVALID_HANDLE_VALUE) {
            CloseHandle(g_out);
        }
        return 2;
    }
    if (!AttachConsole(pid)) {
        emit_line("attach_err=%lu\r\n", GetLastError());
        emit_line("result=no_console\r\n");
        if (g_out != INVALID_HANDLE_VALUE) {
            CloseHandle(g_out);
        }
        return 3;
    }
    con = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (con == INVALID_HANDLE_VALUE) {
        emit_line("con_err=%lu\r\n", GetLastError());
        FreeConsole();
        if (g_out != INVALID_HANDLE_VALUE) {
            CloseHandle(g_out);
        }
        return 3;
    }
    log_font(con);
    old_cp = GetConsoleOutputCP();
    emit_line("out_cp=%u\r\n", old_cp);
    dump_window(con, &saw);
    CloseHandle(con);
    FreeConsole();
    emit_line("saw_grok=%d\r\n", saw);
    emit_line(saw ? "result=ok\r\n" : "result=blank\r\n");
    if (g_out != INVALID_HANDLE_VALUE) {
        CloseHandle(g_out);
    }
    return saw ? 0 : 4;
}
