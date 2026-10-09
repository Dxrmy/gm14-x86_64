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
#include <algorithm>
#include <iostream>

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

// Fullscreen toggle state
bool g_is_fullscreen = false;
RECT g_saved_window_rect = {};
DWORD g_saved_window_style = 0;

// Map Windows Virtual-Key code to GameMaker 1.4 key code
int map_vk_to_gm(WPARAM vk) {
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
            int gm_key = map_vk_to_gm(wParam);
            bool repeat = (lParam & (1 << 30)) != 0;
            if (!repeat || !g_keys_held.count(gm_key)) {
                g_keys_pressed.insert(gm_key);
            }
            g_keys_held.insert(gm_key);

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
            return 0;
        }

        case WM_KILLFOCUS: {
            for (int k : g_keys_held) {
                g_keys_released.insert(k);
            }
            g_keys_held.clear();
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
    std::printf("  --headless <frames>   Run N frames headless without opening a window\n");
    std::printf("  --timeout <seconds>   Run for N seconds then exit cleanly\n");
    std::printf("  --dump-frame <path>   Save final frame as PNG\n");
    std::printf("  --help, -h            Show this help text\n");
    std::printf("\nControls:\n");
    std::printf("  Arrow Keys            Movement / Navigation (vk_left, vk_right, vk_up, vk_down)\n");
    std::printf("  Z / Enter             Confirm / Interact (gp_face1)\n");
    std::printf("  X / Shift             Cancel / Run (gp_face2)\n");
    std::printf("  C / Ctrl              Menu / Item (gp_face3)\n");
    std::printf("  F4 / Alt+Enter        Toggle Fullscreen\n");
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
        } else if (arg == "--headless" && i + 1 < argc) {
            cfg.headless_frames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            cfg.timeout_seconds = std::atof(argv[++i]);
        } else if (arg == "--dump-frame" && i + 1 < argc) {
            cfg.dump_frame_path = argv[++i];
        } else if (arg[0] != '-' && cfg.data_win_path.empty()) {
            cfg.data_win_path = arg;
        }
    }

    // Default search paths for data.win if not explicitly passed
    if (cfg.data_win_path.empty()) {
        const char* candidates[] = {
            "data.win",
            "game.win",
            "E:\\SteamLibrary\\steamapps\\common\\Undertale\\data.win"
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

        gm14::Runtime rt(dw);

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
        if (target_room >= 0) {
            if (target_room != 0) {
                std::printf("[room] Transitioning to room %d (%s)\n", target_room, dw.rooms[target_room].name.c_str());
                rt.change_room(target_room);
            }
        } else {
            // Default transition: if room 0 is room_start and room 1 is room_introstory (like Undertale),
            // proceed to room 1.
            if (dw.rooms.size() > 1 && dw.rooms[0].name == "room_start" && dw.rooms[1].name == "room_introstory") {
                std::printf("[room] Transitioning to room 1 (%s)\n", dw.rooms[1].name.c_str());
                rt.change_room(1);
            }
        }

        // Headless execution mode (ideal for automated testing / verification)
        if (cfg.headless_frames > 0) {
            std::printf("[runner] Executing %d headless frames...\n", cfg.headless_frames);
            for (int f = 0; f < cfg.headless_frames && rt.running; ++f) {
                rt.keys_held = g_keys_held;
                rt.keys_pressed = g_keys_pressed;
                rt.keys_released = g_keys_released;

                rt.step();

                if (f == cfg.headless_frames - 1 || !cfg.dump_frame_path.empty()) {
                    g_current_frame = rt.draw();
                }

                g_keys_pressed.clear();
                g_keys_released.clear();
                rt.keys_pressed.clear();
                rt.keys_released.clear();
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
            double frame_duration = 1.0 / target_fps;

            // Sync real-time keyboard state to runtime
            rt.keys_held = g_keys_held;
            rt.keys_pressed = g_keys_pressed;
            rt.keys_released = g_keys_released;

            // Step game simulation
            rt.step();

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
