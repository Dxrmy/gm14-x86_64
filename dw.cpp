#include "dw.hpp"

#include <cstdlib>
#include <cstdio>
#if !defined(__3DS__) && !defined(_WIN32)
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef __3DS__
#include <3ds.h>
static void log_status(const char* msg) {
    std::printf("%s", msg);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}
#else
static void log_status(const char* msg) {
    std::printf("%s", msg);
    std::fflush(stdout);
}
#endif

namespace gm14 {

DataWin::DataWin(const std::string& path) : filepath(path) {
    log_status("[1/10] Opening data.win...\n");
    m_fp = std::fopen(path.c_str(), "rb");
    if (!m_fp) throw std::runtime_error("cannot open " + path);
    std::fseek(m_fp, 0, SEEK_END);
    long sz = std::ftell(m_fp);
    std::fseek(m_fp, 0, SEEK_SET);
    if (sz <= 0) throw std::runtime_error("empty file");
    m_file_size = (size_t)sz;

    char form[4];
    read_at(0, 4, form);
    if (std::memcmp(form, "FORM", 4) != 0)
        throw std::runtime_error("not a FORM data.win");

    uint32_t form_size = u32(4);
    uint32_t off = 8, end = 8 + form_size;
    while (off + 8 <= end) {
        char id[5] = {0};
        read_at(off, 4, id);
        uint32_t csz = u32(off + 4);
        chunks[id] = Chunk{off + 8, csz};
        chunk_order.push_back(id);
        off += 8 + csz;
    }
    // cache the two hottest chunks (CODE, STRG) so parsing is fast
    if (chunks.count("CODE")) { auto& ch = chunks["CODE"]; m_code_off = ch.off;
        m_code_cache.resize(ch.size); std::fseek(m_fp, (long)ch.off, SEEK_SET);
        std::fread(m_code_cache.data(), 1, ch.size, m_fp); m_code_len = ch.size; }
    if (chunks.count("STRG")) { auto& ch = chunks["STRG"]; m_strg_off = ch.off;
        m_strg_cache.resize(ch.size); std::fseek(m_fp, (long)ch.off, SEEK_SET);
        std::fread(m_strg_cache.data(), 1, ch.size, m_fp); m_strg_len = ch.size; }

    log_status("[2/10] Parsing general info...\n");
    parse_general();
    log_status("[3/10] Parsing strings...\n");
    parse_strings();

    log_status("[4/10] Parsing bytecode (891k ops)...\n");
    parse_code();
    m_code_cache.clear();
    m_code_cache.shrink_to_fit();
    m_code_len = 0;

    log_status("[5/10] Resolving references...\n");
    parse_variables();
    parse_functions();
    resolve_references();

    log_status("[6/10] Parsing textures & tpag...\n");
    parse_textures();
    parse_scripts();
    parse_tpag();

    log_status("[7/10] Parsing sprites...\n");
    parse_sprites();

    log_status("[8/10] Parsing fonts & backgrounds...\n");
    parse_fonts();
    parse_bgnd();

    log_status("[9/10] Parsing objects...\n");
    parse_objects();

    log_status("[10/10] Parsing rooms...\n");
    parse_rooms();
    parse_sounds();
    parse_audio();
    build_texture_index();
    log_status("Game data loaded successfully!\n");
}

DataWin::~DataWin() {
    if (m_fp) std::fclose(m_fp);
}

void DataWin::read_at(uint32_t o, uint32_t n, void* dst) const {
    if ((size_t)o + n > m_file_size) throw std::runtime_error("read out of bounds");
    if (n == 0) return;
    if (m_code_len > 0 && o >= m_code_off && (uint64_t)o + n <= (uint64_t)m_code_off + m_code_len) {
        std::memcpy(dst, m_code_cache.data() + (o - m_code_off), n); return;
    }
    if (m_strg_len > 0 && o >= m_strg_off && (uint64_t)o + n <= (uint64_t)m_strg_off + m_strg_len) {
        std::memcpy(dst, m_strg_cache.data() + (o - m_strg_off), n); return;
    }

    if (m_sector_len > 0 && o >= m_sector_start && (uint64_t)o + n <= (uint64_t)m_sector_start + m_sector_len) {
        std::memcpy(dst, m_sector_buf.data() + (o - m_sector_start), n);
        return;
    }

    if (n >= 65536) {
        std::fseek(m_fp, (long)o, SEEK_SET);
        if (std::fread(dst, 1, n, m_fp) != n) throw std::runtime_error("short read");
        return;
    }

    if (m_sector_buf.size() < 65536) m_sector_buf.resize(65536);
    m_sector_start = o & ~0xFFFF;
    size_t to_read = 65536;
    if ((size_t)m_sector_start + to_read > m_file_size)
        to_read = m_file_size - m_sector_start;
    std::fseek(m_fp, (long)m_sector_start, SEEK_SET);
    size_t got = std::fread(m_sector_buf.data(), 1, to_read, m_fp);
    m_sector_len = (uint32_t)got;

    if (o >= m_sector_start && (uint64_t)o + n <= (uint64_t)m_sector_start + m_sector_len) {
        std::memcpy(dst, m_sector_buf.data() + (o - m_sector_start), n);
    } else {
        std::fseek(m_fp, (long)o, SEEK_SET);
        if (std::fread(dst, 1, n, m_fp) != n) throw std::runtime_error("short read");
    }
}

std::vector<uint8_t> DataWin::bytes_at(uint32_t o, uint32_t n) const {
    std::vector<uint8_t> v(n);
    read_at(o, n, v.data());
    return v;
}

uint8_t  DataWin::u8(uint32_t o) const { uint8_t v; read_at(o,1,&v); return v; }
uint16_t DataWin::u16(uint32_t o) const { uint16_t v; read_at(o,2,&v); return v; }
int16_t  DataWin::i16(uint32_t o) const { int16_t v; read_at(o,2,&v); return v; }
uint32_t DataWin::u32(uint32_t o) const { uint32_t v; read_at(o,4,&v); return v; }
int32_t  DataWin::i32(uint32_t o) const { int32_t v; read_at(o,4,&v); return v; }
uint64_t DataWin::u64(uint32_t o) const { uint64_t v; read_at(o,8,&v); return v; }
double   DataWin::f64(uint32_t o) const { double v; read_at(o,8,&v); return v; }
float    DataWin::f32(uint32_t o) const { float v; read_at(o,4,&v); return v; }

std::string DataWin::gm_string_at_obj(uint32_t o) const {
    if (o == 0) return "";
    uint32_t n = u32(o);
    if (n > 16 * 1024 * 1024) throw std::runtime_error("insane string length");
    std::vector<uint8_t> buf(n);
    read_at(o + 4, n, buf.data());
    return std::string((const char*)buf.data(), n);
}

std::string DataWin::str_content(uint32_t o) const {
    if (o == 0) return "";
    auto it = str_by_content.find(o);
    if (it != str_by_content.end() && it->second >= 0 && it->second < (int)strings.size())
        return strings[it->second];
    return gm_string_at_obj(o - 4);
}

void DataWin::parse_general() {
    Chunk c = chunks.at("GEN8");
    uint32_t o = c.off;
    bytecode_version = u8(o + 0x01);
    game_name = str_content(u32(o + 0x28));
    display_name = str_content(u32(o + 0x64));
    window_width = u32(o + 0x3C);
    window_height = u32(o + 0x40);
    game_id = u32(o + 0x14);
    last_obj = u32(o + 0x0C);
    uint32_t ro = o + 0x80;
    uint32_t n = u32(ro);
    for (uint32_t i = 0; i < n; ++i) room_order.push_back(u32(ro + 4 + 4 * i));
}

void DataWin::parse_strings() {
    Chunk c = chunks.at("STRG");
    uint32_t n = u32(c.off);
    strings.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = u32(c.off + 4 + 4 * i);
        strings[i] = gm_string_at_obj(p);
        str_by_content[p + 4] = (int)i;
    }
}

void DataWin::parse_code() {
    auto it_code = chunks.find("CODE");
    if (it_code == chunks.end() || it_code->second.size == 0) return;
    Chunk c = it_code->second;
    uint32_t n = u32(c.off);
    std::unordered_map<uint32_t, std::pair<uint32_t,uint32_t>> blob_cache; // blob -> (start,count)
    code.reserve(n);
    blob_cache.reserve(n);
    all_instrs.reserve(900000);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = u32(c.off + 4 + 4 * i);
        CodeEntry e;
        e.file_off = p;
        e.name = str_content(u32(p));
        e.length = u32(p + 4);
        e.locals = u16(p + 8);
        uint16_t args = u16(p + 10);
        e.weird = (args & 0x8000) != 0;
        e.args = args & 0x7FFF;
        int32_t rel = i32(p + 12);
        e.blob_off = p + 12 + rel;
        if (e.length == 0) { code.push_back(std::move(e)); continue; }
        auto it = blob_cache.find(e.blob_off);
        if (it != blob_cache.end()) {
            e.instr_start = it->second.first; e.instr_count = it->second.second;
            code.push_back(std::move(e)); continue;
        }
        uint32_t start_index = (uint32_t)all_instrs.size();
        uint32_t pos = e.blob_off, endp = e.blob_off + e.length;
        while (pos < endp) {
            Instruction ins;
            ins.file_off = pos;
            uint32_t w = u32(pos);
            ins.first_word = w;
            uint8_t t1 = (w >> 16) & 0xF;
            uint8_t k = (w >> 24) & 0xFF;
            auto is_push = [](uint8_t kk){ return kk==OP_PUSH||kk==OP_PUSHLOC||kk==OP_PUSHGLB||kk==OP_PUSHBLTN||kk==OP_PUSHI; };
            if (is_push(k)) {
                if (t1 == 0) ins.value = f64(pos + 4);
                else if (t1 == 3) ins.value = (double)u64(pos + 4);
                else if (t1 == 0x0F) ins.value = (double)(int16_t)(w & 0xFFFF);
                else { ins.raw_operand = i32(pos + 4); ins.value = ins.raw_operand; }
            } else if (k == OP_POP) {
                ins.raw_operand = (t1 == 0x0F) ? (int16_t)(w & 0xFFFF) : i32(pos + 4);
            } else if (k == OP_CALL) {
                ins.raw_operand = i32(pos + 4);
            } else if (k == OP_BREAK) {
                if (t1 == 2) ins.raw_operand = i32(pos + 4);
            }
            all_instrs.push_back(ins);
            pos += 4 * ins.size();
        }
        e.instr_start = start_index; e.instr_count = (uint32_t)all_instrs.size() - start_index;
        blob_cache[e.blob_off] = {e.instr_start, e.instr_count};
        code.push_back(std::move(e));
    }
}

void DataWin::parse_variables() {
    auto it = chunks.find("VARI");
    if (it == chunks.end() || it->second.size == 0) return;
    uint32_t o = it->second.off, sz = it->second.size;
    uint32_t p = o + 12;
    if (sz > 12) variables.reserve((sz - 12) / 20);
    while (p + 20 <= o + sz) {
        Variable v;
        v.name = str_content(u32(p));
        v.instance_type = i32(p + 4);
        v.varid = i32(p + 8);
        v.occurrences = u32(p + 12);
        v.first = i32(p + 16);
        variables.push_back(std::move(v));
        p += 20;
    }
}

void DataWin::parse_functions() {
    auto it = chunks.find("FUNC");
    if (it == chunks.end() || it->second.size == 0) return;
    uint32_t o = it->second.off;
    uint32_t n = u32(o);
    uint32_t p = o + 4;
    functions.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Function f;
        f.name = str_content(u32(p));
        f.occurrences = u32(p + 4);
        f.first = i32(p + 8);
        functions.push_back(std::move(f));
        p += 12;
    }
}

void DataWin::resolve_references() {
    auto find_ins = [&](uint32_t off) -> Instruction* {
        auto it = std::lower_bound(all_instrs.begin(), all_instrs.end(), off,
            [](const Instruction& a, uint32_t val) {
                return a.file_off < val;
            });
        if (it != all_instrs.end() && it->file_off == off) return &(*it);
        return nullptr;
    };
    log_status(" - resolving var refs...\n");
    for (int vi = 0; vi < (int)variables.size(); ++vi) {
        Variable& v = variables[vi];
        if (v.occurrences == 0 || v.first <= 0) continue;
        int32_t addr = v.first;
        for (uint32_t k = 0; k < v.occurrences; ++k) {
            Instruction* ins = find_ins((uint32_t)addr);
            if (!ins) break;
            ins->var = (int16_t)vi;
            addr += (ins->raw_operand & 0x07FFFFFF);
        }
    }
    log_status(" - resolving fn refs...\n");
    for (int fi = 0; fi < (int)functions.size(); ++fi) {
        Function& f = functions[fi];
        if (f.occurrences == 0 || f.first <= 0) continue;
        int32_t addr = f.first;
        for (uint32_t k = 0; k < f.occurrences; ++k) {
            Instruction* ins = find_ins((uint32_t)addr);
            if (!ins) break;
            ins->fun = (int16_t)fi;
            addr += (ins->raw_operand & 0x07FFFFFF);
        }
    }
    log_status(" - resolved all references!\n");
}

static std::vector<uint32_t> list_ptrs(const DataWin& dw, uint32_t off) {
    std::vector<uint32_t> out;
    uint32_t n = dw.u32_at(off);
    if (n > 2000000) n = 0;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) out.push_back(dw.u32_at(off + 4 + 4 * i));
    return out;
}

void DataWin::parse_textures() {
    auto it = chunks.find("TXTR");
    if (it == chunks.end() || it->second.size == 0) return;
    for (uint32_t p : list_ptrs(*this, it->second.off)) {
        tex_ptrs.push_back(u32(p + 4));   // pointer to the raw PNG blob
    }
}

void DataWin::parse_scripts() {
    auto it = chunks.find("SCPT");
    if (it == chunks.end() || it->second.size == 0) return;
    for (uint32_t p : list_ptrs(*this, it->second.off)) {
        scripts.emplace_back(str_content(u32(p)), (int)i32(p + 4));
    }
}

void DataWin::parse_tpag() {
    auto it_tpag = chunks.find("TPAG");
    if (it_tpag == chunks.end() || it_tpag->second.size == 0) return;
    Chunk c = it_tpag->second;
    for (uint32_t p : list_ptrs(*this, c.off)) {
        Tpag t;
        t.sx = u16(p); t.sy = u16(p+2); t.sw = u16(p+4); t.sh = u16(p+6);
        t.tx = u16(p+8); t.ty = u16(p+10); t.tw = u16(p+12); t.th = u16(p+14);
        t.bw = u16(p+16); t.bh = u16(p+18); t.tex = i16(p+20);
        tpags.push_back(t);
    }
}

void DataWin::parse_sprites() {
    auto it_sprt = chunks.find("SPRT");
    if (it_sprt == chunks.end() || it_sprt->second.size == 0) return;
    Chunk c = it_sprt->second;
    // tpag pointer -> index
    std::unordered_map<uint32_t, int> tmap;
    auto it_tpag = chunks.find("TPAG");
    if (it_tpag != chunks.end() && it_tpag->second.size > 0) {
        Chunk t = it_tpag->second;
        uint32_t tn = u32(t.off);
        for (uint32_t i = 0; i < tn; ++i) tmap[u32(t.off + 4 + 4*i)] = (int)i;
    }
    for (uint32_t p : list_ptrs(*this, c.off)) {
        Sprite s;
        s.name = str_content(u32(p));
        s.width = u32(p+4); s.height = u32(p+8);
        s.ml = i32(p+12); s.mr = i32(p+16); s.mb = i32(p+20); s.mt = i32(p+24);
        s.origin_x = i32(p+48); s.origin_y = i32(p+52);
        uint32_t tag = u32(p+56);
        uint32_t fc = 0;
        uint32_t frames_start = 0;
        if (tag == 0xFFFFFFFF) {
            // Modern GMS sprite format (bytecode 17+ / GMS2 / updated GMS 1.4)
            fc = u32(p+84);
            frames_start = p+88;
        } else {
            // Classic GMS 1.4 sprite format
            fc = tag;
            frames_start = p+60;
        }
        for (uint32_t i = 0; i < fc; ++i) {
            uint32_t fp = u32(frames_start + 4*i);
            auto it = tmap.find(fp);
            s.frames.push_back(it == tmap.end() ? -1 : it->second);
        }
        sprites.push_back(std::move(s));
    }
}

void DataWin::parse_fonts() {
    auto it = chunks.find("FONT");
    if (it == chunks.end() || it->second.size == 0) return;
    std::unordered_map<uint32_t,int> tmap;
    auto tc = chunks.find("TPAG");
    if (tc != chunks.end()) { uint32_t tn = u32(tc->second.off);
        for (uint32_t i = 0; i < tn; ++i) tmap[u32(tc->second.off + 4 + 4*i)] = (int)i; }
    for (uint32_t p : list_ptrs(*this, it->second.off)) {
        Font f;
        f.name = str_content(u32(p));
        f.em = u32(p + 8);
        auto t = tmap.find(u32(p + 28));
        f.tpag = (t == tmap.end()) ? -1 : t->second;
        f.scale_x = f32(p + 32); f.scale_y = f32(p + 36);
        uint32_t gc = 0;
        uint32_t glyphs_start = 0;
        if (bytecode_version >= 17) {
            gc = u32(p + 48);
            glyphs_start = p + 52;
        } else {
            gc = u32(p + 40);
            glyphs_start = p + 44;
        }
        if (gc < 100000) {
            for (uint32_t i = 0; i < gc; ++i) {
                uint32_t gp = u32(glyphs_start + 4*i);
                Glyph g;
                g.ch = u16(gp); g.sx = u16(gp+2); g.sy = u16(gp+4);
                g.sw = u16(gp+6); g.sh = u16(gp+8);
                g.shift = i16(gp+10); g.offset = i16(gp+12);
                f.glyphs[g.ch] = g;
            }
        }
        fonts.push_back(std::move(f));
    }
}

void DataWin::parse_bgnd() {
    auto it = chunks.find("BGND");
    if (it == chunks.end() || it->second.size == 0) return;
    std::unordered_map<uint32_t,int> tmap;
    auto tc = chunks.find("TPAG");
    if (tc != chunks.end() && tc->second.size > 0) {
        uint32_t tn = u32(tc->second.off);
        for (uint32_t i=0;i<tn;i++) tmap[u32(tc->second.off+4+4*i)] = (int)i;
    }
    for (uint32_t p : list_ptrs(*this, it->second.off)) {
        Bgnd b;
        b.name = str_content(u32(p));
        b.transparent = u32(p+4)!=0; b.smooth = u32(p+8)!=0; b.preload = u32(p+12)!=0;
        auto f = tmap.find(u32(p+16));
        b.tpag = (f==tmap.end()) ? -1 : f->second;
        bgnds.push_back(std::move(b));
    }
}

void DataWin::parse_objects() {
    auto it_obj = chunks.find("OBJT");
    if (it_obj == chunks.end() || it_obj->second.size == 0) return;
    Chunk c = it_obj->second;
    for (uint32_t p : list_ptrs(*this, c.off)) {
        ObjDef o;
        o.name = str_content(u32(p));
        uint32_t q = p + 4;
        o.sprite = i32(q); q += 4;
        o.visible = u32(q)!=0; q+=4;
        if (bytecode_version >= 17) {
            q += 4; // skip 'managed' flag (GMS 2+)
        }
        o.solid = u32(q)!=0; q+=4;
        o.depth = i32(q); q+=4;
        o.persistent = u32(q)!=0; q+=4;
        o.parent = i32(q); q+=4;
        o.mask = i32(q); q+=4;
        q += 4*8;                 // physics scalars
        int32_t vcount = i32(q); q += 4;
        q += 4*3;                 // friction/awake/kinematic
        q += 8*vcount;
        // events: outer pointer list of pointer lists of Event
        {
            uint32_t outer = u32(q);
            for (uint32_t t = 0; t < outer; ++t) {
                uint32_t elist = u32(q + 4 + 4*t);
                if (elist == 0) continue;
                uint32_t inner_n = u32(elist);
                for (uint32_t j = 0; j < inner_n; ++j) {
                    uint32_t ep = u32(elist + 4 + 4*j);
                    uint32_t subtype = u32(ep);
                    uint32_t an = u32(ep + 4);
                    for (uint32_t a = 0; a < an; ++a) {
                        uint32_t ap = u32(ep + 8 + 4*a);
                        int32_t code_id = i32(ap + 32);
                        if (code_id >= 0) o.events[t][subtype].push_back(code_id);
                    }
                }
            }
        }
        objects.push_back(std::move(o));
    }
}

void DataWin::parse_rooms() {
    auto it_room = chunks.find("ROOM");
    if (it_room == chunks.end() || it_room->second.size == 0) return;
    Chunk c = it_room->second;
    for (uint32_t p : list_ptrs(*this, c.off)) {
        Room r;
        r.name = str_content(u32(p));
        r.width = u32(p+8); r.height = u32(p+12); r.speed = u32(p+16);
        r.persistent = u32(p+20)!=0;
        r.bg_color = u32(p+24);
        r.draw_bg_color = u32(p+28)!=0;
        r.creation_code = i32(p+32);
        r.flags = u32(p+36);
        uint32_t bgp = u32(p+40), viewp = u32(p+44), objp = u32(p+48), tilep = u32(p+52);
        if (bgp) for (uint32_t bp : list_ptrs(*this, bgp)) {
            RoomBackground b;
            b.enabled = u32(bp)!=0; b.foreground = u32(bp+4)!=0;
            b.bgnd = i32(bp+8); b.x=i32(bp+12); b.y=i32(bp+16);
            b.tile_x=i32(bp+20); b.tile_y=i32(bp+24);
            b.speed_x=i32(bp+28); b.speed_y=i32(bp+32);
            b.stretch = u32(bp+36)!=0;
            r.backgrounds.push_back(b);
        }
        if (viewp) for (uint32_t vp : list_ptrs(*this, viewp)) {
            RoomView v;
            v.visible = u32(vp)!=0;
            v.x = i32(vp+4); v.y = i32(vp+8); v.w = i32(vp+12); v.h = i32(vp+16);
            v.port_x = i32(vp+20); v.port_y = i32(vp+24); v.port_w = i32(vp+28); v.port_h = i32(vp+32);
            v.border_x = i32(vp+36); v.border_y = i32(vp+40);
            v.speed_x = i32(vp+44); v.speed_y = i32(vp+48);
            v.follow_object = i32(vp+52);
            r.views.push_back(v);
        }
        if (objp) for (uint32_t ip : list_ptrs(*this, objp)) {
            RoomInstance in;
            in.x=i32(ip); in.y=i32(ip+4); in.obj=i32(ip+8); in.iid=u32(ip+12);
            in.creation_code=i32(ip+16);
            in.scale_x=f32(ip+20); in.scale_y=f32(ip+24);
            if (bytecode_version >= 17) {
                // GMS 2.2.2+ adds image_speed (f32) and image_index (i32) at ip+28, ip+32
                in.color=u32(ip+36); in.rotation=f32(ip+40);
                in.precreate_code = i32(ip+44);
            } else {
                in.color=u32(ip+28); in.rotation=f32(ip+32);
                in.precreate_code = (bytecode_version>=16) ? i32(ip+36) : -1;
            }
            r.instances.push_back(in);
        }
        if (tilep) for (uint32_t tp : list_ptrs(*this, tilep)) {
            Tile t;
            t.x=i32(tp); t.y=i32(tp+4); t.bgnd=i32(tp+8);
            t.srcx=i32(tp+12); t.srcy=i32(tp+16); t.w=i32(tp+20); t.h=i32(tp+24);
            t.depth=i32(tp+28); t.iid=u32(tp+32);
            t.scale_x=f32(tp+36); t.scale_y=f32(tp+40); t.color=u32(tp+44);
            r.tiles.push_back(t);
        }
        rooms.push_back(std::move(r));
    }
}

void DataWin::parse_sounds() {
    // SOND: uint32 count, then `count` absolute pointers to UndertaleSound objects.
    // UndertaleSound layout (bytecode >= 14, regular audio):
    //   [u32 name_str][u32 flags][u32 type_str][u32 file_str]
    //   [u32 effects][f32 volume][f32 pitch]
    //   [i32 audio_group]           (regular audio; == builtin group -> next is audo id)
    //   [i32 audio_file]            (index into AUDO)
    auto it = chunks.find("SOND");
    if (it == chunks.end()) return;
    Chunk c = it->second;
    uint32_t n = u32(c.off);
    sounds.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = u32(c.off + 4 + 4 * i);
        if (p == 0 || p >= m_file_size) continue;
        Sound s;
        uint32_t name_p = u32(p + 0);
        s.flags       = u32(p + 4);
        uint32_t type_p = u32(p + 8);
        uint32_t file_p = u32(p + 12);
        s.effects     = u32(p + 16);
        s.volume      = f32(p + 20);
        s.pitch       = f32(p + 24);
        if (name_p) s.name = str_content(name_p);
        if (type_p) s.type = str_content(type_p);
        if (file_p) s.file = str_content(file_p);
        // bytecode v16: the group id at +28; audo index at +32.
        s.audo_id = i32(p + 32);
        sounds.push_back(std::move(s));
    }
}

void DataWin::parse_audio() {
    // AUDO: uint32 count, then `count` absolute pointers to UndertaleEmbeddedAudio
    // objects: [u32 length][length bytes of raw audio], 4-byte aligned.
    auto it = chunks.find("AUDO");
    if (it == chunks.end()) return;
    Chunk c = it->second;
    uint32_t n = u32(c.off);
    audio.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = u32(c.off + 4 + 4 * i);
        EmbeddedAudio e;
        if (p != 0 && p < m_file_size) {
            uint32_t len = u32(p);
            // sanity: stay inside the file.
            if ((uint64_t)p + 4 + len <= (uint64_t)m_file_size) {
                e.offset = p + 4;
                e.length = len;
            }
        }
        audio.push_back(e);
    }
}

std::vector<uint8_t> DataWin::audio_bytes(int audo_index) const {
    std::vector<uint8_t> out;
    if (audo_index < 0 || audo_index >= (int)audio.size()) return out;
    const EmbeddedAudio& e = audio[audo_index];
    if (e.length == 0) return out;
    out.resize(e.length);
    read_at(e.offset, e.length, out.data());
    return out;
}

const Sound* DataWin::sound_by_name(const std::string& name) const {
    int i = sound_index_by_name(name);
    return i >= 0 ? &sounds[i] : nullptr;
}

int DataWin::sound_index_by_name(const std::string& name) const {
    for (size_t i = 0; i < sounds.size(); ++i)
        if (sounds[i].name == name) return (int)i;
    return -1;
}

void DataWin::build_texture_index() {
    m_tex_end.clear();
    std::vector<uint32_t> starts = tex_ptrs;
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    auto it = chunks.find("TXTR");
    uint32_t txtr_end = (it != chunks.end()) ? it->second.off + it->second.size : (uint32_t)m_file_size;
    for (size_t i = 0; i < starts.size(); ++i)
        m_tex_end[starts[i]] = (i + 1 < starts.size()) ? starts[i+1] : txtr_end;
}

Image DataWin::load_texture(int index) const {
    Image img;
    if (index < 0 || index >= (int)tex_ptrs.size()) return img;
    uint32_t start = tex_ptrs[index];
    uint32_t end = m_tex_end.count(start) ? m_tex_end.at(start) : (uint32_t)m_file_size;
    if (end <= start || end - start > 32u * 1024 * 1024) return img;
    std::vector<uint8_t> blob = bytes_at(start, end - start);
    int w, h, comp;
    unsigned char* px = stbi_load_from_memory(blob.data(), (int)blob.size(), &w, &h, &comp, 4);
    if (!px) return img;
    img.w = w; img.h = h;
    img.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return img;
}

const Image& TexturePageCache::get(int tex_index, const DataWin& dw) {
    return get_or_load(tex_index, [&](int idx) { return dw.load_texture(idx); });
}

const Image& DataWin::get_texture(int index) const {
    return m_texture_cache.get(index, *this);
}

void DataWin::set_texture_cache_capacity(size_t cap) const {
    m_texture_cache.set_capacity(cap);
}

void DataWin::clear_texture_cache() const {
    m_texture_cache.clear();
}

size_t DataWin::texture_cache_size() const {
    return m_texture_cache.size();
}

// --- compositor ---
static inline void blend_px(uint8_t* dst, const uint8_t* src, int a) {
    float sa = (src[3] / 255.0f) * (a / 255.0f);
    if (sa <= 0) return;
    float da = dst[3] / 255.0f;
    float out_a = sa + da * (1.0f - sa);
    if (out_a > 0) {
        for (int c = 0; c < 3; ++c) {
            float col = (src[c] * sa + dst[c] * da * (1.0f - sa)) / out_a;
            dst[c] = (uint8_t)std::min(255.0f, std::max(0.0f, col));
        }
        dst[3] = (uint8_t)std::min(255.0f, std::max(0.0f, out_a * 255.0f));
    }
}

static void blit(Image& dst, const Image& src, int dx, int dy, float alpha = 1.0f) {
    if (src.w <= 0 || src.h <= 0) return;
    int a = (int)(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f);
    for (int y = 0; y < src.h; ++y) {
        int ty = dy + y;
        if (ty < 0 || ty >= dst.h) continue;
        for (int x = 0; x < src.w; ++x) {
            int tx = dx + x;
            if (tx < 0 || tx >= dst.w) continue;
            blend_px(&dst.rgba[((size_t)ty * dst.w + tx) * 4],
                     &src.rgba[((size_t)y * src.w + x) * 4], a);
        }
    }
}

static Image crop(const Image& src, int x0, int y0, int w, int h) {
    Image out; out.w = w; out.h = h; out.rgba.assign((size_t)w*h*4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = x0 + x, sy = y0 + y;
            if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) continue;
            std::memcpy(&out.rgba[((size_t)y*w+x)*4], &src.rgba[((size_t)sy*src.w+sx)*4], 4);
        }
    return out;
}

static Image scale_image(const Image& src, float sx, float sy) {
    if (src.w <= 0 || src.h <= 0) return Image{};
    if (std::abs(sx - 1.0f) < 1e-4f && std::abs(sy - 1.0f) < 1e-4f) return src;

    int tw = (int)std::round(std::abs(sx) * src.w);
    int th = (int)std::round(std::abs(sy) * src.h);
    if (tw <= 0 || th <= 0) return Image{};

    Image out;
    out.w = tw;
    out.h = th;
    out.rgba.resize((size_t)tw * th * 4);

    bool flip_x = (sx < 0);
    bool flip_y = (sy < 0);

    for (int y = 0; y < th; ++y) {
        int src_y = (int)((float)y / th * src.h);
        if (src_y >= src.h) src_y = src.h - 1;
        if (flip_y) src_y = src.h - 1 - src_y;

        for (int x = 0; x < tw; ++x) {
            int src_x = (int)((float)x / tw * src.w);
            if (src_x >= src.w) src_x = src.w - 1;
            if (flip_x) src_x = src.w - 1 - src_x;

            std::memcpy(&out.rgba[((size_t)y * tw + x) * 4],
                        &src.rgba[((size_t)src_y * src.w + src_x) * 4], 4);
        }
    }
    return out;
}

static Image tint_image(const Image& src, uint32_t color) {
    if (color == 0xFFFFFFFF) return src;
    uint8_t tr = (uint8_t)(color & 0xFF);
    uint8_t tg = (uint8_t)((color >> 8) & 0xFF);
    uint8_t tb = (uint8_t)((color >> 16) & 0xFF);
    uint8_t ta = (uint8_t)((color >> 24) & 0xFF);
    if (ta == 0 && color <= 0x00FFFFFF) ta = 255;
    Image out = src;
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i + 0] = (uint8_t)((out.rgba[i + 0] * tr) / 255);
        out.rgba[i + 1] = (uint8_t)((out.rgba[i + 1] * tg) / 255);
        out.rgba[i + 2] = (uint8_t)((out.rgba[i + 2] * tb) / 255);
        if (ta != 255) {
            out.rgba[i + 3] = (uint8_t)((out.rgba[i + 3] * ta) / 255);
        }
    }
    return out;
}

Image DataWin::render_room(int room_index) const {

    const Room& room = rooms[room_index];
    Image canvas;
    canvas.w = std::max<uint32_t>(1u, room.width);
    canvas.h = std::max<uint32_t>(1u, room.height);
    canvas.rgba.assign((size_t)canvas.w * canvas.h * 4, 0);

    // Respect room.draw_bg_color flag
    if (room.draw_bg_color) {
        uint8_t bg[4] = { (uint8_t)(room.bg_color & 0xFF),
                          (uint8_t)((room.bg_color >> 8) & 0xFF),
                          (uint8_t)((room.bg_color >> 16) & 0xFF), 255 };
        for (size_t i = 0; i < canvas.rgba.size(); i += 4) std::memcpy(&canvas.rgba[i], bg, 4);
    }

    // Access texture page via DataWin's LRU cache
    auto get_page = [&](int idx) -> const Image& {
        return get_texture(idx);
    };

    // backgrounds (behind)
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& bg : room.backgrounds) {
            if (!bg.enabled) continue;
            if ((pass == 1) != bg.foreground) continue;
            if (bg.bgnd < 0 || bg.bgnd >= (int)bgnds.size()) continue;
            const Bgnd& b = bgnds[bg.bgnd];
            if (b.tpag < 0) continue;
            const Tpag* tp = tpag(b.tpag);
            if (!tp || tp->tex < 0) continue;
            const Image& pg = get_page(tp->tex);
            Image src = crop(pg, tp->sx, tp->sy, tp->sw, tp->sh);
            if (bg.stretch) {
                // nearest resize to room size
                Image rs; rs.w = room.width; rs.h = room.height; rs.rgba.assign((size_t)room.width*room.height*4,0);
                for (uint32_t y = 0; y < room.height; ++y) {
                    for (uint32_t x = 0; x < room.width; ++x) {
                        int px = (int)((float)x / room.width * src.w);
                        int py = (int)((float)y / room.height * src.h);
                        if (px >= src.w) px = src.w - 1;
                        if (py >= src.h) py = src.h - 1;
                        std::memcpy(&rs.rgba[((size_t)y * rs.w + x) * 4], &src.rgba[((size_t)py * src.w + px) * 4], 4);
                    }
                }
                blit(canvas, rs, bg.x, bg.y);
            } else {
                // Support tile_x only, tile_y only, or both correctly
                int step_x = std::max(1, src.w);
                int step_y = std::max(1, src.h);
                int start_x = bg.x;
                int end_x = bg.x + 1;
                if (bg.tile_x) {
                    start_x = bg.x % step_x;
                    if (start_x > 0) start_x -= step_x;
                    end_x = canvas.w;
                }
                int start_y = bg.y;
                int end_y = bg.y + 1;
                if (bg.tile_y) {
                    start_y = bg.y % step_y;
                    if (start_y > 0) start_y -= step_y;
                    end_y = canvas.h;
                }
                for (int cy = start_y; cy < end_y; cy += step_y) {
                    for (int cx = start_x; cx < end_x; cx += step_x) {
                        blit(canvas, src, cx, cy);
                    }
                }
            }
        }
        if (pass == 0) {
            // tiles (behind instances)
            std::vector<const Tile*> ts;
            for (const auto& tl : room.tiles) ts.push_back(&tl);
            std::sort(ts.begin(), ts.end(), [](const Tile* a, const Tile* b){ return a->depth > b->depth; });
            for (const Tile* tl : ts) {
                if (tl->bgnd < 0 || tl->bgnd >= (int)bgnds.size()) continue;
                const Bgnd& b = bgnds[tl->bgnd];
                if (b.tpag < 0) continue;
                const Tpag* tp = tpag(b.tpag);
                if (!tp || tp->tex < 0) continue;
                const Image& pg = get_page(tp->tex);
                // Tile Texture Atlas UV Fix: sampling from (tp->sx + tl->srcx, tp->sy + tl->srcy)
                int sheet_x = tp->sx + tl->srcx;
                int sheet_y = tp->sy + tl->srcy;
                if (tl->w <= 0 || tl->h <= 0) continue;
                if (sheet_x >= pg.w || sheet_y >= pg.h || sheet_x + tl->w <= 0 || sheet_y + tl->h <= 0) continue;
                Image src = crop(pg, sheet_x, sheet_y, tl->w, tl->h);
                if (tl->scale_x != 1.0f || tl->scale_y != 1.0f) {
                    src = scale_image(src, tl->scale_x, tl->scale_y);
                }
                if (tl->color != 0xFFFFFFFF) {
                    src = tint_image(src, tl->color);
                }
                int tx = tl->x;
                int ty = tl->y;
                if (tl->scale_x < 0) tx += (int)std::round(tl->scale_x * tl->w);
                if (tl->scale_y < 0) ty += (int)std::round(tl->scale_y * tl->h);
                blit(canvas, src, tx, ty);
            }
        }
    }

    // instances sorted by depth (higher first)
    std::vector<const RoomInstance*> ordered;
    for (const auto& in : room.instances) ordered.push_back(&in);
    std::sort(ordered.begin(), ordered.end(), [&](const RoomInstance* a, const RoomInstance* b) {
        int da = (a->obj >= 0 && a->obj < (int)objects.size()) ? objects[a->obj].depth : 0;
        int db = (b->obj >= 0 && b->obj < (int)objects.size()) ? objects[b->obj].depth : 0;
        return da > db;
    });
    for (const RoomInstance* in : ordered) {
        if (in->obj < 0 || in->obj >= (int)objects.size()) continue;
        const ObjDef& od = objects[in->obj];
        if (!od.visible) continue;
        if (od.sprite < 0 || od.sprite >= (int)sprites.size()) continue;
        const Sprite& sp = sprites[od.sprite];
        if (sp.frames.empty() || sp.frames[0] < 0) continue;
        const Tpag* tp = tpag(sp.frames[0]);
        if (!tp || tp->tex < 0) continue;
        const Image& pg = get_page(tp->tex);
        Image src = crop(pg, tp->sx, tp->sy, tp->sw, tp->sh);
        if (in->scale_x != 1.0f || in->scale_y != 1.0f) {
            src = scale_image(src, in->scale_x, in->scale_y);
        }
        if (in->color != 0xFFFFFFFF) {
            src = tint_image(src, in->color);
        }
        int dx = in->x + (int)std::round(in->scale_x >= 0 ? (tp->tx - sp.origin_x) * in->scale_x : (tp->tx + tp->sw - sp.origin_x) * in->scale_x);
        int dy = in->y + (int)std::round(in->scale_y >= 0 ? (tp->ty - sp.origin_y) * in->scale_y : (tp->ty + tp->sh - sp.origin_y) * in->scale_y);
        blit(canvas, src, dx, dy, 1.0f);
    }

    return canvas;
}

} // namespace gm14
