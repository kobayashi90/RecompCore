// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/SavestateLayout.h"
#include "Core/State.h"
#include "Core/System.h"

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
#include <climits>
#include <dwmapi.h>
#include <thread>

#include "VideoCommon/Present.h"
// The F1 settings menu claims its keys through the host hotkey hook, and the
// OSD ImGui context only accepts XKB keysyms -- the same two headers the
// Wayland/X11 platforms use. The keysym header is vendored (VideoCommon/)
// because mingw-w64 ships no xkbcommon package.
#include "VideoCommon/xkbcommon-keysyms.h"
#include "moderngekko/hotkey.hpp"
#include "resource.h"

namespace
{
// Menu command ids. Load State entries are allocated a contiguous range,
// since the list is rebuilt from disk each time the menu opens.
constexpr UINT ID_SAVE_STATE = 41001;
constexpr UINT ID_PAUSE = 41002;
constexpr UINT ID_MUTE = 41003;
constexpr UINT ID_FULLSCREEN = 41004;
constexpr UINT ID_LOAD_STATE_FIRST = 41100;
constexpr UINT ID_LOAD_STATE_LAST = 41199;

// Hold-to-fast-forward target. 2x is fast enough to skip a cutscene without
// outrunning what most hosts can actually emulate.
constexpr float FAST_FORWARD_SPEED = 2.0f;

class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  bool CreateMenus();
  void RefreshMenu(HMENU menu);
  void SaveStateToStatesDirectory();
  void ToggleFullscreen();
  void UpdateWindowPosition();
  void ProcessEvents();

  HWND m_hwnd{};
  HMENU m_menu{};
  HMENU m_file_menu{};
  HMENU m_load_menu{};
  HMENU m_view_menu{};
  // Parallel to the Load State menu entries, rebuilt whenever it opens.
  std::vector<std::string> m_load_state_paths;
  std::time_t m_last_save_time{};
  std::size_t m_save_sequence{};
  bool m_fullscreen = false;
  LONG m_windowed_style = 0;
  RECT m_windowed_rect{};
  // Global button mask, exactly like the X11/Wayland backends keep one: the
  // OSD ImGui menu needs press/release state, and a per-event toggle would
  // release a held button on the next unrelated event.
  unsigned int m_pointer_buttons = 0;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(nullptr, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(nullptr, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::CreateMenus()
{
  m_menu = CreateMenu();
  m_file_menu = CreatePopupMenu();
  m_load_menu = CreatePopupMenu();
  m_view_menu = CreatePopupMenu();
  if (!m_menu || !m_file_menu || !m_load_menu || !m_view_menu)
    return false;

  // No F1 accelerator here: F1 opens the settings menu (claimed through the
  // hotkey hook below), so advertising it as Save State would be a lie. The
  // menu itself and the menu's States tab both still save.
  AppendMenuW(m_file_menu, MF_STRING, ID_SAVE_STATE, L"&Save State");
  AppendMenuW(m_file_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_load_menu), L"&Load State");
  AppendMenuW(m_file_menu, MF_SEPARATOR, 0, nullptr);
  // Checked state is refreshed from the core when the menu opens, so it cannot
  // drift out of step with an emulation that was paused some other way.
  AppendMenuW(m_file_menu, MF_STRING, ID_PAUSE, L"&Pause");

  AppendMenuW(m_view_menu, MF_STRING, ID_FULLSCREEN, L"&Fullscreen\tAlt+Enter");
  AppendMenuW(m_view_menu, MF_STRING, ID_MUTE, L"&Mute Audio");

  AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_file_menu), L"&File");
  AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_view_menu), L"&View");
  return SetMenu(m_hwnd, m_menu) != FALSE;
}

// Rebuilt on open rather than cached: states are written by this process while
// the menu is closed, and by the launcher between sessions.
void PlatformWin32::RefreshMenu(const HMENU menu)
{
  if (menu == m_file_menu)
  {
    auto& system = Core::System::GetInstance();
    const bool paused = Core::GetState(system) == Core::State::Paused;
    CheckMenuItem(m_file_menu, ID_PAUSE, MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
    return;
  }

  if (menu == m_view_menu)
  {
    CheckMenuItem(m_view_menu, ID_MUTE,
                  MF_BYCOMMAND |
                      (Config::Get(Config::MAIN_AUDIO_MUTED) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_view_menu, ID_FULLSCREEN,
                  MF_BYCOMMAND | (m_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    return;
  }

  if (menu != m_load_menu)
    return;

  while (DeleteMenu(m_load_menu, 0, MF_BYPOSITION))
  {
  }
  m_load_state_paths.clear();

  // Location, extension and order all come from State::Layout, so this menu and
  // any frontend listing the same directory cannot disagree.
  const std::vector<std::filesystem::path> states =
      State::Layout::List(StringToPath(File::GetUserPath(D_STATESAVES_IDX)));

  if (states.empty())
  {
    AppendMenuW(m_load_menu, MF_STRING | MF_GRAYED, 0, L"(no savestates)");
    return;
  }

  const std::size_t limit = std::min<std::size_t>(
      states.size(), ID_LOAD_STATE_LAST - ID_LOAD_STATE_FIRST + 1);
  for (std::size_t i = 0; i < limit; ++i)
  {
    AppendMenuW(m_load_menu, MF_STRING, ID_LOAD_STATE_FIRST + i,
                states[i].filename().wstring().c_str());
    m_load_state_paths.push_back(PathToString(states[i]));
  }
}

void PlatformWin32::SaveStateToStatesDirectory()
{
  const std::string directory = File::GetUserPath(D_STATESAVES_IDX);
  File::CreateFullPath(directory);
  const std::time_t now = std::time(nullptr);
  if (now == m_last_save_time)
    ++m_save_sequence;
  else
  {
    m_last_save_time = now;
    m_save_sequence = 0;
  }

  const std::filesystem::path path =
      StringToPath(directory) /
      State::Layout::TimestampedName(now, m_save_sequence, State::Layout::MANUAL_PREFIX);
  State::SaveAs(Core::System::GetInstance(), PathToString(path));
}

void PlatformWin32::ToggleFullscreen()
{
  if (!m_fullscreen)
  {
    GetWindowRect(m_hwnd, &m_windowed_rect);
    m_windowed_style = GetWindowLong(m_hwnd, GWL_STYLE);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
      return;

    SetMenu(m_hwnd, nullptr);
    SetWindowLong(m_hwnd, GWL_STYLE, m_windowed_style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(m_hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    m_fullscreen = true;
    return;
  }

  SetWindowLong(m_hwnd, GWL_STYLE, m_windowed_style);
  SetMenu(m_hwnd, m_menu);
  SetWindowPos(m_hwnd, nullptr, m_windowed_rect.left, m_windowed_rect.top,
               m_windowed_rect.right - m_windowed_rect.left,
               m_windowed_rect.bottom - m_windowed_rect.top,
               SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOZORDER);
  m_fullscreen = false;
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow() || !CreateMenus())
    return false;

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

// Native virtual key -> XKB keysym, the one currency the OSD ImGui context
// accepts (OnScreenUI::SetKeysym) and the F1 menu's hotkey hook claims
// (NotifyHotkey wants the same values on every host). Covers what the menu
// navigates with: editing keys, arrows, F-keys, modifiers, the character keys
// and the numpad. VK letter/digit codes are layout-logical, which is exactly
// what XKB keysyms are; the OEM punctuation row is the US layout, harmless
// because those keys only reach the menu as ImGui punctuation -- the menu has
// no text input fields.
unsigned KeysymFromVk(const WPARAM vk)
{
  if (vk >= VK_F1 && vk <= VK_F12)
    return XKB_KEY_F1 + (vk - VK_F1);
  if (vk >= 'A' && vk <= 'Z')
    return XKB_KEY_a + (vk - 'A');
  if (vk >= '0' && vk <= '9')
    return XKB_KEY_0 + (vk - '0');
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    return XKB_KEY_KP_0 + (vk - VK_NUMPAD0);
  switch (vk)
  {
  case VK_ESCAPE:
    return XKB_KEY_Escape;
  case VK_RETURN:
    return XKB_KEY_Return;
  case VK_TAB:
    return XKB_KEY_Tab;
  case VK_BACK:
    return XKB_KEY_BackSpace;
  case VK_DELETE:
    return XKB_KEY_Delete;
  case VK_INSERT:
    return XKB_KEY_Insert;
  case VK_HOME:
    return XKB_KEY_Home;
  case VK_END:
    return XKB_KEY_End;
  case VK_PRIOR:
    return XKB_KEY_Page_Up;
  case VK_NEXT:
    return XKB_KEY_Page_Down;
  case VK_LEFT:
    return XKB_KEY_Left;
  case VK_UP:
    return XKB_KEY_Up;
  case VK_RIGHT:
    return XKB_KEY_Right;
  case VK_DOWN:
    return XKB_KEY_Down;
  case VK_SPACE:
    return XKB_KEY_space;
  case VK_SHIFT:
    return XKB_KEY_Shift_L;
  case VK_RSHIFT:
    return XKB_KEY_Shift_R;
  case VK_CONTROL:
    return XKB_KEY_Control_L;
  case VK_RCONTROL:
    return XKB_KEY_Control_R;
  case VK_MENU:
    return XKB_KEY_Alt_L;
  case VK_RMENU:
    return XKB_KEY_Alt_R;
  case VK_LWIN:
    return XKB_KEY_Super_L;
  case VK_RWIN:
    return XKB_KEY_Super_R;
  case VK_CAPITAL:
    return XKB_KEY_Caps_Lock;
  case VK_NUMLOCK:
    return XKB_KEY_Num_Lock;
  case VK_SNAPSHOT:
    return XKB_KEY_Print;
  case VK_PAUSE:
    return XKB_KEY_Pause;
  case VK_ADD:
    return XKB_KEY_KP_Add;
  case VK_SUBTRACT:
    return XKB_KEY_KP_Subtract;
  case VK_MULTIPLY:
    return XKB_KEY_KP_Multiply;
  case VK_DIVIDE:
    return XKB_KEY_KP_Divide;
  case VK_DECIMAL:
    return XKB_KEY_KP_Decimal;
  case VK_OEM_MINUS:
    return XKB_KEY_minus;
  case VK_OEM_PLUS:
    return XKB_KEY_equal;
  case VK_OEM_1:
    return XKB_KEY_semicolon;
  case VK_OEM_2:
    return XKB_KEY_slash;
  case VK_OEM_3:
    return XKB_KEY_grave;
  case VK_OEM_4:
    return XKB_KEY_bracketleft;
  case VK_OEM_5:
    return XKB_KEY_backslash;
  case VK_OEM_6:
    return XKB_KEY_bracketright;
  case VK_OEM_7:
    return XKB_KEY_apostrophe;
  default:
    return 0;
  }
}

// Feeds one key transition into the OSD ImGui context (the F1 settings menu
// draws inside the game window through it). Returns the keysym so the caller
// can run the host hotkey hook on it as well.
unsigned FeedKeyToPresenter(const WPARAM vk, const bool down)
{
  const unsigned keysym = KeysymFromVk(vk);
  if (keysym && g_presenter)
    g_presenter->SetKeysym(keysym, down);
  return keysym;
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  {
    const bool repeat = (static_cast<ULONG_PTR>(lParam) & (1u << 30)) != 0;
    // Feed the OSD ImGui context first, exactly like the Wayland backend:
    // the F1 settings menu draws inside the game window through it, and it
    // needs both presses and (from WM_KEYUP/WM_SYSKEYUP) releases.
    const unsigned keysym = FeedKeyToPresenter(wParam, true);
    // The menu gets first refusal on keys like F1 so it can claim them before
    // the platform's own hotkeys below. Never on auto-repeat: holding F1 down
    // would otherwise toggle the menu every few milliseconds.
    if (keysym && !repeat && moderngekko::NotifyHotkey(keysym))
      return 0;
    // Auto-repeat must not re-run the platform hotkeys either: holding ESC
    // in fullscreen would flip it on and off, and with the menu just closed
    // by the first press it would quit the game. A repeated system key (held
    // F10, held Alt) still goes to DefWindowProc as Windows expects.
    if (repeat)
    {
      if (msg == WM_SYSKEYDOWN)
        return DefWindowProc(hwnd, msg, wParam, lParam);
      break;
    }
    // Alt combos belong to Windows (Alt+F4 closes, Alt+Space opens the system
    // menu, Alt+F1..F12 do nothing) -- without this the F2..F8 state slots
    // would swallow Alt+F4 as "load slot 4". Alt+Enter is the one exception,
    // and it arrives as a system key precisely because Alt is held.
    if (msg == WM_SYSKEYDOWN && (GetKeyState(VK_MENU) & 0x8000) != 0)
    {
      if (wParam == VK_RETURN)
      {
        platform->ToggleFullscreen();
        return 0;
      }
      return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    switch (wParam)
    {
    case VK_F1:
      // Fallback only: with the settings menu registered (which it always is
      // in the runner) F1 is claimed above, and the File menu / States tab
      // cover saving. Kept for a future platform without the menu.
      platform->SaveStateToStatesDirectory();
      return 0;
    case VK_F2:
    case VK_F3:
    case VK_F4:
    case VK_F5:
    case VK_F6:
    case VK_F7:
    case VK_F8:
    {
      // Same slots as the X11/Wayland backends: F2..F8 load slots 2..8,
      // Shift+F2..F8 save. (F1 itself is the menu on both, so slot 1 is
      // reached through the menu's States tab.)
      const int slot = static_cast<int>(wParam - VK_F2) + 2;
      auto& system = Core::System::GetInstance();
      if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        State::Save(system, slot);
      else
        State::Load(system, slot);
      return 0;
    }
    case VK_F9:
      Core::SaveScreenShot();
      return 0;
    case VK_F10:
    {
      // Windows generates WM_SYSKEYDOWN for F10 (it is the menu-bar key), so
      // this case has to live in the shared switch below the system-message
      // branch -- pause, as on X11/Wayland, instead of DefWindowProc beeping
      // at the menu bar.
      auto& system = Core::System::GetInstance();
      const bool running = Core::GetState(system) == Core::State::Running;
      Core::SetState(system, running ? Core::State::Paused : Core::State::Running);
      return 0;
    }
    case VK_F11:
      // Deliberately fullscreen, not "load last saved" as on X11/Wayland:
      // F11 has always been the Windows fullscreen key here, and Alt+Enter
      // works on both platforms anyway.
      platform->ToggleFullscreen();
      return 0;
    case VK_F12:
    {
      auto& system = Core::System::GetInstance();
      if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        State::UndoLoadState(system);
      else
        State::UndoSaveState(system);
      return 0;
    }
    case VK_SPACE:
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, FAST_FORWARD_SPEED);
      return 0;
    case VK_ESCAPE:
      // Plain ESC closes the fullscreen window first, then quits -- but only
      // while no menu is open: the settings menu claims ESC when visible
      // (ConfigMenu::HotkeyThunk) so closing it cannot also quit the game.
      if (platform->m_fullscreen)
        platform->ToggleFullscreen();
      else
        platform->RequestShutdown();
      return 0;
    default:
      break;
    }
    // Anything else that came in as a system message (Alt+letter and friends)
    // keeps its default Windows behaviour.
    if (msg == WM_SYSKEYDOWN)
      return DefWindowProc(hwnd, msg, wParam, lParam);
    break;
  }

  case WM_KEYUP:
  case WM_SYSKEYUP:
  {
    // Releases must reach the menu too, or a key it saw go down stays stuck
    // down inside ImGui after the menu closes.
    FeedKeyToPresenter(wParam, false);
    if (msg == WM_KEYUP)
    {
      if (wParam == VK_SPACE)
      {
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
        return 0;
      }
      break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_MOUSEMOVE:
  {
    if (!g_presenter)
      break;
    // Window coordinates -> backbuffer pixels, the space the OSD ImGui
    // context draws in (HiDPI: the two differ), as on X11/Wayland.
    const float scale = g_presenter->GetBackbufferScale();
    g_presenter->SetMousePos(static_cast<float>(static_cast<short>(LOWORD(lParam))) * scale,
                             static_cast<float>(static_cast<short>(HIWORD(lParam))) * scale);
    break;
  }

  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  {
    if (!platform || !g_presenter)
      break;
    // Same bits as the X11/Wayland backends: left 1<<0, right 1<<1, middle 1<<2.
    unsigned int bit = 0;
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      bit = 1u << 0;
      break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      bit = 1u << 1;
      break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      bit = 1u << 2;
      break;
    default:
      break;
    }
    // Clicks carry client coordinates as well: a click without a prior move
    // (remote desktop, warping) would otherwise land on a stale position.
    const float scale = g_presenter->GetBackbufferScale();
    g_presenter->SetMousePos(static_cast<float>(static_cast<short>(LOWORD(lParam))) * scale,
                             static_cast<float>(static_cast<short>(HIWORD(lParam))) * scale);
    const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
    if (down)
      platform->m_pointer_buttons |= bit;
    else
      platform->m_pointer_buttons &= ~bit;
    g_presenter->SetMousePress(platform->m_pointer_buttons);
    break;
  }

  case WM_MOUSEWHEEL:
  {
    if (!g_presenter)
      break;
    // One notch = +/-1.0 like X11's Button4/Button5; Windows reports the
    // distance in WHEEL_DELTA units, positive = scrolled forward (up).
    const float wheel =
        static_cast<float>(static_cast<short>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA);
    if (wheel != 0.0f)
      g_presenter->SetMouseWheel(0.0f, wheel);
    break;
  }

  case WM_KILLFOCUS:
    // Never leave emulation running fast because Space was released while
    // another window had focus and the key-up went elsewhere.
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
    break;

  case WM_INITMENUPOPUP:
    if (platform)
      platform->RefreshMenu(reinterpret_cast<HMENU>(wParam));
    break;

  case WM_COMMAND:
  {
    if (!platform)
      break;
    const UINT command = LOWORD(wParam);
    auto& system = Core::System::GetInstance();
    if (command == ID_SAVE_STATE)
    {
      platform->SaveStateToStatesDirectory();
    }
    else if (command == ID_PAUSE)
    {
      const bool paused = Core::GetState(system) == Core::State::Paused;
      Core::SetState(system, paused ? Core::State::Running : Core::State::Paused);
    }
    else if (command == ID_MUTE)
    {
      Config::SetCurrent(Config::MAIN_AUDIO_MUTED, !Config::Get(Config::MAIN_AUDIO_MUTED));
    }
    else if (command == ID_FULLSCREEN)
    {
      platform->ToggleFullscreen();
    }
    else if (command >= ID_LOAD_STATE_FIRST && command <= ID_LOAD_STATE_LAST)
    {
      const std::size_t index = command - ID_LOAD_STATE_FIRST;
      if (index < platform->m_load_state_paths.size())
        State::LoadAs(system, platform->m_load_state_paths[index]);
    }
    break;
  }

  case WM_CLOSE:
    platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
