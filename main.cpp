#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <string_view>
#include <atomic>
#include <shellapi.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

#define JSMN_STATIC
#include "jsmn.h"

#define WMAPP_WORKSPACE_UPDATE (WM_APP + 1)
#define WMAPP_RELOAD (WM_APP + 2)

static constexpr DWORD WALLFLASHER_PIPE_BUF_SIZE = 1024 * 1024;
static constexpr wchar_t WALLFLASHER_MUTEX_NAME[] = L"Local\\WallFlasher.SingleInstance";
static constexpr wchar_t WALLFLASHER_UNLOAD_EVENT_NAME[] = L"Local\\WallFlasher.UnloadEvent";
static constexpr wchar_t WALLFLASHER_RELOAD_EVENT_NAME[] = L"Local\\WallFlasher.ReloadEvent";

struct WallpaperEntry {
    HBITMAP hBitmap = nullptr;
    HDC hdc = nullptr;
    HGDIOBJ oldObject = nullptr;
};

struct ParsedMonitorState {
    std::wstring deviceKey;
    std::wstring workspace;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct MonitorInitEntry {
    std::wstring deviceKey;
    int width = 0;
    int height = 0;
    std::vector<std::wstring> workspaceNames;
};

struct MonitorInfo {
    RECT rc = {};
    std::wstring deviceKey;
    std::wstring currentWorkspace;
    WallpaperEntry* currentWallpaper = nullptr;
};

struct PendingWorkspaceUpdate {
    CRITICAL_SECTION lock = {};
    std::vector<std::wstring> workspaces;
    std::vector<bool> changed;
    std::vector<ParsedMonitorState> latestParsed;
    bool queued = false;
};

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;
    std::atomic_bool running{ true };
    ULONG_PTR gdiplusToken = 0;
    std::unordered_map<std::wstring, WallpaperEntry> wallpaperCache;
    std::vector<MonitorInfo> monitors;
    PendingWorkspaceUpdate pending;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    HANDLE hPipeThread = nullptr;
    HANDLE hUnloadEvent = nullptr;
    HANDLE hReloadEvent = nullptr;
    HANDLE hControlThread = nullptr;
    HANDLE hSingleInstanceMutex = nullptr;
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static std::wstring ToLower(std::wstring s);
static void LoadWallpapers(AppState* state);
static void FreeWallpapers(AppState* state);
static void ReloadWallpapersAndLayout(AppState* state);
static WallpaperEntry* ResolveWallpaper(AppState* state, const std::wstring& deviceKey, const std::wstring& workspace);
static void PaintOverlay(AppState* state, HDC hdc, const RECT& clip);
static std::string RunKomorebicState();

// jsmn helpers

static int TokenLen(const jsmntok_t& tok) {
    return tok.end - tok.start;
}

static bool TokenEquals(const char* json, const jsmntok_t& tok, const char* s) {
    const int len = TokenLen(tok);
    return tok.type == JSMN_STRING && (int)strlen(s) == len && strncmp(json + tok.start, s, len) == 0;
}

static int TokenAfter(const jsmntok_t* tokens, int tokCount, int idx) {
    if (idx < 0 || idx >= tokCount) return tokCount;
    int next = idx + 1;
    if (tokens[idx].type == JSMN_OBJECT || tokens[idx].type == JSMN_ARRAY) {
        const int end = tokens[idx].end;
        while (next < tokCount && tokens[next].start < end) {
            ++next;
        }
    }
    return next;
}

static int FindKey(const char* json, const jsmntok_t* tokens, int tokCount, int objectIdx, const char* key) {
    if (objectIdx < 0 || objectIdx >= tokCount || tokens[objectIdx].type != JSMN_OBJECT) return -1;
    int idx = objectIdx + 1;
    for (int i = 0; i < tokens[objectIdx].size && idx < tokCount; ++i) {
        const int keyIdx = idx++;
        if (keyIdx >= tokCount || idx >= tokCount) return -1;
        const int valueIdx = idx;
        if (TokenEquals(json, tokens[keyIdx], key)) return valueIdx;
        idx = TokenAfter(tokens, tokCount, valueIdx);
    }
    return -1;
}

static int ArrayElement(const jsmntok_t* tokens, int tokCount, int arrayIdx, int wanted) {
    if (arrayIdx < 0 || arrayIdx >= tokCount || tokens[arrayIdx].type != JSMN_ARRAY) return -1;
    if (wanted < 0 || wanted >= tokens[arrayIdx].size) return -1;
    int idx = arrayIdx + 1;
    for (int i = 0; i < wanted && idx < tokCount; ++i) idx = TokenAfter(tokens, tokCount, idx);
    return (idx < tokCount) ? idx : -1;
}

static int PrimitiveToInt(const char* json, const jsmntok_t& tok) {
    char buf[16] = {};
    int len = TokenLen(tok);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, json + tok.start, len);
    return atoi(buf);
}

static std::wstring TokenToWString(const char* json, const jsmntok_t& tok) {
    const int len = TokenLen(tok);
    if (len <= 0) return L"";
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, json + tok.start, len, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, json + tok.start, len, &result[0], wlen);
    return result;
}

static int ParseJsonTokens(const char* json, int jsonLen, std::vector<jsmntok_t>& tokens) {
    for (;;) {
        jsmn_parser parser;
        jsmn_init(&parser);
        const int result = jsmn_parse(&parser, json, jsonLen, tokens.data(), (unsigned int)tokens.size());
        if (result == JSMN_ERROR_NOMEM) {
            tokens.resize(tokens.size() * 2);
            continue;
        }
        return result;
    }
}

// komorebi

static int FindKomorebiMonitorElements(const char* json, const jsmntok_t* tokens, int tokCount) {
    if (tokCount < 1 || tokens[0].type != JSMN_OBJECT) return -1;
    const int wrappedStateIdx = FindKey(json, tokens, tokCount, 0, "state");
    const int stateIdx = (wrappedStateIdx >= 0) ? wrappedStateIdx : 0;
    if (stateIdx >= tokCount || tokens[stateIdx].type != JSMN_OBJECT) return -1;
    const int monitorsIdx = FindKey(json, tokens, tokCount, stateIdx, "monitors");
    if (monitorsIdx < 0 || tokens[monitorsIdx].type != JSMN_OBJECT) return -1;
    const int elementsIdx = FindKey(json, tokens, tokCount, monitorsIdx, "elements");
    return (elementsIdx >= 0 && tokens[elementsIdx].type == JSMN_ARRAY) ? elementsIdx : -1;
}

static std::wstring SanitizeDeviceKey(const std::wstring& deviceId) {
    std::wstring result;
    result.reserve(deviceId.size());
    for (wchar_t c : deviceId) {
        wchar_t lc = (wchar_t)towlower(c);
        if ((lc >= L'a' && lc <= L'z') || (lc >= L'0' && lc <= L'9')) result += lc;
    }
    return result;
}

static std::vector<ParsedMonitorState> ParseAllMonitors(const char* json, int jsonLen) {
    std::vector<ParsedMonitorState> result;
    thread_local std::vector<jsmntok_t> tokens(8192);
    const int tokCount = ParseJsonTokens(json, jsonLen, tokens);
    if (tokCount < 0) return result;
    const int elementsIdx = FindKomorebiMonitorElements(json, tokens.data(), tokCount);
    if (elementsIdx < 0) return result;
    const int monitorCount = tokens[elementsIdx].size;
    result.reserve(monitorCount);
    for (int i = 0; i < monitorCount; ++i) {
        const int monitorIdx = ArrayElement(tokens.data(), tokCount, elementsIdx, i);
        if (monitorIdx < 0 || tokens[monitorIdx].type != JSMN_OBJECT) continue;
        ParsedMonitorState monitor;
        const int deviceIdIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "device_id");
        if (deviceIdIdx >= 0 && tokens[deviceIdIdx].type == JSMN_STRING)
            monitor.deviceKey = SanitizeDeviceKey(TokenToWString(json, tokens[deviceIdIdx]));

        // komorebi uses  right/bottom for width/height
        const int sizeIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "size");
        if (sizeIdx >= 0 && tokens[sizeIdx].type == JSMN_OBJECT) {
            const int leftIdx   = FindKey(json, tokens.data(), tokCount, sizeIdx, "left");
            const int topIdx    = FindKey(json, tokens.data(), tokCount, sizeIdx, "top");
            const int rightIdx  = FindKey(json, tokens.data(), tokCount, sizeIdx, "right");
            const int bottomIdx = FindKey(json, tokens.data(), tokCount, sizeIdx, "bottom");
            if (leftIdx   >= 0 && tokens[leftIdx].type   == JSMN_PRIMITIVE) monitor.x      = PrimitiveToInt(json, tokens[leftIdx]);
            if (topIdx    >= 0 && tokens[topIdx].type    == JSMN_PRIMITIVE) monitor.y      = PrimitiveToInt(json, tokens[topIdx]);
            if (rightIdx  >= 0 && tokens[rightIdx].type  == JSMN_PRIMITIVE) monitor.width  = PrimitiveToInt(json, tokens[rightIdx]);
            if (bottomIdx >= 0 && tokens[bottomIdx].type == JSMN_PRIMITIVE) monitor.height = PrimitiveToInt(json, tokens[bottomIdx]);
        }

        const int workspacesIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "workspaces");
        if (workspacesIdx >= 0 && tokens[workspacesIdx].type == JSMN_OBJECT) {
            const int focusedIdx = FindKey(json, tokens.data(), tokCount, workspacesIdx, "focused");
            if (focusedIdx >= 0 && tokens[focusedIdx].type == JSMN_PRIMITIVE) {
                const int focused = PrimitiveToInt(json, tokens[focusedIdx]);
                const int wsElementsIdx = FindKey(json, tokens.data(), tokCount, workspacesIdx, "elements");
                if (wsElementsIdx >= 0 && tokens[wsElementsIdx].type == JSMN_ARRAY) {
                    const int wsIdx = ArrayElement(tokens.data(), tokCount, wsElementsIdx, focused);
                    if (wsIdx >= 0 && tokens[wsIdx].type == JSMN_OBJECT) {
                        const int nameIdx = FindKey(json, tokens.data(), tokCount, wsIdx, "name");
                        if (nameIdx >= 0 && tokens[nameIdx].type == JSMN_STRING)
                            monitor.workspace = ToLower(TokenToWString(json, tokens[nameIdx]));
                    }
                }
            }
        }
        result.push_back(std::move(monitor));
    }
    return result;
}

static std::vector<MonitorInitEntry> ParseMonitorInitState(const char* json, int jsonLen) {
    std::vector<MonitorInitEntry> result;
    thread_local std::vector<jsmntok_t> tokens(8192);
    const int tokCount = ParseJsonTokens(json, jsonLen, tokens);
    if (tokCount < 0) return result;
    const int elementsIdx = FindKomorebiMonitorElements(json, tokens.data(), tokCount);
    if (elementsIdx < 0) return result;
    const int monitorCount = tokens[elementsIdx].size;
    result.reserve(monitorCount);
    for (int i = 0; i < monitorCount; ++i) {
        const int monitorIdx = ArrayElement(tokens.data(), tokCount, elementsIdx, i);
        if (monitorIdx < 0 || tokens[monitorIdx].type != JSMN_OBJECT) continue;
        MonitorInitEntry entry;
        const int deviceIdIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "device_id");
        if (deviceIdIdx >= 0 && tokens[deviceIdIdx].type == JSMN_STRING)
            entry.deviceKey = SanitizeDeviceKey(TokenToWString(json, tokens[deviceIdIdx]));

        // (komorebi quirk: right=width, bottom=height)
        const int sizeIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "size");
        if (sizeIdx >= 0 && tokens[sizeIdx].type == JSMN_OBJECT) {
            const int rightIdx  = FindKey(json, tokens.data(), tokCount, sizeIdx, "right");
            const int bottomIdx = FindKey(json, tokens.data(), tokCount, sizeIdx, "bottom");

            if (rightIdx  >= 0 && tokens[rightIdx].type  == JSMN_PRIMITIVE) entry.width  = PrimitiveToInt(json, tokens[rightIdx]);
            if (bottomIdx >= 0 && tokens[bottomIdx].type == JSMN_PRIMITIVE) entry.height = PrimitiveToInt(json, tokens[bottomIdx]);
        }

        const int workspacesIdx = FindKey(json, tokens.data(), tokCount, monitorIdx, "workspaces");
        if (workspacesIdx >= 0 && tokens[workspacesIdx].type == JSMN_OBJECT) {
            const int wsElementsIdx = FindKey(json, tokens.data(), tokCount, workspacesIdx, "elements");
            if (wsElementsIdx >= 0 && tokens[wsElementsIdx].type == JSMN_ARRAY) {
                const int workspaceCount = tokens[wsElementsIdx].size;
                for (int j = 0; j < workspaceCount; ++j) {
                    const int wsIdx = ArrayElement(tokens.data(), tokCount, wsElementsIdx, j);
                    if (wsIdx >= 0 && tokens[wsIdx].type == JSMN_OBJECT) {
                        const int nameIdx = FindKey(json, tokens.data(), tokCount, wsIdx, "name");
                        if (nameIdx >= 0 && tokens[nameIdx].type == JSMN_STRING) {
                            std::wstring wsName = ToLower(TokenToWString(json, tokens[nameIdx]));
                            if (!wsName.empty()) entry.workspaceNames.push_back(std::move(wsName));
                        }
                    }
                }
            }
        }
        result.push_back(std::move(entry));
    }
    return result;
}

static std::string RunKomorebicState() {
    std::string result;
    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return result;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};
    wchar_t cmdLine[] = L"komorebic state";
    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        hWrite = nullptr;
        char buf[65536];
        DWORD read = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) result.append(buf, buf + read);
        WaitForSingleObject(pi.hProcess, 500);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (hWrite) CloseHandle(hWrite);
    if (hRead) CloseHandle(hRead);
    return result;
}

static bool RunKomorebicIgnoreSelf() {
    wchar_t exePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    std::wstring commandLine = L"komorebic.exe ignore-rule path \"";
    commandLine += exePath;
    commandLine += L"\"";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, 500);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

// console output

static void WriteHandleW(DWORD stdHandle, const std::wstring& text) {
    HANDLE h = GetStdHandle(stdHandle);
    if (!h || h == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        h = GetStdHandle(stdHandle);
    }
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &written, nullptr);
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], bytes, nullptr, nullptr);
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

static void WriteStderrW(const std::wstring& text) {
    WriteHandleW(STD_ERROR_HANDLE, text);
}

static int PrintWallpaperInfo() {
    std::string stateJson = RunKomorebicState();
    if (stateJson.empty()) {
        WriteStderrW(L"wallflasher: failed to read `komorebic state`\r\n");
        return 1;
    }
    std::vector<MonitorInitEntry> entries = ParseMonitorInitState(stateJson.data(), (int)stateJson.size());
    if (entries.empty()) {
        WriteStderrW(L"wallflasher: no monitors/workspaces found in `komorebic state`\r\n");
        return 1;
    }
    for (const auto& entry : entries) {
        if (entry.deviceKey.empty() || entry.width <= 0 || entry.height <= 0) continue;
        for (const auto& wsName : entry.workspaceNames) {
            std::wstring line;
            line.reserve(entry.deviceKey.size() + wsName.size() + 48);
            line += entry.deviceKey + L"_" + wsName + L".jpg : ";
            line += std::to_wstring(entry.width) + L"x" + std::to_wstring(entry.height) + L"\r\n";
            WriteHandleW(STD_OUTPUT_HANDLE, line);
        }
    }
    return 0;
}

// control event

static bool SignalRunningInstanceEvent(const wchar_t* eventName) {
    for (int i = 0; i < 20; ++i) {
        HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (hEvent) {
            const bool ok = SetEvent(hEvent) != FALSE;
            CloseHandle(hEvent);
            return ok;
        }
        Sleep(50);
    }
    return false;
}

static DWORD WINAPI ControlThreadProc(LPVOID param) {
    AppState* state = (AppState*)param;
    HANDLE events[] = { state->hUnloadEvent, state->hReloadEvent };

    while (state->running) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (!state->running) break;
        if (wait == WAIT_OBJECT_0) {
            PostMessageW(state->hwnd, WM_CLOSE, 0, 0);
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            if (!PostMessageW(state->hwnd, WMAPP_RELOAD, 0, 0)) break;
            continue;
        }
        break;
    }
    return 0;
}

// pipe thread

static DWORD WINAPI PipeThreadProc(LPVOID param) {
    AppState* state = (AppState*)param;
    std::string message;
    std::vector<char> buf(WALLFLASHER_PIPE_BUF_SIZE);
    while (state->running) {
        if (!ConnectNamedPipe(state->hPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            if (!state->running) break;
            Sleep(50);
            continue;
        }
        while (state->running) {
            message.clear();

            for (;;) {
                DWORD read = 0;
                const BOOL ok = ReadFile(state->hPipe, buf.data(), (DWORD)buf.size(), &read, nullptr);
                if (read > 0) message.append(buf.data(), buf.data() + read);
                if (ok) break;
                const DWORD err = GetLastError();
                if (err == ERROR_MORE_DATA) continue;
                break;
            }
            if (!state->running) break;
            if (message.empty()) break;
            if (std::string_view(message).substr(0, 60).find("Workspace") == std::string_view::npos)
                continue;

            std::vector<ParsedMonitorState> parsed = ParseAllMonitors(message.data(), (int)message.size());
            if (parsed.empty()) continue;
            bool shouldPost = false;
            EnterCriticalSection(&state->pending.lock);
            state->pending.latestParsed = parsed;
            if (state->pending.workspaces.size() != parsed.size()) {
                state->pending.workspaces.resize(parsed.size());
                state->pending.changed.resize(parsed.size());
            }
            bool any = false;
            for (size_t i = 0; i < parsed.size(); ++i) {
                if (!parsed[i].workspace.empty()) {
                    state->pending.workspaces[i] = parsed[i].workspace;
                    state->pending.changed[i] = true;
                    any = true;
                }
            }
            if (any && !state->pending.queued) {
                state->pending.queued = true;
                shouldPost = true;
            }
            LeaveCriticalSection(&state->pending.lock);
            if (shouldPost && !PostMessageW(state->hwnd, WMAPP_WORKSPACE_UPDATE, 0, 0)) {
                EnterCriticalSection(&state->pending.lock);
                state->pending.queued = false;
                LeaveCriticalSection(&state->pending.lock);
            }
        }
        DisconnectNamedPipe(state->hPipe);
    }
    return 0;
}

// monitor layout from komorebi state

static bool CalculateVirtualDesktopRect(const std::vector<ParsedMonitorState>& monitors, RECT& rect) {
    if (monitors.empty()) return false;
    rect.left   = monitors[0].x;
    rect.top    = monitors[0].y;
    rect.right  = monitors[0].x + monitors[0].width;
    rect.bottom = monitors[0].y + monitors[0].height;
    for (size_t i = 1; i < monitors.size(); ++i) {
        const ParsedMonitorState& monitor = monitors[i];
        if (monitor.x < rect.left) rect.left = monitor.x;
        if (monitor.y < rect.top) rect.top = monitor.y;
        if (monitor.x + monitor.width > rect.right) rect.right = monitor.x + monitor.width;
        if (monitor.y + monitor.height > rect.bottom) rect.bottom = monitor.y + monitor.height;
    }
    return true;
}

static void BuildMonitorLayout(AppState* state, const std::vector<ParsedMonitorState>& parsed) {
    state->monitors.clear();
    RECT virtualRect = {};
    if (!CalculateVirtualDesktopRect(parsed, virtualRect)) return;
    state->monitors.reserve(parsed.size());
    for (const auto& parsedMonitor : parsed) {
        MonitorInfo monitor;
        monitor.deviceKey = parsedMonitor.deviceKey;
        monitor.currentWorkspace = parsedMonitor.workspace;
        monitor.rc.left = parsedMonitor.x - virtualRect.left;
        monitor.rc.top = parsedMonitor.y - virtualRect.top;
        monitor.rc.right = monitor.rc.left + parsedMonitor.width;
        monitor.rc.bottom = monitor.rc.top + parsedMonitor.height;
        monitor.currentWallpaper = ResolveWallpaper(state, monitor.deviceKey, monitor.currentWorkspace);
        state->monitors.push_back(std::move(monitor));
    }
    if (state->hwnd) {
        const int windowW = virtualRect.right - virtualRect.left;
        const int windowH = virtualRect.bottom - virtualRect.top;
        SetWindowPos(state->hwnd, HWND_BOTTOM, virtualRect.left, virtualRect.top, windowW, windowH, SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

static WallpaperEntry* ResolveWallpaper(AppState* state, const std::wstring& deviceKey, const std::wstring& workspace) {
    if (!deviceKey.empty() && !workspace.empty()) {
        const std::wstring exactKey = deviceKey + L"_" + workspace;
        auto it = state->wallpaperCache.find(exactKey);
        if (it != state->wallpaperCache.end()) return &it->second;
    }
    if (!deviceKey.empty()) {
        auto it = state->wallpaperCache.find(deviceKey);
        if (it != state->wallpaperCache.end()) return &it->second;
    }
    auto it = state->wallpaperCache.find(L"wallpaper");
    return it != state->wallpaperCache.end() ? &it->second : nullptr;
}

static void ReloadWallpapersAndLayout(AppState* state) {
    EnterCriticalSection(&state->pending.lock);
    state->pending.workspaces.clear();
    state->pending.changed.clear();
    state->pending.latestParsed.clear();
    state->pending.queued = false;
    LeaveCriticalSection(&state->pending.lock);

    FreeWallpapers(state);
    LoadWallpapers(state);

    std::vector<ParsedMonitorState> parsed;
    std::string latestState = RunKomorebicState();
    if (!latestState.empty()) parsed = ParseAllMonitors(latestState.data(), (int)latestState.size());
    if (!parsed.empty()) BuildMonitorLayout(state, parsed);
    else for (auto& monitor : state->monitors)  monitor.currentWallpaper = ResolveWallpaper(state, monitor.deviceKey, monitor.currentWorkspace);
    if (state->hwnd) {
        InvalidateRect(state->hwnd, nullptr, FALSE);
        UpdateWindow(state->hwnd);
    }
}

// WinMain

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool unloadRequested = false;
    bool reloadRequested = false;
    bool wallpaperInfoRequested = false;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"unload") == 0 || _wcsicmp(argv[i], L"--unload") == 0)
                unloadRequested = true;
            else if (_wcsicmp(argv[i], L"reload") == 0 || _wcsicmp(argv[i], L"--reload") == 0)
                reloadRequested = true;
            else if (_wcsicmp(argv[i], L"wallpaper-info") == 0 || _wcsicmp(argv[i], L"--wallpaper-info") == 0)
                wallpaperInfoRequested = true;
            else {
                std::wstring msg = L"wallflasher: unknown flag: ";
                msg += argv[i];
                msg += L"\r\nusage: wallflasher.exe [unload|reload|wallpaper-info]\r\n";
                WriteStderrW(msg);
                LocalFree(argv);
                return 64;
            }
        }
        LocalFree(argv);
    }
    const int modeCount = (unloadRequested ? 1 : 0) + (reloadRequested ? 1 : 0) + (wallpaperInfoRequested ? 1 : 0);
    if (modeCount > 1) {
        WriteStderrW(L"wallflasher: choose only one mode: unload, reload, or wallpaper-info\r\n");
        return 64;
    }
    if (wallpaperInfoRequested) return PrintWallpaperInfo();
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, WALLFLASHER_MUTEX_NAME);
    if (!hSingleInstanceMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        int result = 0;
        if (unloadRequested)
            result = SignalRunningInstanceEvent(WALLFLASHER_UNLOAD_EVENT_NAME) ? 0 : 1;
        else if (reloadRequested)
            result = SignalRunningInstanceEvent(WALLFLASHER_RELOAD_EVENT_NAME) ? 0 : 1;
        CloseHandle(hSingleInstanceMutex);
        return result;
    }

    if (unloadRequested || reloadRequested) {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
        if (reloadRequested) {
            WriteStderrW(L"wallflasher: no running instance to reload\r\n");
            return 1;
        }
        return 0;
    }

    if (!RunKomorebicIgnoreSelf()) {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
        MessageBoxW(nullptr, L"Failed to add komorebi ignore rule. Is komorebi running and komorebic in PATH?", L"wallflasher", MB_ICONERROR | MB_OK);
        return 1;
    }

    AppState state;
    state.hInst = hInstance;
    state.hSingleInstanceMutex = hSingleInstanceMutex;
    InitializeCriticalSection(&state.pending.lock);

    state.hUnloadEvent = CreateEventW(nullptr, FALSE, FALSE, WALLFLASHER_UNLOAD_EVENT_NAME);
    if (!state.hUnloadEvent) {
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 1;
    }

    state.hReloadEvent = CreateEventW(nullptr, FALSE, FALSE, WALLFLASHER_RELOAD_EVENT_NAME);
    if (!state.hReloadEvent) {
        CloseHandle(state.hUnloadEvent);
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&state.gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        CloseHandle(state.hReloadEvent);
        CloseHandle(state.hUnloadEvent);
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 1;
    }

    LoadWallpapers(&state);
    std::vector<ParsedMonitorState> initParsed;
    {
        std::string initialState = RunKomorebicState();
        if (!initialState.empty()) initParsed = ParseAllMonitors(initialState.data(), (int)initialState.size());
    }
    BuildMonitorLayout(&state, initParsed);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WallFlasherClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExW(&wc)) {
        FreeWallpapers(&state);
        Gdiplus::GdiplusShutdown(state.gdiplusToken);
        CloseHandle(state.hReloadEvent);
        CloseHandle(state.hUnloadEvent);
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 2;
    }

    RECT windowRect = {};
    if (!CalculateVirtualDesktopRect(initParsed, windowRect)) {
        windowRect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        windowRect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        windowRect.right = windowRect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        windowRect.bottom = windowRect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    const int windowX = windowRect.left;
    const int windowY = windowRect.top;
    const int windowW = windowRect.right - windowRect.left;
    const int windowH = windowRect.bottom - windowRect.top;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName, L"WallFlasher", WS_POPUP, windowX, windowY, windowW, windowH, nullptr, nullptr, hInstance, &state);
    if (!hwnd) {
        FreeWallpapers(&state);
        Gdiplus::GdiplusShutdown(state.gdiplusToken);
        CloseHandle(state.hReloadEvent);
        CloseHandle(state.hUnloadEvent);
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 3;
    }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    state.hwnd = hwnd;

    state.hControlThread = CreateThread(nullptr, 0, ControlThreadProc, &state, 0, nullptr);
    if (!state.hControlThread) {
        DestroyWindow(hwnd);
        FreeWallpapers(&state);
        Gdiplus::GdiplusShutdown(state.gdiplusToken);
        CloseHandle(state.hReloadEvent);
        CloseHandle(state.hUnloadEvent);
        DeleteCriticalSection(&state.pending.lock);
        ReleaseMutex(state.hSingleInstanceMutex);
        CloseHandle(state.hSingleInstanceMutex);
        return 4;
    }

    SetWindowPos(hwnd, HWND_BOTTOM, windowX, windowY, windowW, windowH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);

    wchar_t pipeShort[64];
    wchar_t pipeFull[128];
    swprintf_s(pipeShort, L"wallflasher-komorebi-%u", GetCurrentProcessId());
    swprintf_s(pipeFull, L"\\\\.\\pipe\\%s", pipeShort);

    state.hPipe = CreateNamedPipeW(pipeFull, PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, WALLFLASHER_PIPE_BUF_SIZE, 0, nullptr);

    if (state.hPipe != INVALID_HANDLE_VALUE) {
        state.hPipeThread = CreateThread(nullptr, 0, PipeThreadProc, &state, 0, nullptr);
        if (state.hPipeThread) {
            std::wstring commandLine = L"komorebic subscribe-pipe ";
            commandLine += pipeShort;
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    state.running = false;
    std::wstring commandLine = L"komorebic unsubscribe-pipe ";
    commandLine += pipeShort;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    SetEvent(state.hUnloadEvent);
    SetEvent(state.hReloadEvent);
    WaitForSingleObject(state.hControlThread, 3000);
    CloseHandle(state.hControlThread);

    if (state.hPipe != INVALID_HANDLE_VALUE) CloseHandle(state.hPipe);
    if (state.hPipeThread) {
        WaitForSingleObject(state.hPipeThread, 3000);
        CloseHandle(state.hPipeThread);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    FreeWallpapers(&state);
    Gdiplus::GdiplusShutdown(state.gdiplusToken);

    CloseHandle(state.hReloadEvent);
    CloseHandle(state.hUnloadEvent);
    DeleteCriticalSection(&state.pending.lock);

    ReleaseMutex(state.hSingleInstanceMutex);
    CloseHandle(state.hSingleInstanceMutex);

    return (int)msg.wParam;
}

// window procedure

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    AppState* state = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS* pos = (WINDOWPOS*)lParam;
            pos->hwndInsertAfter = HWND_BOTTOM;
            pos->flags &= ~SWP_NOZORDER;
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintOverlay(state, hdc, ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WMAPP_WORKSPACE_UPDATE: {
            std::vector<std::wstring> pendingWorkspaces;
            std::vector<bool> changedMonitors;
            std::vector<ParsedMonitorState> parsedMonitors;

            EnterCriticalSection(&state->pending.lock);
            pendingWorkspaces = state->pending.workspaces;
            changedMonitors = state->pending.changed;
            parsedMonitors = state->pending.latestParsed;

            for (size_t i = 0; i < state->pending.changed.size(); ++i) state->pending.changed[i] = false;
            state->pending.queued = false;
            LeaveCriticalSection(&state->pending.lock);

            if (parsedMonitors.empty()) return 0;
            if (parsedMonitors.size() != state->monitors.size()) {
                BuildMonitorLayout(state, parsedMonitors);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            for (size_t i = 0; i < changedMonitors.size() && i < state->monitors.size(); ++i) {
                if (!changedMonitors[i] || pendingWorkspaces[i].empty()) continue;
                MonitorInfo& monitor = state->monitors[i];
                if (monitor.currentWorkspace != pendingWorkspaces[i]) {
                    monitor.currentWorkspace = pendingWorkspaces[i];
                    monitor.currentWallpaper = ResolveWallpaper(state, monitor.deviceKey, monitor.currentWorkspace);
                    InvalidateRect(hwnd, &monitor.rc, FALSE);
                }
            }
            return 0;
        }

        case WMAPP_RELOAD:
            ReloadWallpapersAndLayout(state);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            state->running = false;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// wallpaper loading/painting

static std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

static void LoadWallpapers(AppState* state) {
    wchar_t exePath[MAX_PATH];
    const DWORD exePathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exePathLen == 0 || exePathLen >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return;
    *slash = L'\0';
    const std::wstring wallpaperDir = std::wstring(exePath) + L"\\wallpapers\\";
    const std::wstring pattern = wallpaperDir + L"*.jpg";

    WIN32_FIND_DATAW findData = {};
    HANDLE findHandle = FindFirstFileW(pattern.c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring filename = findData.cFileName;
        const std::wstring fullPath = wallpaperDir + filename;
        const size_t dot = filename.find_last_of(L'.');
        const std::wstring key = ToLower(dot == std::wstring::npos ? filename : filename.substr(0, dot));
        Gdiplus::Bitmap bitmap(fullPath.c_str());
        if (bitmap.GetLastStatus() != Gdiplus::Ok) continue;
        HBITMAP hBitmap = nullptr;
        if (bitmap.GetHBITMAP(Gdiplus::Color(0, 0, 0), &hBitmap) != Gdiplus::Ok || !hBitmap) continue;
        WallpaperEntry entry;
        entry.hBitmap = hBitmap;
        entry.hdc = CreateCompatibleDC(nullptr);
        if (!entry.hdc) {
            DeleteObject(entry.hBitmap);
            continue;
        }
        entry.oldObject = SelectObject(entry.hdc, entry.hBitmap);
        if (!entry.oldObject) {
            DeleteDC(entry.hdc);
            DeleteObject(entry.hBitmap);
            continue;
        }
        auto old = state->wallpaperCache.find(key);
        if (old != state->wallpaperCache.end()) {
            SelectObject(old->second.hdc, old->second.oldObject);
            DeleteDC(old->second.hdc);
            DeleteObject(old->second.hBitmap);
        }
        state->wallpaperCache[key] = entry;
    } while (FindNextFileW(findHandle, &findData));
    FindClose(findHandle);
}

static void FreeWallpapers(AppState* state) {
    for (auto& monitor : state->monitors) monitor.currentWallpaper = nullptr;
    for (auto& pair : state->wallpaperCache) {
        WallpaperEntry& entry = pair.second;
        SelectObject(entry.hdc, entry.oldObject);
        DeleteDC(entry.hdc);
        DeleteObject(entry.hBitmap);
    }
    state->wallpaperCache.clear();
}

static void PaintOverlay(AppState* state, HDC hdc, const RECT& clip) {
    for (const auto& monitor : state->monitors) {
        const RECT& rc = monitor.rc;
        RECT intersection = {};
        if (!IntersectRect(&intersection, &rc, &clip)) continue;
        WallpaperEntry* wallpaper = monitor.currentWallpaper;
        if (!wallpaper) {
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            continue;
        }
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        BitBlt(hdc, rc.left, rc.top, w, h, wallpaper->hdc, 0, 0, SRCCOPY);
    }
}
