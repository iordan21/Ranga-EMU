#include "Embed.hpp"

#include <dwmapi.h>
#include <algorithm>
#include <vector>

namespace Ranga {

namespace {

constexpr const wchar_t* kHostClass = L"RangaEmuGameHost";
// Smaller windows of the emulator, its dialogs and progress boxes, stay floating above the game.
constexpr int kMinimumWidth = 480, kMinimumHeight = 270;

struct Host {
    HWND window = nullptr;
    HWND game = nullptr;
    DWORD processId = 0;
    HANDLE process = nullptr;
    std::wstring loading;
    ULONGLONG closeAsked = 0;
};

struct Search {
    DWORD processId;
    HWND best = nullptr;
    long long area = 0;
};

BOOL CALLBACK FindLargest(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<Search*>(data);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search.processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
    RECT rect{};
    GetClientRect(window, &rect);
    const long long width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width < kMinimumWidth || height < kMinimumHeight || width * height <= search.area) return TRUE;
    search.best = window;
    search.area = width * height;
    return TRUE;
}

void FitGame(Host& host) {
    if (host.game == nullptr) return;
    RECT client{};
    GetClientRect(host.window, &client);
    SetWindowPos(host.game, nullptr, 0, 0, client.right, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void FocusGame(Host& host) {
    if (host.game == nullptr || !IsWindow(host.game)) return;
    // Keyboard focus crosses into the emulator's thread only with the two input queues attached; SDL in the emulator
    // reads gamepads only while its window has that focus.
    const DWORD gameThread = GetWindowThreadProcessId(host.game, nullptr);
    AttachThreadInput(GetCurrentThreadId(), gameThread, TRUE);
    SetFocus(host.game);
}

void Adopt(Host& host, HWND game) {
    // A Win32 menu bar (Cemu, Xenia) goes; the game fills the host without the emulator's frame.
    if (GetMenu(game) != nullptr) SetMenu(game, nullptr);
    LONG_PTR style = GetWindowLongPtrW(game, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
    style |= WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(game, GWL_STYLE, style);
    LONG_PTR extended = GetWindowLongPtrW(game, GWL_EXSTYLE);
    extended &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
    SetWindowLongPtrW(game, GWL_EXSTYLE, extended);
    SetParent(game, host.window);
    host.game = game;
    FitGame(host);
    FocusGame(host);
    InvalidateRect(host.window, nullptr, TRUE);
}

LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE:
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            break;
        case WM_SIZE:
            if (host != nullptr) FitGame(*host);
            return 0;
        case WM_ACTIVATE:
            if (host != nullptr && LOWORD(wParam) != WA_INACTIVE) FocusGame(*host);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if (host != nullptr && host->game == nullptr) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(139, 141, 147));
                HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                HGDIOBJ old = SelectObject(dc, font);
                DrawTextW(dc, host->loading.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old);
                DeleteObject(font);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            if (host == nullptr) break;
            if (host->closeAsked != 0 && GetTickCount64() - host->closeAsked > 10000) {
                if (MessageBoxW(window, L"O emulador não fechou. Forçar o fechamento? O que não foi salvo no jogo se perde.", L"Ranga Emu", MB_YESNO | MB_ICONWARNING) == IDYES)
                    TerminateProcess(host->process, 1);
                return 0;
            }
            if (host->closeAsked == 0) host->closeAsked = GetTickCount64();
            // The emulator closes its own way, saving what it saves; the host goes when its process ends.
            if (host->game != nullptr && IsWindow(host->game)) PostMessageW(host->game, WM_CLOSE, 0, 0);
            else TerminateProcess(host->process, 0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}

void EmbedUntilExit(const PROCESS_INFORMATION& process, const std::wstring& title, bool fullscreen) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = HostProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = kHostClass;
    RegisterClassExW(&windowClass);

    Host host;
    host.processId = process.dwProcessId;
    host.process = process.hProcess;
    host.loading = L"Abrindo " + title + L"...";
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT frame = monitor.rcMonitor;
    if (fullscreen) {
        style = WS_POPUP | WS_CLIPCHILDREN;
    } else {
        // A 1280 x 720 picture in the middle of the work area, or the whole work area on a smaller screen.
        const RECT work = monitor.rcWork;
        RECT client{0, 0, std::min<LONG>(1280, work.right - work.left), std::min<LONG>(720, work.bottom - work.top)};
        AdjustWindowRect(&client, style, FALSE);
        const LONG width = client.right - client.left, height = client.bottom - client.top;
        frame = {work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, 0, 0};
        frame.right = frame.left + width;
        frame.bottom = frame.top + height;
    }
    host.window = CreateWindowExW(0, kHostClass, (std::wstring(kAppName) + L" - " + title).c_str(), style, frame.left, frame.top,
                                  frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, windowClass.hInstance, &host);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(host.window, 20, &dark, sizeof(dark));
    ShowWindow(host.window, SW_SHOWNORMAL);
    SetForegroundWindow(host.window);

    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, 250, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (host.game != nullptr && !IsWindow(host.game)) {
            host.game = nullptr;
            InvalidateRect(host.window, nullptr, TRUE);
        }
        // The game's window can replace an earlier one (a loading screen, the emulator's main window), so a larger one
        // that shows up later takes the place.
        Search search{host.processId};
        EnumWindows(FindLargest, reinterpret_cast<LPARAM>(&search));
        if (search.best != nullptr) {
            if (host.game != nullptr) ShowWindow(host.game, SW_HIDE);
            Adopt(host, search.best);
        }
    }
    DestroyWindow(host.window);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}

}
