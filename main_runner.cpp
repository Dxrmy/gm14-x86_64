// gm14 interactive PC runner (Win32)
// Native Windows desktop frontend for gm14-x86_64 GameMaker 1.4 runtime.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include "vm.hpp"
#include "dw.hpp"
#include "audio.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <iostream>
#include <ctime>
#include <sstream>

namespace {

// Runner configuration
struct RunnerConfig {
    std::string data_win_path;
    std::string start_room_str;
    int scale = 2;               // 1 = 1x, 2 = 2x, etc.
    double override_fps = 0.0;   // 0 = room speed
    int headless_frames = -1;    // > 0 = headless mode
    double timeout_seconds = 0.0;// > 0 = exit after timeout
    std::string dump_frame_path;
    std::string save_dir = "./saves";
    bool fresh_save = false;
    std::string record_path;
    std::string replay_path;
    bool verbose = true;
};

// Global state
HWND g_hwnd = nullptr;
bool g_running = true;
int g_client_w = 640;
int g_client_h = 480;
gm14::Image g_current_frame;
std::vector<uint32_t> g_bgra_buffer;

// Input tracking
std::set<int> g_keys_held;
std::set<int> g_keys_pressed;
std::set<int> g_keys_released;
int g_mouse_raw_x = 0;
int g_mouse_raw_y = 0;
std::set<int> g_mouse_buttons_held;
std::set<int> g_mouse_buttons_pressed;
std::set<int> g_mouse_buttons_released;
int g_mouse_wheel_delta = 0;
std::map<int, int> g_key_remap;

// Debug control flags
int g_debug_skip_room = 0;
bool g_debug_unfreeze = false;
bool g_debug_restart_room = false;
bool g_debug_dump_info = false;
bool g_debug_toggle_record = false;
double g_speed_modifier = 1.0;

struct InputRecorder {
    bool recording = false;
    std::string filename;
    FILE* fp = nullptr;
    int frames_recorded = 0;

    void start(const std::string& path) {
        stop();
        filename = path;
        fp = std::fopen(filename.c_str(), "w");
        if (fp) {
            recording = true;
            frames_recorded = 0;
            std::fprintf(fp, "# gm14 recording v1\n");
            std::fflush(fp);
            std::printf("\n=======================================================\n");
            std::printf("[recorder] *** RECORDING STARTED *** -> %s\n", filename.c_str());
            std::printf("[recorder] Press F9 at any time to finish and save.\n");
            std::printf("=======================================================\n\n");
            std::fflush(stdout);
        } else {
            std::fprintf(stderr, "[recorder] Failed to open %s for recording\n", path.c_str());
        }
    }

    void record_frame(int frame,
                      const std::set<int>& held,
                      const std::set<int>& pressed,
                      const std::set<int>& released,
                      int mx, int my,
                      const std::set<int>& mb_held,
                      const std::set<int>& mb_pressed,
                      const std::set<int>& mb_released,
                      int wheel,
                      int room) {
        if (!recording || !fp) return;
        frames_recorded++;
        std::fprintf(fp, "F %d R:%d", frame, room);
        if (!held.empty()) {
            std::fprintf(fp, " H:");
            for (int k : held) std::fprintf(fp, "%d,", k);
        }
        if (!pressed.empty()) {
            std::fprintf(fp, " P:");
            for (int k : pressed) std::fprintf(fp, "%d,", k);
        }
        if (!released.empty()) {
            std::fprintf(fp, " Rel:");
            for (int k : released) std::fprintf(fp, "%d,", k);
        }
        std::fprintf(fp, " M:%d,%d", mx, my);
        if (!mb_held.empty()) {
            std::fprintf(fp, " MBH:");
            for (int b : mb_held) std::fprintf(fp, "%d,", b);
        }
        if (!mb_pressed.empty()) {
            std::fprintf(fp, " MBP:");
            for (int b : mb_pressed) std::fprintf(fp, "%d,", b);
        }
        if (!mb_released.empty()) {
            std::fprintf(fp, " MBR:");
            for (int b : mb_released) std::fprintf(fp, "%d,", b);
        }
        if (wheel != 0) {
            std::fprintf(fp, " W:%d", wheel);
        }
        std::fprintf(fp, "\n");
        if (frames_recorded % 30 == 0) std::fflush(fp);
    }

    void stop() {
        if (recording && fp) {
            std::fclose(fp);
            fp = nullptr;
            recording = false;
            std::printf("\n=======================================================\n");
            std::printf("[recorder] *** RECORDING SAVED ***\n");
            std::printf("[recorder] File: %s (%d frames)\n", filename.c_str(), frames_recorded);
            std::printf("=======================================================\n\n");
            std::fflush(stdout);
        }
    }
} g_recorder;

struct RecordedFrame {
    std::set<int> held, pressed, released;
    int mx = 0, my = 0;
    std::set<int> mb_held, mb_pressed, mb_released;
    int wheel = 0;
    int room = -1;
};

struct InputReplayer {
    bool replaying = false;
    std::map<int, RecordedFrame> frames;
    int max_frame = 0;

    bool load(const std::string& path) {
        frames.clear();
        FILE* fp = std::fopen(path.c_str(), "r");
        if (!fp) {
            std::fprintf(stderr, "[replayer] Failed to open %s\n", path.c_str());
            return false;
        }
        char line[1024];
        while (std::fgets(line, sizeof(line), fp)) {
            if (line[0] != 'F') continue;
            RecordedFrame rf;
            int fnum = 0;
            char* p = line + 1;
            fnum = std::strtol(p, &p, 10);
            while (*p) {
                while (*p == ' ') p++;
                if (strncmp(p, "R:", 2) == 0) {
                    p += 2; rf.room = std::strtol(p, &p, 10);
                } else if (strncmp(p, "H:", 2) == 0) {
                    p += 2;
                    while (*p >= '0' && *p <= '9') {
                        rf.held.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "P:", 2) == 0) {
                    p += 2;
                    while (*p >= '0' && *p <= '9') {
                        rf.pressed.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "Rel:", 4) == 0) {
                    p += 4;
                    while (*p >= '0' && *p <= '9') {
                        rf.released.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "M:", 2) == 0) {
                    p += 2;
                    rf.mx = std::strtol(p, &p, 10);
                    if (*p == ',') p++;
                    rf.my = std::strtol(p, &p, 10);
                } else if (strncmp(p, "MBH:", 4) == 0) {
                    p += 4;
                    while (*p >= '0' && *p <= '9') {
                        rf.mb_held.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "MBP:", 4) == 0) {
                    p += 4;
                    while (*p >= '0' && *p <= '9') {
                        rf.mb_pressed.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "MBR:", 4) == 0) {
                    p += 4;
                    while (*p >= '0' && *p <= '9') {
                        rf.mb_released.insert(std::strtol(p, &p, 10));
                        if (*p == ',') p++;
                    }
                } else if (strncmp(p, "W:", 2) == 0) {
                    p += 2;
                    rf.wheel = std::strtol(p, &p, 10);
                } else {
                    p++;
                }
            }
            frames[fnum] = rf;
            if (fnum > max_frame) max_frame = fnum;
        }
        std::fclose(fp);
        replaying = true;
        std::printf("[replayer] Loaded recording %s: %zu frames (up to frame %d)\n",
                    path.c_str(), frames.size(), max_frame);
        std::fflush(stdout);
        return true;
    }

    bool step(int frame, gm14::Runtime& rt) {
        if (!replaying) return false;
        auto it = frames.find(frame);
        if (it != frames.end()) {
            rt.keys_held = it->second.held;
            rt.keys_pressed = it->second.pressed;
            rt.keys_released = it->second.released;
            rt.mouse_x = it->second.mx;
            rt.mouse_y = it->second.my;
            rt.mouse_buttons_held = it->second.mb_held;
            rt.mouse_buttons_pressed = it->second.mb_pressed;
            rt.mouse_buttons_released = it->second.mb_released;
            rt.mouse_wheel_delta = it->second.wheel;
        } else {
            rt.keys_held.clear();
            rt.keys_pressed.clear();
            rt.keys_released.clear();
            rt.mouse_buttons_held.clear();
            rt.mouse_buttons_pressed.clear();
            rt.mouse_buttons_released.clear();
            rt.mouse_wheel_delta = 0;
        }
        if (frame >= max_frame) {
            std::printf("[replayer] Replay finished at frame %d.\n", frame);
            std::fflush(stdout);
            replaying = false;
        }
        return true;
    }
} g_replayer;

// Fullscreen toggle state
bool g_is_fullscreen = false;
RECT g_saved_window_rect = {};
DWORD g_saved_window_style = 0;

// Map Windows Virtual-Key code to GameMaker 1.4 key code
int map_vk_to_gm(WPARAM vk) {
    if (g_key_remap.count((int)vk)) return g_key_remap[(int)vk];
    switch (vk) {
        case VK_LEFT:     return 37; // vk_left
        case VK_UP:       return 38; // vk_up
        case VK_RIGHT:    return 39; // vk_right
        case VK_DOWN:     return 40; // vk_down
        case VK_RETURN:   return 13; // vk_enter
        case VK_ESCAPE:   return 27; // vk_escape
        case VK_SPACE:    return 32; // vk_space
        case VK_SHIFT:    return 16; // vk_shift
        case VK_CONTROL:  return 17; // vk_control
        case VK_MENU:     return 18; // vk_alt
        case VK_BACK:     return 8;  // vk_backspace
        case VK_TAB:      return 9;  // vk_tab
        case VK_PAUSE:    return 19; // vk_pause
        case VK_PRIOR:    return 33; // vk_pageup
        case VK_NEXT:     return 34; // vk_pagedown
        case VK_END:      return 35; // vk_end
        case VK_HOME:     return 36; // vk_home
        case VK_INSERT:   return 45; // vk_insert
        case VK_DELETE:   return 46; // vk_delete
        case VK_NUMPAD0:  return 96;
        case VK_NUMPAD1:  return 97;
        case VK_NUMPAD2:  return 98;
        case VK_NUMPAD3:  return 99;
        case VK_NUMPAD4:  return 100;
        case VK_NUMPAD5:  return 101;
        case VK_NUMPAD6:  return 102;
        case VK_NUMPAD7:  return 103;
        case VK_NUMPAD8:  return 104;
        case VK_NUMPAD9:  return 105;
        case VK_MULTIPLY: return 106; // vk_multiply
        case VK_ADD:      return 107; // vk_add
        case VK_SUBTRACT: return 109; // vk_subtract
        case VK_DECIMAL:  return 110; // vk_decimal
        case VK_DIVIDE:   return 111; // vk_divide
        case VK_F1:       return 112;
        case VK_F2:       return 113;
        case VK_F3:       return 114;
        case VK_F4:       return 115;
        case VK_F5:       return 116;
        case VK_F6:       return 117;
        case VK_F7:       return 118;
        case VK_F8:       return 119;
        case VK_F9:       return 120;
        case VK_F10:      return 121;
        case VK_F11:      return 122;
        case VK_F12:      return 123;
        default:
            return (int)vk; // Letters 'A'-'Z' (65-90), Digits '0'-'9' (48-57)
    }
}

int parse_key_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "left") return VK_LEFT;
    if (s == "right") return VK_RIGHT;
    if (s == "up") return VK_UP;
    if (s == "down") return VK_DOWN;
    if (s == "enter" || s == "return") return VK_RETURN;
    if (s == "shift") return VK_SHIFT;
    if (s == "ctrl" || s == "control") return VK_CONTROL;
    if (s == "alt" || s == "menu") return VK_MENU;
    if (s == "space") return VK_SPACE;
    if (s == "esc" || s == "escape") return VK_ESCAPE;
    if (s == "tab") return VK_TAB;
    if (s == "backspace" || s == "back") return VK_BACK;
    if (s.size() == 1) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return (int)('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return (int)c;
    }
    return 0;
}

int parse_action_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "left" || s == "vk_left") return 37;
    if (s == "up" || s == "vk_up") return 38;
    if (s == "right" || s == "vk_right") return 39;
    if (s == "down" || s == "vk_down") return 40;
    if (s == "confirm" || s == "accept" || s == "z" || s == "enter" || s == "face1") return 90;
    if (s == "cancel" || s == "back" || s == "x" || s == "shift" || s == "face2") return 88;
    if (s == "menu" || s == "item" || s == "c" || s == "ctrl" || s == "face3") return 67;
    int k = parse_key_str(s);
    return map_vk_to_gm((WPARAM)k);
}

void parse_bind(const std::string& spec) {
    size_t eq = spec.find('=');
    if (eq == std::string::npos) return;
    std::string action = spec.substr(0, eq);
    std::string key = spec.substr(eq + 1);
    int vk = parse_key_str(key);
    int target_gm = parse_action_str(action);
    if (vk > 0 && target_gm > 0) {
        g_key_remap[vk] = target_gm;
        std::printf("[runner] Bound key 0x%X -> GM key %d (%s -> %s)\n", vk, target_gm, key.c_str(), action.c_str());
    }
}

void toggle_fullscreen(HWND hwnd) {
    if (!g_is_fullscreen) {
        GetWindowRect(hwnd, &g_saved_window_rect);
        g_saved_window_style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(hMon, &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, g_saved_window_style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_is_fullscreen = true;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, g_saved_window_style);
        SetWindowPos(hwnd, nullptr,
                     g_saved_window_rect.left, g_saved_window_rect.top,
                     g_saved_window_rect.right - g_saved_window_rect.left,
                     g_saved_window_rect.bottom - g_saved_window_rect.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_is_fullscreen = false;
    }
}

void render_frame(HDC hdc) {
    if (g_current_frame.w <= 0 || g_current_frame.h <= 0 || g_current_frame.rgba.empty()) {
        RECT rc = { 0, 0, g_client_w, g_client_h };
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }

    // Convert RGBA to BGRA for GDI StretchDIBits
    size_t num_pixels = (size_t)g_current_frame.w * (size_t)g_current_frame.h;
    if (g_bgra_buffer.size() < num_pixels) {
        g_bgra_buffer.resize(num_pixels);
    }
    const uint32_t* src = reinterpret_cast<const uint32_t*>(g_current_frame.rgba.data());
    uint32_t* dst = g_bgra_buffer.data();
    for (size_t i = 0; i < num_pixels; ++i) {
        uint32_t c = src[i];
        // src is RGBA: (A << 24) | (B << 16) | (G << 8) | R
        // dst is BGRA: (A << 24) | (R << 16) | (G << 8) | B
        dst[i] = (c & 0xFF00FF00) | ((c & 0x000000FF) << 16) | ((c & 0x00FF0000) >> 16);
    }

    // Calculate aspect-ratio preserved destination rectangle inside client area
    double scale_x = (double)g_client_w / (double)g_current_frame.w;
    double scale_y = (double)g_client_h / (double)g_current_frame.h;
    double scale = std::min(scale_x, scale_y);
    int dst_w = (int)(g_current_frame.w * scale);
    int dst_h = (int)(g_current_frame.h * scale);
    int dst_x = (g_client_w - dst_w) / 2;
    int dst_y = (g_client_h - dst_h) / 2;

    // Clear letterbox / pillarbox bars with black
    if (dst_x > 0) {
        RECT r_left = { 0, 0, dst_x, g_client_h };
        RECT r_right = { dst_x + dst_w, 0, g_client_w, g_client_h };
        FillRect(hdc, &r_left, (HBRUSH)GetStockObject(BLACK_BRUSH));
        FillRect(hdc, &r_right, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    if (dst_y > 0) {
        RECT r_top = { 0, 0, g_client_w, dst_y };
        RECT r_bot = { 0, dst_y + dst_h, g_client_w, g_client_h };
        FillRect(hdc, &r_top, (HBRUSH)GetStockObject(BLACK_BRUSH));
        FillRect(hdc, &r_bot, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_current_frame.w;
    bmi.bmiHeader.biHeight = -g_current_frame.h; // Negative indicates top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, COLORONCOLOR); // Sharp nearest-neighbor pixel scaling
    StretchDIBits(hdc,
                  dst_x, dst_y, dst_w, dst_h,
                  0, 0, g_current_frame.w, g_current_frame.h,
                  dst, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            render_frame(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // Prevent flicker

        case WM_SIZE: {
            g_client_w = LOWORD(lParam);
            g_client_h = HIWORD(lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            // Debug hotkeys
            if (wParam == VK_NEXT) { // PageDown
                g_debug_skip_room = 1;
                return 0;
            }
            if (wParam == VK_PRIOR) { // PageUp
                g_debug_skip_room = -1;
                return 0;
            }
            if (wParam == VK_F1) {
                g_debug_unfreeze = true;
                return 0;
            }
            if (wParam == VK_F2) {
                g_debug_dump_info = true;
                return 0;
            }
            if (wParam == VK_HOME) {
                g_debug_restart_room = true;
                return 0;
            }
            if (wParam == VK_F9) {
                g_debug_toggle_record = true;
                return 0;
            }
            if (wParam == VK_OEM_6) { // ']' key: speed up
                g_speed_modifier += 0.5;
                std::printf("[speed] Game speed set to %.2fx\n", g_speed_modifier);
                return 0;
            }
            if (wParam == VK_OEM_4) { // '[' key: slow down
                g_speed_modifier = std::max(0.1, g_speed_modifier - 0.5);
                std::printf("[speed] Game speed set to %.2fx\n", g_speed_modifier);
                return 0;
            }
            if (wParam == VK_OEM_5) { // '\' key: reset speed
                g_speed_modifier = 1.0;
                std::printf("[speed] Game speed reset to 1.0x\n");
                return 0;
            }

            int gm_key = map_vk_to_gm(wParam);
            bool repeat = (lParam & (1 << 30)) != 0;
            if (!repeat || !g_keys_held.count(gm_key)) {
                g_keys_pressed.insert(gm_key);
            }
            g_keys_held.insert(gm_key);

            // Also map WASD to arrow keys for universal menu & directional navigation
            int extra_gm_key = 0;
            if (wParam == 'W' || wParam == 'w') extra_gm_key = 38; // vk_up
            else if (wParam == 'A' || wParam == 'a') extra_gm_key = 37; // vk_left
            else if (wParam == 'S' || wParam == 's') extra_gm_key = 40; // vk_down
            else if (wParam == 'D' || wParam == 'd') extra_gm_key = 39; // vk_right

            if (extra_gm_key != 0) {
                if (!repeat || !g_keys_held.count(extra_gm_key)) {
                    g_keys_pressed.insert(extra_gm_key);
                }
                g_keys_held.insert(extra_gm_key);
            }

            // F4 or Alt+Enter toggles fullscreen
            if (wParam == VK_F4 || (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
                toggle_fullscreen(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_KEYUP: {
            int gm_key = map_vk_to_gm(wParam);
            g_keys_held.erase(gm_key);
            g_keys_released.insert(gm_key);

            int extra_gm_key = 0;
            if (wParam == 'W' || wParam == 'w') extra_gm_key = 38;
            else if (wParam == 'A' || wParam == 'a') extra_gm_key = 37;
            else if (wParam == 'S' || wParam == 's') extra_gm_key = 40;
            else if (wParam == 'D' || wParam == 'd') extra_gm_key = 39;

            if (extra_gm_key != 0) {
                g_keys_held.erase(extra_gm_key);
                g_keys_released.insert(extra_gm_key);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.insert(1);
            g_mouse_buttons_pressed.insert(1);
            return 0;
        }

        case WM_LBUTTONUP: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.erase(1);
            g_mouse_buttons_released.insert(1);
            return 0;
        }

        case WM_RBUTTONDOWN: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.insert(2);
            g_mouse_buttons_pressed.insert(2);
            return 0;
        }

        case WM_RBUTTONUP: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.erase(2);
            g_mouse_buttons_released.insert(2);
            return 0;
        }

        case WM_MBUTTONDOWN: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.insert(3);
            g_mouse_buttons_pressed.insert(3);
            return 0;
        }

        case WM_MBUTTONUP: {
            g_mouse_raw_x = (short)LOWORD(lParam);
            g_mouse_raw_y = (short)HIWORD(lParam);
            g_mouse_buttons_held.erase(3);
            g_mouse_buttons_released.insert(3);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            short delta = (short)HIWORD(wParam);
            g_mouse_wheel_delta += (delta / WHEEL_DELTA);
            return 0;
        }

        case WM_KILLFOCUS: {
            for (int k : g_keys_held) {
                g_keys_released.insert(k);
            }
            g_keys_held.clear();
            for (int b : g_mouse_buttons_held) {
                g_mouse_buttons_released.insert(b);
            }
            g_mouse_buttons_held.clear();
            return 0;
        }

        case WM_CLOSE: {
            g_running = false;
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }
}

void print_usage(const char* prog) {
    std::printf("gm14_runner: Interactive GameMaker 1.4 Native PC Runner\n");
    std::printf("Usage: %s [data.win] [options]\n\n", prog);
    std::printf("Options:\n");
    std::printf("  --room <id|name>      Start directly in room by index or name\n");
    std::printf("  --scale <1|2|3|4>     Window scale factor (default: 2 -> 1280x960)\n");
    std::printf("  --fps <n>             Override target FPS (default: room speed)\n");
    std::printf("  --speed, -s <mult>    Game speed multiplier (e.g. 1.5, 2.0; default: 1.0)\n");
    std::printf("  --headless <frames>   Run N frames headless without opening a window\n");
    std::printf("  --timeout <seconds>   Run for N seconds then exit cleanly\n");
    std::printf("  --dump-frame <path>   Save final frame as PNG\n");
    std::printf("  --save-dir <path>     Directory for game save/INI files (default: ./saves)\n");
    std::printf("  --fresh               Start fresh: wipe save directory before running\n");
    std::printf("  --bind <action>=<key> Custom key mapping (e.g. up=w, confirm=space)\n");
    std::printf("  --record [path]       Record gameplay input to file (default: session_<timestamp>.rec)\n");
    std::printf("  --replay <path>       Replay recorded input frame-by-frame\n");
    std::printf("  --verbose, -v         Enable verbose diagnostic logging\n");
    std::printf("  --help, -h            Show this help text\n");
    std::printf("\nControls & Hotkeys:\n");
    std::printf("  Arrow Keys / WASD     Movement / Navigation (vk_left, vk_right, vk_up, vk_down)\n");
    std::printf("  Z / Enter             Confirm / Interact (gp_face1)\n");
    std::printf("  X / Shift             Cancel / Run (gp_face2)\n");
    std::printf("  C / Ctrl              Menu / Item (gp_face3)\n");
    std::printf("  [ / ]                 Decrease / Increase game speed (+-0.5x)\n");
    std::printf("  \\                     Reset game speed to 1.0x\n");
    std::printf("  F4 / Alt+Enter        Toggle Fullscreen\n");
    std::printf("  [PageDown]/[PageUp]   Skip room forward / backward\n");
    std::printf("  [Home]                Reload current room\n");
    std::printf("  [F1]                  Unfreeze player (reset global.interact = 0)\n");
    std::printf("  [F2]                  Dump room, player and instance state\n");
    std::printf("  [F9]                  Toggle real-time input recording\n");
    std::printf("  Esc                   Exit / Back\n");
}

} // namespace

int main(int argc, char** argv) {
    RunnerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--room" && i + 1 < argc) {
            cfg.start_room_str = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            cfg.scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fps" && i + 1 < argc) {
            cfg.override_fps = std::atof(argv[++i]);
        } else if ((arg == "--speed" || arg == "-s") && i + 1 < argc) {
            g_speed_modifier = std::max(0.05, std::atof(argv[++i]));
        } else if (arg == "--headless" && i + 1 < argc) {
            cfg.headless_frames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            cfg.timeout_seconds = std::atof(argv[++i]);
        } else if (arg == "--dump-frame" && i + 1 < argc) {
            cfg.dump_frame_path = argv[++i];
        } else if (arg == "--save-dir" && i + 1 < argc) {
            cfg.save_dir = argv[++i];
        } else if (arg == "--fresh") {
            cfg.fresh_save = true;
        } else if (arg == "--record") {
            if (i + 1 < argc && argv[i + 1][0] != '-') cfg.record_path = argv[++i];
            else {
                time_t now = time(nullptr);
                char tstr[64];
                strftime(tstr, sizeof(tstr), "session_%Y%m%d_%H%M%S.rec", localtime(&now));
                cfg.record_path = tstr;
            }
        } else if (arg == "--replay" && i + 1 < argc) {
            cfg.replay_path = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        } else if (arg == "--bind" && i + 1 < argc) {
            parse_bind(argv[++i]);
        } else if (arg[0] != '-' && cfg.data_win_path.empty()) {
            cfg.data_win_path = arg;
        }
    }

    // Default search paths for data.win if not explicitly passed
    if (cfg.data_win_path.empty()) {
        const char* candidates[] = {
            "data.win",
            "game.win",
            "game.unx",
            "game.ios",
            "game.droid"
        };
        for (const char* c : candidates) {
            DWORD attr = GetFileAttributesA(c);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                cfg.data_win_path = c;
                break;
            }
        }
    }

    if (cfg.data_win_path.empty()) {
        std::fprintf(stderr, "Error: No data.win file specified or found.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    try {
        std::printf("[loader] Loading data: %s\n", cfg.data_win_path.c_str());
        gm14::DataWin dw(cfg.data_win_path);

        std::string game_title = dw.display_name.empty()
                                     ? (dw.game_name.empty() ? "gm14 runner" : dw.game_name)
                                     : dw.display_name;

        std::printf("[game] %s (internal: %s, %zu rooms, %zu objects, %zu sprites)\n",
                    game_title.c_str(), dw.game_name.c_str(),
                    dw.rooms.size(), dw.objects.size(), dw.sprites.size());

        if (dw.code.empty()) {
            std::printf("[warn] No GML bytecode chunk found in data file (game was likely compiled with YYC to native code).\n");
        }

        gm14::EngineConfig eng_cfg = gm14::load_engine_config("gm14.ini");
        if (eng_cfg.target_fps > 0 && cfg.override_fps == 0.0) {
            cfg.override_fps = eng_cfg.target_fps;
        }
        if (eng_cfg.start_room >= 0 && cfg.start_room_str.empty()) {
            cfg.start_room_str = std::to_string(eng_cfg.start_room);
        }
        if (eng_cfg.save_directory != "./saves" && cfg.save_dir == "./saves") {
            cfg.save_dir = eng_cfg.save_directory;
        }

        gm14::Runtime rt(dw, eng_cfg);
        rt.save_dir = cfg.save_dir;
        if (!cfg.save_dir.empty()) {
            CreateDirectoryA(cfg.save_dir.c_str(), nullptr);
            if (cfg.fresh_save) {
                std::string search = cfg.save_dir + "/*";
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            std::string fpath = cfg.save_dir + "/" + fd.cFileName;
                            DeleteFileA(fpath.c_str());
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
                std::printf("[save] Cleaned save directory for fresh run: %s\n", cfg.save_dir.c_str());
            }
        }

        // Feed audio backend: attach WaveOutBackend to runtime audio engine
        rt.ensure_audio();
        if (rt.audio) {
            rt.audio->set_backend(gm14::make_waveout_backend());
            std::printf("[audio] WaveOut backend initialized (%d Hz, %d channels)\n",
                        rt.audio->sample_rate(), rt.audio->channels());
        }

        // Resolve starting room
        int target_room = -1;
        if (!cfg.start_room_str.empty()) {
            bool is_digit = true;
            for (char c : cfg.start_room_str) {
                if (!std::isdigit((unsigned char)c)) { is_digit = false; break; }
            }
            if (is_digit) {
                target_room = std::atoi(cfg.start_room_str.c_str());
            } else {
                for (size_t ri = 0; ri < dw.rooms.size(); ++ri) {
                    if (dw.rooms[ri].name == cfg.start_room_str) {
                        target_room = (int)ri;
                        break;
                    }
                }
            }
            if (target_room < 0 || target_room >= (int)dw.rooms.size()) {
                std::fprintf(stderr, "[warn] Room '%s' not found, using default.\n", cfg.start_room_str.c_str());
                target_room = -1;
            }
        }

        // Natural boot mode: let game initialization run naturally
        rt.fast_boot = false;

        // Boot into room 0 (runs initial persistent controllers and globals)
        rt.start_room(0);
        if (target_room >= 0 && target_room != 0) {
            std::printf("[room] Transitioning to room %d (%s)\n", target_room, dw.rooms[target_room].name.c_str());
            rt.change_room(target_room);
        }

        // Headless execution mode (ideal for automated testing / verification)
        if (cfg.headless_frames > 0) {
            std::printf("[runner] Executing %d headless frames...\n", cfg.headless_frames);
            for (int f = 0; f < cfg.headless_frames && rt.running; ++f) {
                rt.keys_held = g_keys_held;
                rt.keys_pressed = g_keys_pressed;
                rt.keys_released = g_keys_released;
                rt.mouse_buttons_held = g_mouse_buttons_held;
                rt.mouse_buttons_pressed = g_mouse_buttons_pressed;
                rt.mouse_buttons_released = g_mouse_buttons_released;
                rt.mouse_wheel_delta = g_mouse_wheel_delta;
                g_mouse_wheel_delta = 0;

                rt.step();

                if (f == cfg.headless_frames - 1 || !cfg.dump_frame_path.empty()) {
                    g_current_frame = rt.draw();
                }

                g_keys_pressed.clear();
                g_keys_released.clear();
                rt.keys_pressed.clear();
                rt.keys_released.clear();
                g_mouse_buttons_pressed.clear();
                g_mouse_buttons_released.clear();
                rt.mouse_buttons_pressed.clear();
                rt.mouse_buttons_released.clear();
                rt.mouse_wheel_delta = 0;
            }
            std::printf("[runner] Headless execution completed successfully.\n");
            if (!cfg.dump_frame_path.empty() && g_current_frame.w > 0) {
                stbi_write_png(cfg.dump_frame_path.c_str(), g_current_frame.w, g_current_frame.h, 4,
                               g_current_frame.rgba.data(), g_current_frame.w * 4);
                std::printf("[runner] Frame saved to %s (%dx%d)\n",
                            cfg.dump_frame_path.c_str(), g_current_frame.w, g_current_frame.h);
            }
            return 0;
        }

        // Register Win32 Window Class
        HINSTANCE hInstance = GetModuleHandleA(nullptr);
        const char* kClassName = "GM14_RUNNER_CLASS";

        WNDCLASSEXA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kClassName;

        if (!RegisterClassExA(&wc)) {
            std::fprintf(stderr, "Failed to register window class\n");
            return 1;
        }

        // Calculate window dimensions
        int base_w = (dw.window_width > 0) ? (int)dw.window_width : 640;
        int base_h = (dw.window_height > 0) ? (int)dw.window_height : 480;
        g_client_w = base_w * cfg.scale;
        g_client_h = base_h * cfg.scale;

        DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        RECT wr = { 0, 0, g_client_w, g_client_h };
        AdjustWindowRect(&wr, dwStyle, FALSE);
        int win_w = wr.right - wr.left;
        int win_h = wr.bottom - wr.top;

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        int win_x = std::max(0, (screen_w - win_w) / 2);
        int win_y = std::max(0, (screen_h - win_h) / 2);

        g_hwnd = CreateWindowExA(
            0,
            kClassName,
            game_title.c_str(),
            dwStyle,
            win_x, win_y,
            win_w, win_h,
            nullptr, nullptr,
            hInstance, nullptr
        );

        if (!g_hwnd) {
            std::fprintf(stderr, "Failed to create window\n");
            return 1;
        }

        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);

        // High resolution timer setup
        timeBeginPeriod(1);

        LARGE_INTEGER qpc_freq, qpc_start;
        QueryPerformanceFrequency(&qpc_freq);
        QueryPerformanceCounter(&qpc_start);

        // Load replay or start recording if configured
        if (!cfg.replay_path.empty()) {
            g_replayer.load(cfg.replay_path);
        }
        if (!cfg.record_path.empty()) {
            g_recorder.start(cfg.record_path);
        }

        std::printf("\n=======================================================\n");
        std::printf("  gm14 runner - Undertale Engine & Debug Tools\n");
        std::printf("  Room: %d (%s) | Scale: %dx\n",
                    rt.room_index, dw.rooms[rt.room_index].name.c_str(), cfg.scale);
        std::printf("  Hotkeys:\n");
        std::printf("    [PageDown] / [PageUp] : Skip to next / previous room\n");
        std::printf("    [Home]                : Reload current room\n");
        std::printf("    [F1]                  : Unfreeze player (reset interact = 0)\n");
        std::printf("    [F2]                  : Print room and player debug status\n");
        std::printf("    [F4]                  : Toggle fullscreen\n");
        std::printf("    [F9]                  : Toggle input recording\n");
        std::printf("=======================================================\n\n");
        std::fflush(stdout);

        int total_frames = 0;

        std::printf("[runner] Game loop started. Window: %dx%d (scale %dx)\n",
                    g_client_w, g_client_h, cfg.scale);

        // Initial draw
        g_current_frame = rt.draw();
        InvalidateRect(g_hwnd, nullptr, FALSE);

        // Main interactive game loop
        while (g_running && rt.running) {
            LARGE_INTEGER frame_start;
            QueryPerformanceCounter(&frame_start);

            // 1. Process Windows messages
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_running || !rt.running) break;

            // Handle debug hotkeys
            if (g_debug_skip_room != 0) {
                int target = rt.room_index + g_debug_skip_room;
                if (target >= 0 && target < (int)dw.rooms.size()) {
                    std::printf("\n[debug] Room skip (%s): room %d (%s) -> room %d (%s)\n\n",
                                (g_debug_skip_room > 0 ? "PageDown" : "PageUp"),
                                rt.room_index, dw.rooms[rt.room_index].name.c_str(),
                                target, dw.rooms[target].name.c_str());
                    std::fflush(stdout);
                    rt.globals["interact"] = gm14::Value(3.0);
                    rt.globals["entrance"] = gm14::Value(g_debug_skip_room > 0 ? 1.0 : 2.0);
                    rt.change_room(target);
                }
                g_debug_skip_room = 0;
            }
            if (g_debug_restart_room) {
                g_debug_restart_room = false;
                std::printf("\n[debug] Reloading room %d (%s)\n\n", rt.room_index, dw.rooms[rt.room_index].name.c_str());
                std::fflush(stdout);
                rt.change_room(rt.room_index);
            }
            if (g_debug_unfreeze) {
                g_debug_unfreeze = false;
                rt.globals["interact"] = gm14::Value(0.0);
                std::printf("\n[debug] Player unfreeze: global.interact reset to 0\n\n");
                std::fflush(stdout);
            }
            if (g_debug_dump_info) {
                g_debug_dump_info = false;
                std::printf("\n================ [DEBUG DUMP] ================\n");
                std::printf("Room: %d / %zu (%s)\n", rt.room_index, dw.rooms.size(), dw.rooms[rt.room_index].name.c_str());
                std::printf("Active Instances: %zu\n", rt.instances.size());
                std::printf("Camera view: (%d, %d)\n", rt.view_x, rt.view_y);
                if (rt.globals.count("interact")) {
                    std::printf("global.interact: %.1f\n", gm14::to_num(rt.globals["interact"]));
                }
                for (auto& inst : rt.instances) {
                    if (inst->alive && dw.objects[inst->obj].name == "obj_mainchara") {
                        double px = gm14::to_num(inst->vars.count("x") ? inst->vars["x"] : gm14::Value(0.0));
                        double py = gm14::to_num(inst->vars.count("y") ? inst->vars["y"] : gm14::Value(0.0));
                        std::printf("Player obj_mainchara: (x=%.1f, y=%.1f)\n", px, py);
                    }
                }
                std::printf("==============================================\n\n");
                std::fflush(stdout);
            }
            if (g_debug_toggle_record) {
                g_debug_toggle_record = false;
                if (g_recorder.recording) {
                    g_recorder.stop();
                } else {
                    time_t now = time(nullptr);
                    char tstr[64];
                    strftime(tstr, sizeof(tstr), "session_%Y%m%d_%H%M%S.rec", localtime(&now));
                    g_recorder.start(tstr);
                }
            }

            // Check optional timeout
            if (cfg.timeout_seconds > 0.0) {
                double elapsed_total = (double)(frame_start.QuadPart - qpc_start.QuadPart) / (double)qpc_freq.QuadPart;
                if (elapsed_total >= cfg.timeout_seconds) {
                    std::printf("[runner] Reached timeout of %.2f seconds.\n", cfg.timeout_seconds);
                    break;
                }
            }

            // Determine target simulation framerate
            double target_fps = cfg.override_fps;
            if (target_fps <= 0.0) {
                if (rt.globals.count("room_speed")) {
                    double spd = gm14::to_num(rt.globals["room_speed"]);
                    if (spd >= 1.0 && spd <= 240.0) target_fps = spd;
                } else if (rt.global_builtins.count("room_speed")) {
                    double spd = gm14::to_num(rt.global_builtins["room_speed"]);
                    if (spd >= 1.0 && spd <= 240.0) target_fps = spd;
                } else if (dw.rooms[rt.room_index].speed > 0) {
                    target_fps = (double)dw.rooms[rt.room_index].speed;
                } else {
                    target_fps = 30.0;
                }
            }
            double frame_duration = 1.0 / (target_fps * g_speed_modifier);

            // Sync input: replayer or live user input
            if (g_replayer.replaying) {
                g_replayer.step(total_frames, rt);
            } else {
                rt.keys_held = g_keys_held;
                rt.keys_pressed = g_keys_pressed;
                rt.keys_released = g_keys_released;

                // Sync real-time mouse state with aspect-ratio letterbox compensation
                if (g_current_frame.w > 0 && g_current_frame.h > 0) {
                    double scale_x = (double)g_client_w / (double)g_current_frame.w;
                    double scale_y = (double)g_client_h / (double)g_current_frame.h;
                    double scale = std::min(scale_x, scale_y);
                    if (scale > 0.0) {
                        int dst_w = (int)(g_current_frame.w * scale);
                        int dst_h = (int)(g_current_frame.h * scale);
                        int dst_x = (g_client_w - dst_w) / 2;
                        int dst_y = (g_client_h - dst_h) / 2;
                        double gx = (g_mouse_raw_x - dst_x) / scale;
                        double gy = (g_mouse_raw_y - dst_y) / scale;
                        gx = std::max(0.0, std::min((double)g_current_frame.w - 1.0, gx));
                        gy = std::max(0.0, std::min((double)g_current_frame.h - 1.0, gy));
                        rt.mouse_x = gx + rt.view_x;
                        rt.mouse_y = gy + rt.view_y;
                    }
                }
                rt.mouse_buttons_held = g_mouse_buttons_held;
                rt.mouse_buttons_pressed = g_mouse_buttons_pressed;
                rt.mouse_buttons_released = g_mouse_buttons_released;
                rt.mouse_wheel_delta = g_mouse_wheel_delta;
                g_mouse_wheel_delta = 0;
            }

            if (g_recorder.recording) {
                g_recorder.record_frame(total_frames, rt.keys_held, rt.keys_pressed, rt.keys_released,
                                        rt.mouse_x, rt.mouse_y,
                                        rt.mouse_buttons_held, rt.mouse_buttons_pressed, rt.mouse_buttons_released,
                                        rt.mouse_wheel_delta, rt.room_index);
            }

            // Step game simulation with room transition tracking
            int prev_room = rt.room_index;
            rt.step();
            if (rt.room_index != prev_room) {
                std::printf("[room] Switched to room %d (%s) - %zu active instances\n",
                            rt.room_index, dw.rooms[rt.room_index].name.c_str(), rt.instances.size());
                std::fflush(stdout);
            }

            // Draw new frame
            g_current_frame = rt.draw();

            // Blit RGBA buffer to window device context
            HDC hdc = GetDC(g_hwnd);
            if (hdc) {
                render_frame(hdc);
                ReleaseDC(g_hwnd, hdc);
            }

            // Clear single-step trigger sets AFTER draw has executed
            g_keys_pressed.clear();
            g_keys_released.clear();
            rt.keys_pressed.clear();
            rt.keys_released.clear();

            g_mouse_buttons_pressed.clear();
            g_mouse_buttons_released.clear();
            rt.mouse_buttons_pressed.clear();
            rt.mouse_buttons_released.clear();
            rt.mouse_wheel_delta = 0;

            total_frames++;

            // Precise frame limiter
            LARGE_INTEGER qpc_now;
            QueryPerformanceCounter(&qpc_now);
            double elapsed_sec = (double)(qpc_now.QuadPart - frame_start.QuadPart) / (double)qpc_freq.QuadPart;
            double remaining_ms = (frame_duration - elapsed_sec) * 1000.0;
            if (remaining_ms > 2.0) {
                Sleep((DWORD)(remaining_ms - 1.5));
            }
            while (true) {
                QueryPerformanceCounter(&qpc_now);
                double cur_elapsed = (double)(qpc_now.QuadPart - frame_start.QuadPart) / (double)qpc_freq.QuadPart;
                if (cur_elapsed >= frame_duration) break;
                YieldProcessor();
            }
        }

        g_recorder.stop();
        timeEndPeriod(1);

        if (!cfg.dump_frame_path.empty() && g_current_frame.w > 0) {
            stbi_write_png(cfg.dump_frame_path.c_str(), g_current_frame.w, g_current_frame.h, 4,
                           g_current_frame.rgba.data(), g_current_frame.w * 4);
            std::printf("[runner] Frame saved to %s (%dx%d)\n",
                        cfg.dump_frame_path.c_str(), g_current_frame.w, g_current_frame.h);
        }

        std::printf("[runner] Execution finished (%d frames simulated).\n", total_frames);

    } catch (const std::exception& e) {
        std::fprintf(stderr, "[fatal error] %s\n", e.what());
        return 1;
    }

    return 0;
}
