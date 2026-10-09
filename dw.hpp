// gm14 — GameMaker Studio 1.4 data.win parser + room renderer (C++17)
//
// Port of the Python reference under tools/gm14/gmlib. Designed to compile
// both for the host (for testing, with STB image I/O) and for the Nintendo 3DS
// (citro2d backend in main_3ds.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <utility>
#include <memory>
#include <stdexcept>

namespace gm14 {

struct Chunk { uint32_t off; uint32_t size; };

// ---- instructions (bytecode v15/16) ----
enum : uint8_t {
    OP_CONV = 0x07, OP_MUL = 0x08, OP_DIV = 0x09, OP_REM = 0x0A, OP_MOD = 0x0B,
    OP_ADD = 0x0C, OP_SUB = 0x0D, OP_AND = 0x0E, OP_OR = 0x0F, OP_XOR = 0x10,
    OP_NEG = 0x11, OP_NOT = 0x12, OP_SHL = 0x13, OP_SHR = 0x14, OP_CMP = 0x15,
    OP_POP = 0x45, OP_DUP = 0x86, OP_RET = 0x9C, OP_EXIT = 0x9D, OP_POPZ = 0x9E,
    OP_CALLV = 0x99, OP_B = 0xB6, OP_BT = 0xB7, OP_BF = 0xB8,
    OP_PUSHENV = 0xBA, OP_POPENV = 0xBB, OP_PUSH = 0xC0, OP_PUSHLOC = 0xC1,
    OP_PUSHGLB = 0xC2, OP_PUSHBLTN = 0xC3, OP_PUSHI = 0x84, OP_CALL = 0xD9,
    OP_BREAK = 0xFF
};

struct Instruction {
    uint32_t first_word = 0;
    int32_t raw_operand = 0;
    uint32_t file_off = 0;
    int16_t var = -1;          // index into DataWin::variables
    int16_t fun = -1;          // index into DataWin::functions
    double value = 0;          // numeric literal (push)

    uint8_t kind() const { return (first_word >> 24) & 0xFF; }
    uint8_t type1() const { return (first_word >> 16) & 0xF; }
    uint8_t type2() const { return (first_word >> 20) & 0xF; }
    uint8_t cmp() const { return (first_word >> 8) & 0xFF; }
    uint16_t low16() const { return first_word & 0xFFFF; }
    uint8_t ref_type() const { return ((uint32_t)raw_operand >> 24) & 0xF8; }
    uint32_t str_index() const { return (uint32_t)raw_operand; }
    uint8_t size() const {
        uint8_t t1 = type1();
        switch (kind()) {
            case OP_PUSH: case OP_PUSHLOC: case OP_PUSHGLB: case OP_PUSHBLTN: case OP_PUSHI:
                if (t1 == 0 || t1 == 3) return 3;
                if (t1 == 0x0F) return 1;
                return 2;
            case OP_POP: return (t1 == 0x0F) ? 1 : 2;
            case OP_CALL: return 2;
            case OP_BREAK: return (t1 == 2) ? 2 : 1;
            default: return 1;
        }
    }
};

struct CodeEntry {
    uint32_t file_off = 0;
    uint32_t blob_off = 0;
    std::string name;
    uint32_t length = 0;
    uint16_t locals = 0, args = 0;
    bool weird = false;
    uint32_t instr_start = 0;   // index into DataWin::all_instrs
    uint32_t instr_count = 0;
};

struct Variable {
    std::string name;
    int32_t instance_type = 0;
    int32_t varid = 0;
    uint32_t occurrences = 0;
    int32_t first = -1;
};

struct Function {
    std::string name;
    uint32_t occurrences = 0;
    int32_t first = -1;
};

struct Tpag {
    uint16_t sx, sy, sw, sh, tx, ty, tw, th, bw, bh;
    int16_t tex;
};

struct Sprite {
    std::string name;
    uint32_t width = 0, height = 0;
    int32_t ml = 0, mr = 0, mb = 0, mt = 0;
    int32_t origin_x = 0, origin_y = 0;
    std::vector<int> frames;       // TPAG indices
};

struct Bgnd {
    std::string name;
    bool transparent = false, smooth = false, preload = false;
    int tpag = -1;
};

struct RoomInstance {
    int32_t x = 0, y = 0, obj = -1;
    uint32_t iid = 0;
    int32_t creation_code = -1, precreate_code = -1;
    float scale_x = 1, scale_y = 1;
    uint32_t color = 0xFFFFFFFF;
    float rotation = 0;
};

struct RoomBackground {
    bool enabled = false, foreground = false;
    int bgnd = -1;
    int32_t x = 0, y = 0, tile_x = 1, tile_y = 1, speed_x = 0, speed_y = 0;
    bool stretch = false;
};

struct Tile {
    int32_t x, y, bgnd, srcx, srcy, w, h, depth;
    uint32_t iid;
    float scale_x, scale_y;
    uint32_t color;
};

struct RoomView {
    bool visible = false;
    int32_t x = 0, y = 0, w = 320, h = 240;
    int32_t port_x = 0, port_y = 0, port_w = 640, port_h = 480;
    int32_t border_x = 0, border_y = 0, speed_x = -1, speed_y = -1;
    int32_t follow_object = -1;
};

struct Room {
    std::string name;
    uint32_t width = 0, height = 0, speed = 0;
    bool persistent = false;
    uint32_t bg_color = 0;
    bool draw_bg_color = false;
    int32_t creation_code = -1;
    uint32_t flags = 0;
    std::vector<RoomBackground> backgrounds;
    std::vector<RoomView> views;
    std::vector<RoomInstance> instances;
    std::vector<Tile> tiles;
};

// Event type -> subtype -> list of code-entry indices
using EventMap = std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<int>>>;

struct ObjDef {
    std::string name;
    int32_t sprite = -1;
    bool visible = true, solid = false, persistent = false;
    int32_t depth = 0, parent = -1, mask = -1;
    EventMap events;
};

// ---- image ----
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

struct Glyph {
    uint16_t ch = 0, sx = 0, sy = 0, sw = 0, sh = 0;
    int16_t shift = 0, offset = 0;
};
struct Font {
    std::string name;
    uint32_t em = 0;
    int tpag = -1;
    float scale_x = 1, scale_y = 1;
    std::unordered_map<uint16_t, Glyph> glyphs;
};

struct Sound {
    std::string name;
    uint32_t flags = 0;    // 0x1 embedded, 0x2 compressed, 0x64 regular
    std::string type;
    std::string file;
    uint32_t effects = 0;
    float volume = 1.0f;
    float pitch = 0.0f;    // (panning in legacy GMS)
    int32_t audo_id = -1;  // index into DataWin::audio, -1 when unembedded
    bool is_embedded() const { return (flags & 0x1) != 0; }
};

struct EmbeddedAudio {
    uint32_t offset = 0;   // absolute file offset of the data blob
    uint32_t length = 0;   // blob byte length
};

class DataWin;

// LRU cache for decoded texture pages (budgeted memory for constrained platforms / o3DS)
class TexturePageCache {
public:
    explicit TexturePageCache(size_t max_pages = 8) : m_capacity(std::max<size_t>(1, max_pages)) {}

    void set_capacity(size_t cap) {
        m_capacity = std::max<size_t>(1, cap);
        trim();
    }
    size_t capacity() const { return m_capacity; }
    size_t size() const { return m_order.size(); }

    void clear() {
        m_entries.clear();
        m_order.clear();
    }

    bool contains(int tex_index) const {
        return m_entries.find(tex_index) != m_entries.end();
    }

    template <typename Loader>
    const Image& get_or_load(int tex_index, Loader&& loader) {
        auto it = m_entries.find(tex_index);
        if (it != m_entries.end()) {
            m_order.splice(m_order.begin(), m_order, it->second.lru_it);
            return it->second.image;
        }
        while (m_order.size() >= m_capacity) {
            evict_oldest();
        }
        Image img = loader(tex_index);
        m_order.push_front(tex_index);
        Entry entry{ std::move(img), m_order.begin() };
        auto inserted = m_entries.emplace(tex_index, std::move(entry));
        return inserted.first->second.image;
    }

    const Image& get(int tex_index, const DataWin& dw);

    bool evict_oldest() {
        if (m_order.empty()) return false;
        int oldest_idx = m_order.back();
        m_order.pop_back();
        m_entries.erase(oldest_idx);
        return true;
    }

private:
    void trim() {
        while (m_order.size() > m_capacity) {
            evict_oldest();
        }
    }

    struct Entry {
        Image image;
        std::list<int>::iterator lru_it;
    };

    size_t m_capacity = 8;
    std::list<int> m_order; // front = MRU, back = LRU
    std::unordered_map<int, Entry> m_entries;
};

class DataWin {
public:
    explicit DataWin(const std::string& path);
    ~DataWin();

    // chunks
    std::unordered_map<std::string, Chunk> chunks;
    std::vector<std::string> chunk_order;

    // general info
    int bytecode_version = 16;
    std::string game_name, display_name;
    uint32_t window_width = 0, window_height = 0;
    uint32_t game_id = 0, last_obj = 0;
    std::vector<uint32_t> room_order;

    std::vector<std::string> strings;
    std::unordered_map<uint32_t, int> str_by_content;

    std::vector<CodeEntry> code;
    std::vector<Instruction> all_instrs;   // flat pool
    std::vector<Variable> variables;
    std::vector<Function> functions;

    std::vector<uint32_t> tex_ptrs;    // raw PNG blob start offsets
    std::vector<std::pair<std::string,int>> scripts;  // SCPT: (name, code id)
    std::vector<Tpag> tpags;
    std::vector<Sprite> sprites;
    std::vector<Bgnd> bgnds;
    std::vector<Font> fonts;
    std::vector<ObjDef> objects;
    std::vector<Room> rooms;
    std::vector<Sound> sounds;
    std::vector<EmbeddedAudio> audio;

    // Load the raw bytes of an embedded audio entry (AUDO).
    std::vector<uint8_t> audio_bytes(int audo_index) const;
    const Sound* sound_by_name(const std::string& name) const;
    int sound_index_by_name(const std::string& name) const;
    const Sound* sound_at(int index) const { return (index >= 0 && index < (int)sounds.size()) ? &sounds[index] : nullptr; }

    // helpers
    Image load_texture(int index) const;
    const Image& get_texture(int index) const;
    void set_texture_cache_capacity(size_t cap) const;
    void clear_texture_cache() const;
    size_t texture_cache_size() const;
    TexturePageCache& texture_cache() const { return m_texture_cache; }
    const Tpag* tpag(int i) const { return (i >= 0 && i < (int)tpags.size()) ? &tpags[i] : nullptr; }

    // render room i to an RGBA buffer (room size)
    Image render_room(int room_index) const;

    size_t size() const { return m_file_size; }

    // random-access reads (streamed from the file, with CODE/STRG cached)
    void read_at(uint32_t o, uint32_t n, void* dst) const;
    uint32_t u32_at(uint32_t o) const { return u32(o); }
    std::vector<uint8_t> bytes_at(uint32_t o, uint32_t n) const;

private:
    FILE* m_fp = nullptr;
    size_t m_file_size = 0;
    mutable TexturePageCache m_texture_cache{8};
    std::vector<uint8_t> m_code_cache, m_strg_cache;
    uint32_t m_code_off = 0, m_code_len = 0, m_strg_off = 0, m_strg_len = 0;
    mutable std::vector<uint8_t> m_sector_buf;
    mutable uint32_t m_sector_start = 0xFFFFFFFF;
    mutable uint32_t m_sector_len = 0;

    // low-level reads
    uint8_t u8(uint32_t o) const;
    uint16_t u16(uint32_t o) const;
    int16_t i16(uint32_t o) const;
    uint32_t u32(uint32_t o) const;
    int32_t i32(uint32_t o) const;
    uint64_t u64(uint32_t o) const;
    double f64(uint32_t o) const;
    float f32(uint32_t o) const;
    std::string gm_string_at_obj(uint32_t o) const;
    std::string str_content(uint32_t o) const;
    void need(uint32_t o, uint32_t n) const;

    void parse_general();
    void parse_strings();
    void parse_code();
    void parse_variables();
    void parse_functions();
    void resolve_references();
    void parse_tpag();
    void parse_textures();
    void parse_scripts();
    void parse_sprites();
    void parse_bgnd();
    void parse_fonts();
    void parse_objects();
    void parse_rooms();
    void parse_sounds();
    void parse_audio();
    void build_texture_index();

    // texture blob bounds
    std::unordered_map<uint32_t, uint32_t> m_tex_end;
};

} // namespace gm14
