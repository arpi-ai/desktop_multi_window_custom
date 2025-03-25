#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#define _WIN32_WINNT 0x0602 // Target Windows 8 or later

#include <Ws2tcpip.h> // For InetPton
#include <windows.h>
#include <winsock2.h> // Include winsock2.h before windows.h

#include "flutter_window.h"
#include "flutter_windows.h"
#include "tchar.h"

#include <algorithm> // For std::min
#include <chrono>    // For timing
#include <iostream>
#include <memory>
#include <shlwapi.h> // For PathRemoveFileSpec
#include <sstream>   // For building JSON strings
#include <string>    // For std::to_string
#include <utility>
#pragma comment(lib, "Shlwapi.lib") // Link with Shlwapi.lib

#pragma comment(lib, "Ws2_32.lib") // Link with Ws2_32.lib

#include "include/desktop_multi_window/desktop_multi_window_plugin.h"
#include "multi_window_plugin_internal.h"

POINT lastCursorPos;
bool isDragging = false;
bool isProcessing = false;
std::chrono::time_point<std::chrono::steady_clock> mouseDownTime;

void SendPostRequest(const std::string &path, const std::string &jsonBody) {
    // Server settings
    std::string host = "127.0.0.1";
    u_short port = 37519; // Use u_short for port

    // Initialize WinSock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return;
    }

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed" << std::endl;
        WSACleanup();
        return;
    }

    // Server address setup
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    // Use InetPtonA for ANSI string
    if (InetPtonA(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
        std::cerr << "Invalid IP address" << std::endl;
        closesocket(sock);
        WSACleanup();
        return;
    }

    // Connect to the server
    if (connect(sock, reinterpret_cast<sockaddr *>(&serverAddr),
                sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Connection failed" << std::endl;
        closesocket(sock);
        WSACleanup();
        return;
    }

    // Create HTTP request
    std::string request = "POST " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request +=
        "Content-Type: application/json\r\n"; // Specify JSON content type
    request += "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += jsonBody;

    // Send request
    send(sock, request.c_str(), static_cast<int>(request.size()), 0);

    // Receive response (optional)
    char buffer[1024];
    int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        std::cout << "Response: " << buffer << std::endl;
    }

    // Close socket
    closesocket(sock);
    WSACleanup();
}

// Function to execute on click
void OnClickAction() {
    std::string path = "/click";
    std::string jsonBody = R"({"action": "screenshot"})";
    SendPostRequest(path, jsonBody);
}

void OnMovingAction(HWND hwnd) {
    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        int x = rect.left;
        int y = rect.top;

        std::ostringstream jsonBodyStream;
        jsonBodyStream << R"({"action": "move", "position": {"x": )" << x
                       << R"(, "y": )" << y << R"(}})";
        std::string jsonBody = jsonBodyStream.str();

        std::string path = "/move";
        SendPostRequest(path, jsonBody);
    } else {
        std::cerr << "Failed to get window position" << std::endl;
    }
}

void OnAutomaticDetectionAction() {
    std::string path = "/click";
    std::string jsonBody = R"({"action": "auto-screenshot"})";
    SendPostRequest(path, jsonBody);
}

void OnManualDetectionAction() {
    std::string path = "/click";
    std::string jsonBody = R"({"action": "manual-screenshot"})";
    SendPostRequest(path, jsonBody);
}

void OnOpenAction() {
    std::string path = "/open";
    std::string jsonBody = R"({"action": "open"})";
    SendPostRequest(path, jsonBody);
}

void OnCloseAction() {
    std::string path = "/close";
    std::string jsonBody = R"({"action": "close"})";
    SendPostRequest(path, jsonBody);
}

namespace {

WindowCreatedCallback _g_window_created_callback = nullptr;

TCHAR kFlutterWindowClassName[] = _T("FlutterMultiWindow");

int32_t class_registered_ = 0;

void RegisterWindowClass(WNDPROC wnd_proc) {
    if (class_registered_ == 0) {
        WNDCLASS window_class{};
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = kFlutterWindowClassName;
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.cbClsExtra = 0;
        window_class.cbWndExtra = 0;
        window_class.hInstance = GetModuleHandle(nullptr);
        window_class.hIcon = LoadIcon(window_class.hInstance, IDI_APPLICATION);
        window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        window_class.lpszMenuName = nullptr;
        window_class.lpfnWndProc = wnd_proc;
        RegisterClass(&window_class);
    }
    class_registered_++;
}

void UnregisterWindowClass() {
    class_registered_--;
    if (class_registered_ != 0) {
        return;
    }
    UnregisterClass(kFlutterWindowClassName, nullptr);
}

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
inline int Scale(int source, double scale_factor) {
    return static_cast<int>(source * scale_factor);
}

using EnableNonClientDpiScaling = BOOL __stdcall(HWND hwnd);

// Dynamically loads the |EnableNonClientDpiScaling| from the User32 module.
// This API is only needed for PerMonitor V1 awareness mode.
void EnableFullDpiSupportIfAvailable(HWND hwnd) {
    HMODULE user32_module = LoadLibraryA("User32.dll");
    if (!user32_module) {
        return;
    }
    auto enable_non_client_dpi_scaling =
        reinterpret_cast<EnableNonClientDpiScaling *>(
            GetProcAddress(user32_module, "EnableNonClientDpiScaling"));
    if (enable_non_client_dpi_scaling != nullptr) {
        enable_non_client_dpi_scaling(hwnd);
        FreeLibrary(user32_module);
    }
}

} // namespace

LRESULT CALLBACK FlutterWindow::CustomWndProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCCREATE: {
        auto window_struct = reinterpret_cast<CREATESTRUCT *>(lparam);
        // SetWindowLongPtr(hwnd, GWLP_USERDATA,
        // reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

        auto that = static_cast<FlutterWindow *>(window_struct->lpCreateParams);
        EnableFullDpiSupportIfAvailable(hwnd);
        that->window_handle_ = hwnd;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Set the background to be transparent
        RECT rect;
        GetClientRect(hwnd, &rect);
        SetBkMode(hdc, TRANSPARENT);

        // Get the executable's directory
        wchar_t exePath[MAX_PATH];
        GetModuleFileName(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpec(
            exePath); // Remove the executable name to get the directory

        // Append the relative path to resources
        std::wstring imagePath =
            std::wstring(exePath) +
            L"\\data\\flutter_assets\\assets\\images\\btn.bmp";

        // Load the image using the constructed relative path
        HBITMAP hBitmap = static_cast<HBITMAP>(LoadImage(
            nullptr, imagePath.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE));

        if (hBitmap) {
            HDC hMemDC = CreateCompatibleDC(hdc);
            SelectObject(hMemDC, hBitmap);
            BitBlt(hdc, -2, -2, 94, 94, hMemDC, 0, 0, SRCCOPY);
            DeleteDC(hMemDC);
            DeleteObject(hBitmap);
        }

        EndPaint(hwnd, &ps);
    }
    case WM_SETCURSOR: {
        // 마우스를 창에 올릴 때 커서를 손 모양으로 변경
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        if (isProcessing) {
            return 0;
        }
        isProcessing = true;
        // 마우스 왼쪽 버튼을 눌렀을 때 드래그 시작
        isDragging = true;
        SetCapture(hwnd);             // 마우스 이동 추적 시작
        GetCursorPos(&lastCursorPos); // 현재 마우스 위치 저장
        mouseDownTime =
            std::chrono::steady_clock::now(); // 마우스 눌린 시간 기록
        isProcessing = false;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (isProcessing) {
            return 0;
        }
        isProcessing = true;
        // 마우스 이동 시 창을 따라 움직이도록 설정
        if (isDragging) {
            POINT currentCursorPos;
            GetCursorPos(&currentCursorPos);

            // 창의 위치를 업데이트
            int dx = currentCursorPos.x - lastCursorPos.x;
            int dy = currentCursorPos.y - lastCursorPos.y;

            RECT windowRect;
            GetWindowRect(hwnd, &windowRect);
            MoveWindow(hwnd, windowRect.left + dx, windowRect.top + dy,
                       windowRect.right - windowRect.left,
                       windowRect.bottom - windowRect.top, TRUE);

            // 현재 마우스 위치를 새 위치로 업데이트
            lastCursorPos = currentCursorPos;
        }
        isProcessing = false;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (isProcessing) {
            return 0;
        }
        isProcessing = true;
        ReleaseCapture();

        // 눌린 시간과 뗀 시간 사이의 간격 확인
        auto mouseUpTime = std::chrono::steady_clock::now();
        std::chrono::duration<double> clickDuration =
            mouseUpTime - mouseDownTime;

        // 0.15초 미만이면 클릭으로 간주하여 함수 실행
        if (clickDuration.count() < 0.15) {
            OnClickAction();
        } else {
            OnMovingAction(hwnd);
        }

        isDragging = false;

        isProcessing = false;
        return 0;
    }
    case WM_RBUTTONUP: {
        // Create the context menu
        HMENU hMenu = CreatePopupMenu();
        if (hMenu) {
            AppendMenu(hMenu, MF_STRING, 1, L"Automatic Detection");
            AppendMenu(hMenu, MF_STRING, 2, L"Manual Detection");
            AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(hMenu, MF_STRING, 3, L"Open");
            AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(hMenu, MF_STRING, 4, L"Close");

            // Get the cursor position
            POINT pt;
            GetCursorPos(&pt);

            // Display the context menu
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                           nullptr);

            DestroyMenu(hMenu);
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wparam)) {
        case 1:
            OnAutomaticDetectionAction();
            break;
        case 2:
            OnManualDetectionAction();
            break;
        case 3:
            OnOpenAction();
            break;
        case 4:
            OnCloseAction();
            break;
        }
        return 0;
    }
    default: {
        if (FlutterWindow *that = GetThisFromHandle(hwnd)) {
            return that->MessageHandler(hwnd, message, wparam, lparam);
        }
    }
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

FlutterWindow::FlutterWindow(
    int64_t id, std::string args,
    const std::shared_ptr<FlutterWindowCallback> &callback)
    : callback_(callback), id_(id), window_handle_(nullptr), scale_factor_(1) {
    // DPI 인식을 비활성화하여 항상 1:1 스케일링 사용 (맨 앞으로 이동)
    SetProcessDPIAware();

    RegisterWindowClass(FlutterWindow::CustomWndProc);

    const POINT target_point = {static_cast<LONG>(10), static_cast<LONG>(10)};
    HWND window_handle = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, kFlutterWindowClassName, L"",
        WS_POPUPWINDOW, target_point.x, target_point.y,
        92, // 전체 창 크기
        92, nullptr, nullptr, GetModuleHandle(nullptr), this);

    // Set the window background to be transparent
    SetLayeredWindowAttributes(window_handle, RGB(255, 255, 255), 0,
                               LWA_COLORKEY);

    int width_scaled = 92;  // 74 -> 92로 변경
    int height_scaled = 92; // 74 -> 92로 변경
    int diameter = min(width_scaled, height_scaled);
    HRGN hRgn = CreateEllipticRgn(0, 0, diameter, diameter);
    SetWindowRgn(window_handle, hRgn, TRUE);

    // 항상 맨 위에 고정
    SetWindowPos(window_handle, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    RECT frame;
    GetClientRect(window_handle, &frame);
    flutter::DartProject project(L"data");

    project.set_dart_entrypoint_arguments(
        {"multi_window", std::to_string(id), std::move(args)});

    flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
        frame.right - frame.left, frame.bottom - frame.top, project);

    if (!flutter_controller_->engine() || !flutter_controller_->view()) {
        std::cerr << "Failed to setup FlutterViewController." << std::endl;
    }
    auto view_handle = flutter_controller_->view()->GetNativeWindow();
    SetParent(view_handle, window_handle);
    // MoveWindow(view_handle, 0, 0, frame.right - frame.left, frame.bottom -
    // frame.top, false);

    InternalMultiWindowPluginRegisterWithRegistrar(
        flutter_controller_->engine()->GetRegistrarForPlugin(
            "DesktopMultiWindowPlugin"));
    window_channel_ = WindowChannel::RegisterWithRegistrar(
        flutter_controller_->engine()->GetRegistrarForPlugin(
            "DesktopMultiWindowPlugin"),
        id_);

    if (_g_window_created_callback) {
        _g_window_created_callback(flutter_controller_.get());
    }

    ShowWindow(window_handle, SW_HIDE);
}

// static
FlutterWindow *FlutterWindow::GetThisFromHandle(HWND window) noexcept {
    return reinterpret_cast<FlutterWindow *>(
        GetWindowLongPtr(window, GWLP_USERDATA));
}

// static
LRESULT CALLBACK FlutterWindow::WndProc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto window_struct = reinterpret_cast<CREATESTRUCT *>(lparam);
        SetWindowLongPtr(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

        auto that = static_cast<FlutterWindow *>(window_struct->lpCreateParams);
        EnableFullDpiSupportIfAvailable(window);
        that->window_handle_ = window;
    } else if (FlutterWindow *that = GetThisFromHandle(window)) {
        return that->MessageHandler(window, message, wparam, lparam);
    }

    return DefWindowProc(window, message, wparam, lparam);
}

LRESULT FlutterWindow::MessageHandler(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {

    // Give Flutter, including plugins, an opportunity to handle window
    // messages.
    if (flutter_controller_) {
        std::optional<LRESULT> result =
            flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                          lparam);
        if (result) {
            return *result;
        }
    }

    auto child_content_ = flutter_controller_
                              ? flutter_controller_->view()->GetNativeWindow()
                              : nullptr;

    switch (message) {
    case WM_FONTCHANGE: {
        flutter_controller_->engine()->ReloadSystemFonts();
        break;
    }
    case WM_DESTROY: {
        Destroy();
        if (!destroyed_) {
            destroyed_ = true;
            if (auto callback = callback_.lock()) {
                callback->OnWindowDestroy(id_);
            }
        }
        return 0;
    }
    case WM_CLOSE: {
        if (auto callback = callback_.lock()) {
            callback->OnWindowClose(id_);
        }
        break;
    }
    case WM_DPICHANGED: {
        auto newRectSize = reinterpret_cast<RECT *>(lparam);
        LONG newWidth = newRectSize->right - newRectSize->left;
        LONG newHeight = newRectSize->bottom - newRectSize->top;

        SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top,
                     newWidth, newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

        return 0;
    }
    case WM_SIZE: {
        RECT rect;
        GetClientRect(window_handle_, &rect);
        if (child_content_ != nullptr) {
            // Size and position the child window.
            MoveWindow(child_content_, rect.left, rect.top,
                       rect.right - rect.left, rect.bottom - rect.top, TRUE);
        }
        return 0;
    }

    case WM_ACTIVATE: {
        if (child_content_ != nullptr) {
            SetFocus(child_content_);
        }
        return 0;
    }
    default:
        break;
    }

    return DefWindowProc(window_handle_, message, wparam, lparam);
}

void FlutterWindow::Destroy() {
    if (window_channel_) {
        window_channel_ = nullptr;
    }
    if (flutter_controller_) {
        flutter_controller_ = nullptr;
    }
    if (window_handle_) {
        DestroyWindow(window_handle_);
        window_handle_ = nullptr;
    }
}

FlutterWindow::~FlutterWindow() {
    if (window_handle_) {
        std::cout << "window_handle leak." << std::endl;
    }
    UnregisterWindowClass();
}

void DesktopMultiWindowSetWindowCreatedCallback(
    WindowCreatedCallback callback) {
    _g_window_created_callback = callback;
}