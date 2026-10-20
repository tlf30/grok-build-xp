/* Write HKCU\Console\<exe> so a new console for that exe uses Cascadia Mono.
   The exe path is stored with backslashes turned into underscores, which is
   the key the XP console host looks up. Does not change the default console face. */
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int xp_set_console_face(const WCHAR *exe_path) {
    WCHAR key[560];
    HKEY hk;
    LONG rc;
    DWORD font_family = 0x36;
    DWORD font_weight = 0x190;
    DWORD font_size = 0x000F0000;
    const WCHAR *prefix = L"Console\\";
    const WCHAR *face = L"Cascadia Mono";
    int n = 0;
    int i;

    if (exe_path == NULL || exe_path[0] == 0) {
        return 0;
    }
    for (i = 0; prefix[i] != 0; i++) {
        if (n + 1 >= (int)(sizeof(key) / sizeof(key[0]))) {
            return 0;
        }
        key[n++] = prefix[i];
    }
    for (i = 0; exe_path[i] != 0; i++) {
        WCHAR ch = exe_path[i];
        if (ch == L'\\') {
            ch = L'_';
        }
        if (n + 1 >= (int)(sizeof(key) / sizeof(key[0]))) {
            return 0;
        }
        key[n++] = ch;
    }
    key[n] = 0;

    rc = RegCreateKeyExW(
        HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) {
        return 0;
    }
    RegSetValueExW(
        hk,
        L"FaceName",
        0,
        REG_SZ,
        (const BYTE *)face,
        (DWORD)((lstrlenW(face) + 1) * sizeof(WCHAR)));
    RegSetValueExW(hk, L"FontFamily", 0, REG_DWORD, (const BYTE *)&font_family, sizeof(font_family));
    RegSetValueExW(hk, L"FontWeight", 0, REG_DWORD, (const BYTE *)&font_weight, sizeof(font_weight));
    RegSetValueExW(hk, L"FontSize", 0, REG_DWORD, (const BYTE *)&font_size, sizeof(font_size));
    RegCloseKey(hk);
    return 1;
}

int xp_clear_console_face(const WCHAR *exe_path) {
    WCHAR key[560];
    const WCHAR *prefix = L"Console\\";
    int n = 0;
    int i;

    if (exe_path == NULL || exe_path[0] == 0) {
        return 0;
    }
    for (i = 0; prefix[i] != 0; i++) {
        if (n + 1 >= (int)(sizeof(key) / sizeof(key[0]))) {
            return 0;
        }
        key[n++] = prefix[i];
    }
    for (i = 0; exe_path[i] != 0; i++) {
        WCHAR ch = exe_path[i];
        if (ch == L'\\') {
            ch = L'_';
        }
        if (n + 1 >= (int)(sizeof(key) / sizeof(key[0]))) {
            return 0;
        }
        key[n++] = ch;
    }
    key[n] = 0;
    return RegDeleteKeyW(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
}
