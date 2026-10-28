/* Stand-in for pythonw.exe in the launcher's Proton smoke test (tools/release/build_launcher.sh --smoke):
 * writes its own command line and working folder to fakepy-out.txt beside the launcher (two folders up from
 * toolchain\python\) and exits with code 7, so the test sees the quoting, the --root argument, the passed-through
 * arguments, the working folder and the exit code. Never shipped. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    static wchar_t text[65536], cwd[32768], out[32768];
    char utf8[65536 * 3];
    DWORD n;
    GetCurrentDirectoryW(32768, cwd);
    _snwprintf(text, 65536, L"cmd=%ls\r\ncwd=%ls\r\n", GetCommandLineW(), cwd);
    GetModuleFileNameW(NULL, out, 32768);
    for (int up = 0; up < 3; up++) {
        wchar_t *s = wcsrchr(out, L'\\');
        if (s) *s = 0;
    }
    wcscat(out, L"\\fakepy-out.txt");
    int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, NULL, NULL);
    HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, utf8, (DWORD)(len - 1), &n, NULL);
        CloseHandle(h);
    }
    return 7;
}
