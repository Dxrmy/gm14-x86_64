// gm14 C++ virtual machine (port of gmlib/vm.py) for the 3DS build.
#pragma once
#include "dw.hpp"
#include "audio.hpp"
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <set>
#include <utility>
#include <algorithm>
#include <iostream>

namespace gm14 {

struct Value {
    enum Type { NUM, STR, UNDEF, VARPTR } type = UNDEF;
    double num = 0;
    std::string str;
    // When a scalar read resolves to an array-backed variable, carry the array
    // so builtins (string_char_at, draw_text, ...) can index it as GM does.
    const std::vector<Value>* arrref = nullptr;

    Value() {}
    Value(double n) : type(NUM), num(n) {}
    Value(int n) : type(NUM), num((double)n) {}
    Value(const std::string& s) : type(STR), str(s) {}
    Value(const char* s) : type(STR), str(s) {}
    bool is_str() const { return type == STR; }
    bool is_num() const { return type == NUM; }
    bool is_arr() const { return arrref != nullptr; }
};

double to_num(const Value& v);
std::string to_str(const Value& v);
bool to_bool(const Value& v);

struct Instance {
    int iid = 0;
    int obj = 0;
    bool alive = true;
    std::unordered_map<std::string, Value> vars;
    std::unordered_map<std::string, std::vector<Value>> arrays;
    std::vector<int> alarms;   // -1 == unset
    // Path tracking
    int path_index = -1;
    double path_speed = 0.0;
    double path_position = 0.0;
    int path_endaction = 0;
    bool path_absolute = true;
    double path_scale = 1.0;
    double path_orientation = 0.0;
    double path_start_x = 0.0;
    double path_start_y = 0.0;
    Instance(int iid_, int obj_) : iid(iid_), obj(obj_), alarms(12, -1) {}
};

struct Frame {
    const CodeEntry* entry = nullptr;
    const Instruction* base = nullptr;
    int n = 0;
    std::vector<uint32_t> addrs;
    std::unordered_map<uint32_t, int> addr_index;
    int pc = 0;
    std::vector<Value> stack;
    std::vector<Value> locals;
    std::vector<Value> args;
    std::unordered_map<int, std::vector<Value>> local_arrays;
    Instance* self = nullptr;
    Instance* other = nullptr;
    Value value;
};

struct EventContext {
    Instance* inst = nullptr;
    int cur_obj = -1;
    int etype = 0;
    int subtype = 0;
};

struct EnvFrame {
    Instance* saved_cur = nullptr;
    Instance* saved_other = nullptr;
    std::vector<Instance*> inst_list;
    size_t inst_idx = 0;
    int loop_pc = 0;
};

struct DsGrid {
    int width = 0;
    int height = 0;
    std::vector<Value> data;
    DsGrid() = default;
    DsGrid(int w, int h, const Value& init = Value(0.0))
        : width(std::max(0, w)), height(std::max(0, h)),
          data((size_t)std::max(0, w) * (size_t)std::max(0, h), init) {}
    Value get(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height) return Value(0.0);
        return data[(size_t)y * (size_t)width + (size_t)x];
    }
    void set(int x, int y, const Value& v) {
        if (x >= 0 && x < width && y >= 0 && y < height) data[(size_t)y * (size_t)width + (size_t)x] = v;
    }
    void clear(const Value& v) {
        for (auto& cell : data) cell = v;
    }
    void set_region(int x1, int y1, int x2, int y2, const Value& v) {
        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);
        x1 = std::max(0, x1);
        y1 = std::max(0, y1);
        x2 = std::min(width - 1, x2);
        y2 = std::min(height - 1, y2);
        for (int y = y1; y <= y2; ++y) {
            for (int x = x1; x <= x2; ++x) {
                data[(size_t)y * (size_t)width + (size_t)x] = v;
            }
        }
    }
};

struct TextFile {
    enum Mode { READ, WRITE, APPEND } mode = READ;
    std::string path;
    std::string read_buf;
    size_t read_pos = 0;
    FILE* fp = nullptr;
};

struct IniFile {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
    void clear() {
        sections.clear();
    }
};

struct Surface {
    Image img;
};

// Global execution context trackers for crash and OOM diagnostics
extern const char* g_diag_code_name;
extern int g_diag_pc;
extern const char* g_diag_obj_name;
extern int g_diag_inst_id;
extern const char* g_diag_func_name;

struct EngineConfig {
#ifdef _3DS
    int max_texture_pages = 2;            // Max texture sheets kept in RAM (2 on 3DS, 0 = unlimited on PC)
#else
    int max_texture_pages = 0;            // Max texture sheets kept in RAM (0 = unlimited on PC)
#endif
    size_t audio_pcm_budget_bytes = 16 * 1024 * 1024; // Bounded PCM sound cache (8MB on 3DS, 16MB on low-spec/PC)
    long long instruction_budget = 2000000;// Bytecode loop watchdog (250k on 3DS, 2M on PC)
    int culling_margin = 32;              // Safety margin for tile culling
    bool enable_blend_modes = true;       // Enable bm_add additive blending
    std::string save_directory = "./saves";// Directory for .ini and .sav files
    int target_fps = 30;                  // Target framerate (30 for Undertale, 0 = room speed)
    int start_room = -1;                  // -1 = natural boot, >= 0 = force room
    bool verbose_logging = false;         // Enable/disable diagnostic sink
};

EngineConfig load_engine_config(const std::string& ini_path);

class Runtime {
public:
    explicit Runtime(DataWin& dw, const EngineConfig& cfg = EngineConfig{});
    ~Runtime();

    DataWin& dw;
    EngineConfig cfg;
    std::unordered_map<std::string, const CodeEntry*> scripts;
    std::unordered_map<std::string, Value> globals;
    std::unordered_map<std::string, std::vector<Value>> global_arrays;
    std::unordered_map<std::string, Value> global_builtins;
    std::vector<std::unique_ptr<Instance>> instances;
    Instance* cur = nullptr;
    Instance* other = nullptr;
    std::vector<EnvFrame> env_stack;
    std::vector<EventContext> event_stack;
    int next_iid = 100000;
    int room_index = 0;
    int pending_room = -1;
    bool running = true;
    bool fast_boot = true;
    int frame_count = 0;
    // Optional diagnostic sink (set by the 3DS front-end while recording).
    void (*diag_sink)(const char*) = nullptr;
    std::set<std::string> warned;
    std::vector<std::string> m_unimplemented;   // unknown builtins seen, in order

    // input
    std::set<int> keys_held, keys_pressed, keys_released;
    int mouse_x = 0, mouse_y = 0;
    std::set<int> mouse_buttons_held, mouse_buttons_pressed, mouse_buttons_released;
    int mouse_wheel_delta = 0;

    // drawing
    Image* screen = nullptr;
    int view_x = 0, view_y = 0;
    int draw_color = 0xFFFFFF;
    double draw_alpha = 1.0;
    int draw_blend_mode = 0; // 0 = bm_normal, 1 = bm_add, 2 = bm_max, 3 = bm_subtract
    int draw_font = -1;
    int draw_halign = 0; // 0=fa_left, 1=fa_center, 2=fa_right
    int draw_valign = 0; // 0=fa_top, 1=fa_middle, 2=fa_bottom
    std::unordered_map<int, Image> page_cache;
    std::vector<int> page_lru;
    const Image& page(int tex_index);
    bool tpag_image(int tpag_index, Image& out, int& tx, int& ty);
    void blit_screen(const Image& src, int dx, int dy, double alpha);
    void blit_sub_screen(const Image& src, int sx, int sy, int sw, int sh, int dx, int dy, double alpha);
    void blit_sub_screen_tint(const Image& src, int sx, int sy, int sw, int sh, int dx, int dy, double alpha, uint32_t color);
    // Scaled + tinted + optionally rotated blit. (dx,dy) is the top-left of the
    // destination rect; out_w/out_h are the scaled size. angle is in degrees.
    // If use_pivot is true, rotates about (pivot_x, pivot_y); otherwise about rect center.
    void blit_sub_screen_scaled(const Image& src, int sx, int sy, int sw, int sh,
                                int dx, int dy, int out_w, int out_h,
                                double alpha, uint32_t color = 0xFFFFFF, double angle_deg = 0.0,
                                double pivot_x = 0.0, double pivot_y = 0.0, bool use_pivot = false);

    using Builtin = std::function<Value(Runtime&, std::vector<Value>&)>;
    std::unordered_map<std::string, Builtin> builtins;
    std::unordered_map<int, std::unordered_map<std::string, Value>> ds_maps;
    int next_ds_map = 1;
    std::unordered_map<int, std::vector<Value>> ds_lists;
    int next_ds_list = 1;
    std::unordered_map<int, DsGrid> ds_grids;
    int next_ds_grid = 1;
    std::unordered_map<int, TextFile> text_files;
    int next_text_file = 1;

    std::unordered_map<std::string, IniFile> ini_files;
    std::string current_ini_filename;
    IniFile current_ini;
    bool ini_is_open = false;
    std::string save_dir;
    std::string resolve_path(const std::string& fn) const;

    // surfaces
    std::unordered_map<int, Surface> surfaces;
    int next_surface_id = 1;
    std::vector<Image*> surface_target_stack;

    // caster audio
    std::unordered_map<std::string, int> caster_path_to_id;
    int next_caster_id = 10000;

    // audio
    std::unique_ptr<AudioEngine> audio;
    void ensure_audio();
    int audio_asset_index(const Value& v) const;   // asset index from a GML sound value
    void audio_register_clips();                   // decode all SOND clips (lazy, cached)

    // lifecycle
    void start_room(int index, std::vector<std::unique_ptr<Instance>> keep = {});
    void change_room(int index);
    void run_event(Instance* inst, int etype, int subtype);
    bool action_relative = false;
    bool gui_drawing = false;
    int gui_width = -1;
    int gui_height = -1;
    bool current_room_persistent = false;
    std::unordered_map<int, std::vector<std::unique_ptr<Instance>>> persistent_rooms;
    void update_camera();
    void update_view_follow();
    void get_path_point(const Path& path, double pos, double& out_x, double& out_y, double& out_spd);

    void step();
    Image draw();

    // State Saver & Loader
    bool save_state(std::ostream& os) const;
    bool load_state(std::istream& is);
    bool save_state_file(const std::string& filepath) const;
    bool load_state_file(const std::string& filepath);
    int load_caster_sound(const std::string& raw_name, int forced_id = -1);

    Value call(const std::string& name, std::vector<Value>& args);
    Value run_entry(const CodeEntry* entry, std::vector<Value>& args,
                    Instance* self, Instance* other);

    void exec(Frame& fr);
    Value get_var(Frame& fr, const Instruction& ins);
    void set_var(Frame& fr, const Instruction& ins, int inst_type, const Value& v);
    void set_var_inst(Instance* inst, const std::string& name, const Value& v);
    Value get_array(Frame& fr, const Instruction& ins, int inst_type, int index);
    void set_array(Frame& fr, const Instruction& ins, int inst_type, int index, const Value& v);
    Instance* resolve_inst(int inst_type);

private:
    void register_builtins();
};

} // namespace gm14
