/* Project Helghast.exe: the release's launcher.
 *
 * All decisions live in kz3-setup\release\launch.py (host-tested: tools/release/test_launch.py). This stub only
 * starts the bundled Python without a console window on it:
 *     <dir>\toolchain\python\pythonw.exe "<dir>\kz3-setup\release\launch.py" --root "<dir>" <own arguments>
 * where <dir> is the folder this exe is in, waits for it (Steam and shortcuts see one process for the whole session)
 * and returns its exit code.
 *
 * --launcher-dry-run (first argument): write the command line it would run, or the error, to
 * <dir>\launcher-dry-run.txt (UTF-8) and exit without starting anything or showing a dialog (Proton smoke test).
 *
 * Build: tools/release/build_launcher.sh (MinGW-w64 gcc from the .#win shell; plain Win32, no C runtime extras).
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>

#define BIG 32768

static wchar_t dir[BIG], py[BIG], script[BIG], cmd[BIG * 2];

static const wchar_t *after_argv0(const wchar_t *cl)
{
    if (*cl == L'"') {
        cl++;
        while (*cl && *cl != L'"') cl++;
        if (*cl) cl++;
    } else {
        while (*cl && *cl != L' ' && *cl != L'\t') cl++;
    }
    while (*cl == L' ' || *cl == L'\t') cl++;
    return cl;
}

static int exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void dry_write(const wchar_t *text)
{
    wchar_t out[BIG];
    char utf8[BIG * 3];
    DWORD n = 0;
    _snwprintf(out, BIG, L"%ls\\launcher-dry-run.txt", dir);
    out[BIG - 1] = 0;
    int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, NULL, NULL);
    HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (len > 1) WriteFile(h, utf8, (DWORD)(len - 1), &n, NULL);
    WriteFile(h, "\r\n", 2, &n, NULL);
    CloseHandle(h);
}

static int fail(int dry, const wchar_t *what, const wchar_t *path)
{
    wchar_t msg[BIG];
    _snwprintf(msg, BIG, L"%ls\n%ls\n\nUnpack or install the whole release again, keeping its folders.\n\n"
               L"If it still fails, report it on the issues page: https://github.com/undecuple/project-helghast/issues", what, path);
    msg[BIG - 1] = 0;
    if (dry) dry_write(msg);
    else MessageBoxW(NULL, msg, L"Project Helghast", MB_OK | MB_ICONERROR);
    return 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR unused, int show)
{
    (void)inst; (void)prev; (void)unused; (void)show;
    DWORD n = GetModuleFileNameW(NULL, dir, BIG);
    if (n == 0 || n >= BIG) return 3;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 3;
    *slash = 0;
    /* "C:\" -> "C:\." so the quoted --root argument does not end in a backslash (it would escape the quote) */
    const wchar_t *rootarg = (slash > dir && slash[-1] == L':') ? L"\\." : L"";

    const wchar_t *rest = after_argv0(GetCommandLineW());
    int dry = 0;
    if (wcsncmp(rest, L"--launcher-dry-run", 18) == 0 && (rest[18] == 0 || rest[18] == L' ')) {
        dry = 1;
        rest += 18;
        while (*rest == L' ') rest++;
    }

    _snwprintf(py, BIG, L"%ls\\toolchain\\python\\pythonw.exe", dir);
    _snwprintf(script, BIG, L"%ls\\kz3-setup\\release\\launch.py", dir);
    py[BIG - 1] = script[BIG - 1] = 0;
    if (!exists(py)) return fail(dry, L"The bundled Python is missing:", py);
    if (!exists(script)) return fail(dry, L"The launcher script is missing:", script);

    int len = _snwprintf(cmd, BIG * 2, L"\"%ls\" \"%ls\" --root \"%ls%ls\"%ls%ls", py, script, dir, rootarg,
                         *rest ? L" " : L"", rest);
    if (len < 0) return fail(dry, L"The command line is too long:", dir);
    if (dry) {
        dry_write(cmd);
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    wchar_t cwd[BIG];
    _snwprintf(cwd, BIG, L"%ls%ls", dir, rootarg);
    cwd[BIG - 1] = 0;
    if (!CreateProcessW(py, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi))
        return fail(0, L"Could not start the bundled Python:", py);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
