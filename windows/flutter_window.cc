//
// Created by yangbin on 2022/1/11.
//

#include "flutter_window.h"

#include "flutter_windows.h"

#include "tchar.h"

#include <iostream>
#include <utility>

#include <windows.h>
#include <memory>
#include <algorithm>  // std::min 사용을 위해 추가
#include <string>     // std::to_string 사용을 위해 추가

#include "include/desktop_multi_window/desktop_multi_window_plugin.h"
#include "multi_window_plugin_internal.h"


POINT lastCursorPos;
bool isDragging = false;

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
    window_class.hIcon =
        LoadIcon(window_class.hInstance, IDI_APPLICATION);
    window_class.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
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

}

LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);

      // 이미지 로드 및 출력 (예제 - 리소스나 파일에서 로드하는 방법 필요)
      HBITMAP hBitmap = static_cast<HBITMAP>(LoadImage(
          NULL, L"C:\\ARPI\\btn.bmp", IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE));
      if (hBitmap) {
        HDC hMemDC = CreateCompatibleDC(hdc);
        SelectObject(hMemDC, hBitmap);
        BitBlt(hdc, 100, 100, 300, 300, hMemDC, 0, 0, SRCCOPY);
        DeleteDC(hMemDC);
        DeleteObject(hBitmap);
      }

      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR: {
        // 마우스를 창에 올릴 때 커서를 손 모양으로 변경
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        // 마우스 왼쪽 버튼을 눌렀을 때 드래그 시작
        isDragging = true;
        SetCapture(hwnd);  // 마우스 이동 추적 시작
        GetCursorPos(&lastCursorPos);  // 현재 마우스 위치 저장
        return 0;
    }
    case WM_MOUSEMOVE: {
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
        return 0;
    }
    case WM_LBUTTONUP: {
        // 마우스 왼쪽 버튼을 떼었을 때 드래그 종료
        isDragging = false;
        ReleaseCapture();
        return 0;
    }
    default:
      return DefWindowProc(hwnd, message, wparam, lparam);
  }
}

FlutterWindow::FlutterWindow(
    int64_t id,
    std::string args,
    const std::shared_ptr<FlutterWindowCallback> &callback
) : callback_(callback), id_(id), window_handle_(nullptr), scale_factor_(1) {
  RegisterWindowClass(CustomWndProc);

  const POINT target_point = {static_cast<LONG>(10), static_cast<LONG>(10)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);
  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  scale_factor_ = dpi / 96.0;

  HWND window_handle = CreateWindow(
      kFlutterWindowClassName, L"", WS_POPUPWINDOW | WS_VISIBLE,
      Scale(target_point.x, scale_factor_), 
      Scale(target_point.y, scale_factor_),
      Scale(1280, scale_factor_), 
      Scale(720, scale_factor_),
      nullptr, nullptr, GetModuleHandle(nullptr), this);

  int width_scaled = Scale(500, scale_factor_);
  int height_scaled = Scale(500, scale_factor_);
  int diameter = min(width_scaled, height_scaled);
  HRGN hRgn = CreateEllipticRgn(0, 0, diameter, diameter);
  SetWindowRgn(window_handle, hRgn, TRUE);

  RECT frame;
  GetClientRect(window_handle, &frame);
  flutter::DartProject project(L"data");

  project.set_dart_entrypoint_arguments({"multi_window", std::to_string(id), std::move(args)});

  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project);

  if (!flutter_controller_->engine()) {
    std::cerr << "Failed to setup FlutterViewController." << std::endl;
  }
  // auto view_handle = flutter_controller_->view()->GetNativeWindow();
  // SetParent(view_handle, window_handle);
  // MoveWindow(view_handle, 0, 0, frame.right - frame.left, frame.bottom - frame.top, true);

  InternalMultiWindowPluginRegisterWithRegistrar(
      flutter_controller_->engine()->GetRegistrarForPlugin("DesktopMultiWindowPlugin"));
  window_channel_ = WindowChannel::RegisterWithRegistrar(
      flutter_controller_->engine()->GetRegistrarForPlugin("DesktopMultiWindowPlugin"), id_);

  if (_g_window_created_callback) {
    _g_window_created_callback(flutter_controller_.get());
  }

  ShowWindow(window_handle, SW_SHOW);
}

// static
FlutterWindow *FlutterWindow::GetThisFromHandle(HWND window) noexcept {
  return reinterpret_cast<FlutterWindow *>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

// static
LRESULT CALLBACK FlutterWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT *>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

    auto that = static_cast<FlutterWindow *>(window_struct->lpCreateParams);
    EnableFullDpiSupportIfAvailable(window);
    that->window_handle_ = window;
  } else if (FlutterWindow *that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

LRESULT FlutterWindow::MessageHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {

  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result = flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
    if (result) {
      return *result;
    }
  }

  auto child_content_ = flutter_controller_ ? flutter_controller_->view()->GetNativeWindow() : nullptr;

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

      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

      return 0;
    }
    case WM_SIZE: {
      RECT rect;
      GetClientRect(window_handle_, &rect);
      if (child_content_ != nullptr) {
        // Size and position the child window.
        MoveWindow(child_content_, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
      }
      return 0;
    }

    case WM_ACTIVATE: {
      if (child_content_ != nullptr) {
        SetFocus(child_content_);
      }
      return 0;
    }
    default: break;
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

void DesktopMultiWindowSetWindowCreatedCallback(WindowCreatedCallback callback) {
  _g_window_created_callback = callback;
}