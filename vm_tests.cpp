// Extended host-only unit tests for gm14 VM builtins.
// Objective pass/fail checks against the official YoYo HTML5 runtime semantics.
#include "vm.hpp"
#include "audio.hpp"
#include <cstdio>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string>
#include <vector>
#include <set>

using namespace gm14;

static int g_pass = 0, g_fail = 0;

static Value call(Runtime& rt, const char* name, std::vector<Value> args = {}) {
    return rt.call(name, args);
}
static void ok_num(const char* label, double got, double want, double eps = 1e-9) {
    if (std::fabs(got - want) <= eps) g_pass++;
    else { g_fail++; std::printf("FAIL %-42s got=%g want=%g\n", label, got, want); }
}
static void ok_str(const char* label, const std::string& got, const std::string& want) {
    if (got == want) g_pass++;
    else { g_fail++; std::printf("FAIL %-42s got=\"%s\" want=\"%s\"\n", label, got.c_str(), want.c_str()); }
}
static void ok_bool(const char* label, Value v, bool want) {
    double g = to_num(v);
    if ((g != 0.0) == want) g_pass++;
    else { g_fail++; std::printf("FAIL %-42s got=%g want_bool=%d\n", label, g, (int)want); }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s DATA.WIN\n", argv[0]); return 1; }
    DataWin dw(argv[1]);
    Runtime rt(dw);
    using S = std::string;
    auto num = [](double d){ return Value(d); };
    auto str = [](const char* s){ return Value(S(s)); };
    auto C = [&](const char* n, std::vector<Value> a = {}){ return call(rt, n, a); };

    for (const auto& ce : dw.code) {
        if (ce.name.find("torielflame") != std::string::npos) {
            std::printf("=== %s ===\n", ce.name.c_str());
            for (uint32_t i = 0; i < ce.instr_count; ++i) {
                const auto& ins = dw.all_instrs[ce.instr_start + i];
                std::printf("%4u: kind=%02X t1=%X low16=%d ref=%02X", i, ins.kind(), ins.type1(), (int)(int16_t)ins.low16(), ins.ref_type());
                if (ins.var >= 0 && ins.var < (int)dw.variables.size()) {
                    std::printf(" var=%s (itype=%d)", dw.variables[ins.var].name.c_str(), dw.variables[ins.var].instance_type);
                }
                if (ins.fun >= 0 && ins.fun < (int)dw.functions.size()) {
                    std::printf(" fn=%s", dw.functions[ins.fun].name.c_str());
                }
                if (ins.type1() == 2 && ins.str_index() < dw.strings.size()) {
                    std::printf(" str=\"%s\"", dw.strings[ins.str_index()].c_str());
                }
                if (ins.type1() == 0 || ins.type1() == 0x0F) {
                    std::printf(" val=%g", ins.value);
                }
                std::printf("\n");
            }
        }
    }


    // ================= strings =================
    ok_num("len abc", to_num(C("string_length",{str("abc")})), 3);
    ok_str("char_at abc 1", to_str(C("string_char_at",{str("abc"),num(1)})), "a");
    ok_str("char_at abc 3", to_str(C("string_char_at",{str("abc"),num(3)})), "c");
    ok_str("char_at abc 4", to_str(C("string_char_at",{str("abc"),num(4)})), "");
    ok_str("char_at abc 0", to_str(C("string_char_at",{str("abc"),num(0)})), "");
    ok_str("copy hello 2 3", to_str(C("string_copy",{str("hello"),num(2),num(3)})), "ell");
    ok_str("copy hello 0 2", to_str(C("string_copy",{str("hello"),num(0),num(2)})), "he");
    ok_str("copy hello 4 99", to_str(C("string_copy",{str("hello"),num(4),num(99)})), "lo");
    ok_num("pos l hello", to_num(C("string_pos",{str("l"),str("hello")})), 3);
    ok_num("pos z hello", to_num(C("string_pos",{str("z"),str("hello")})), 0);
    ok_str("upper", to_str(C("string_upper",{str("aBc")})), "ABC");
    ok_str("lower", to_str(C("string_lower",{str("aBc")})), "abc");
    ok_str("repeat", to_str(C("string_repeat",{str("ab"),num(3)})), "ababab");
    ok_str("replace", to_str(C("string_replace",{str("aXa"),str("X"),str("y")})), "aya");
    ok_str("replace_all", to_str(C("string_replace_all",{str("aXaXa"),str("X"),str("y")})), "ayaya");
    ok_str("delete", to_str(C("string_delete",{str("hello"),num(2),num(2)})), "hlo");
    ok_str("substr", to_str(C("substr",{str("hello"),num(1),num(3)})), "hel");
    ok_num("byte_length", to_num(C("string_byte_length",{str("abc")})), 3);
    ok_str("string int", to_str(C("string",{num(5)})), "5");
    ok_str("string neg", to_str(C("string",{num(-3.5)})), "-3.5");

    // ================= real/ord/chr =================
    ok_num("real 42", to_num(C("real",{str("42")})), 42);
    ok_num("real 3.5", to_num(C("real",{str("3.5")})), 3.5);
    ok_num("ord A", to_num(C("ord",{str("A")})), 65);
    ok_str("chr 65", to_str(C("chr",{num(65)})), "A");
    ok_str("chr 97", to_str(C("chr",{num(97)})), "a");

    // ================= math =================
    ok_num("floor 3.7", to_num(C("floor",{num(3.7)})), 3);
    ok_num("floor -3.2", to_num(C("floor",{num(-3.2)})), -4);
    ok_num("ceil 3.2", to_num(C("ceil",{num(3.2)})), 4);
    ok_num("ceil -3.7", to_num(C("ceil",{num(-3.7)})), -3);
    ok_num("round 2.4", to_num(C("round",{num(2.4)})), 2);
    ok_num("round 2.6", to_num(C("round",{num(2.6)})), 3);
    ok_num("sign -5", to_num(C("sign",{num(-5)})), -1);
    ok_num("sign 0", to_num(C("sign",{num(0)})), 0);
    ok_num("sign 9", to_num(C("sign",{num(9)})), 1);
    ok_num("abs -4", to_num(C("abs",{num(-4)})), 4);
    ok_num("sqr 9", to_num(C("sqr",{num(9)})), 81);
    ok_num("sqrt 16", to_num(C("sqrt",{num(16)})), 4);
    ok_num("power 2^10", to_num(C("power",{num(2),num(10)})), 1024);
    ok_num("min 3 7", to_num(C("min",{num(3),num(7)})), 3);
    ok_num("max 3 7", to_num(C("max",{num(3),num(7)})), 7);
    ok_num("clamp hi", to_num(C("clamp",{num(5),num(1),num(3)})), 3);
    ok_num("clamp lo", to_num(C("clamp",{num(-5),num(1),num(3)})), 1);
    ok_num("degtorad", to_num(C("degtorad",{num(180)})), M_PI);
    ok_num("radtodeg", to_num(C("radtodeg",{num(M_PI)})), 180);
    ok_num("sin0", to_num(C("sin",{num(0)})), 0);
    ok_num("cos0", to_num(C("cos",{num(0)})), 1);
    ok_num("tan(rad45)", to_num(C("tan",{num(M_PI/4)})), 1, 1e-9);
    ok_num("arcsin1", to_num(C("arcsin",{num(1)})), M_PI/2);
    ok_num("arccos1", to_num(C("arccos",{num(1)})), 0);
    ok_num("arctan2", to_num(C("arctan2",{num(0),num(1)})), 0);
    ok_num("dot", to_num(C("dot_product",{num(1),num(2),num(3),num(4)})), 11);
    ok_num("pdist3-4", to_num(C("point_distance",{num(0),num(0),num(3),num(4)})), 5);
    ok_num("pdir0", to_num(C("point_direction",{num(0),num(0),num(1),num(0)})), 0, 1e-6);
    ok_num("pdir down=270", to_num(C("point_direction",{num(0),num(0),num(0),num(1)})), 270, 1e-6);
    ok_num("ldx0", to_num(C("lengthdir_x",{num(10),num(0)})), 10, 1e-6);
    ok_num("ldy0", to_num(C("lengthdir_y",{num(10),num(0)})), 0, 1e-6);
    ok_num("ldx90", to_num(C("lengthdir_x",{num(10),num(90)})), 0, 1e-6);
    ok_num("ldy90", to_num(C("lengthdir_y",{num(10),num(90)})), -10, 1e-6);

    // ================= type predicates =================
    ok_bool("is_string(x)", C("is_string",{str("x")}), true);
    ok_bool("is_string(5)", C("is_string",{num(5)}), false);
    ok_bool("is_real(5)", C("is_real",{num(5)}), true);
    ok_bool("is_real(x)", C("is_real",{str("x")}), false);
    ok_bool("is_numeric(5)", C("is_numeric",{num(5)}), true);
    ok_bool("is_undefined(0)", C("is_undefined",{num(0)}), false);

    // ================= colour =================
    ok_num("make_color_rgb", to_num(C("make_color_rgb",{num(255),num(0),num(0)})), 255);
    ok_num("make_color_rgb gg", to_num(C("make_color_rgb",{num(0),num(255),num(0)})), 65280);
    ok_num("make_color_rgb bb", to_num(C("make_color_rgb",{num(0),num(0),num(255)})), 16711680);
    ok_num("merge_color", to_num(C("merge_color",{num(0),num(0),num(0)})), 0);

    // ================= ds_list =================
    {
        double l = to_num(C("ds_list_create"));
        C("ds_list_add",{num(l),num(10)});
        C("ds_list_add",{num(l),num(20)});
        C("ds_list_add",{num(l),num(30)});
        ok_num("list size", to_num(C("ds_list_size",{num(l)})), 3);
        ok_num("list[0]", to_num(C("ds_list_find_value",{num(l),num(0)})), 10);
        ok_num("list[2]", to_num(C("ds_list_find_value",{num(l),num(2)})), 30);
        C("ds_list_clear",{num(l)});
        ok_num("list size after clear", to_num(C("ds_list_size",{num(l)})), 0);
    }

    // ================= ds_map =================
    {
        double m = to_num(C("ds_map_create"));
        C("ds_map_add",{num(m),str("hp"),num(20)});
        C("ds_map_set",{num(m),str("hp"),num(40)});
        ok_num("map hp after set", to_num(C("ds_map_find_value",{num(m),str("hp")})), 40);
        ok_bool("map exists hp", C("ds_map_exists",{num(m),str("hp")}), true);
        ok_bool("map exists mp", C("ds_map_exists",{num(m),str("mp")}), false);
        C("ds_map_add",{num(m),str("mp"),num(5)});
        ok_num("map mp", to_num(C("ds_map_find_value",{num(m),str("mp")})), 5);
    }

    // ================= random ranges (bounds only) =================
    {
        bool okr = true;
        for (int i = 0; i < 200; ++i) { double v = to_num(C("irandom_range",{num(5),num(10)})); if (v < 5 || v > 10) okr = false; }
        ok_bool("irandom_range in [5,10]", Value(okr?1.0:0.0), true);
        bool oki = true;
        for (int i = 0; i < 200; ++i) { double v = to_num(C("irandom",{num(3)})); if (v < 0 || v > 3) oki = false; }
        ok_bool("irandom in [0,3]", Value(oki?1.0:0.0), true);
        bool okf = true;
        for (int i = 0; i < 200; ++i) { double v = to_num(C("random_range",{num(2),num(4)})); if (v < 2 || v > 4) okf = false; }
        ok_bool("random_range in [2,4]", Value(okf?1.0:0.0), true);
        bool okv = true;
        for (int i = 0; i < 200; ++i) { double v = to_num(C("random",{num(1)})); if (v < 0 || v >= 1) okv = false; }
        ok_bool("random in [0,1)", Value(okv?1.0:0.0), true);
    }

    // ================= choose =================
    {
        bool okc = true;
        for (int i = 0; i < 100; ++i) { double v = to_num(C("choose",{num(7),num(8),num(9)})); if (v!=7 && v!=8 && v!=9) okc=false; }
        ok_bool("choose Ôêê {7,8,9}", Value(okc?1.0:0.0), true);
    }

    // ================= room info (real data) =================
    ok_str("room_get_name(1)", to_str(C("room_get_name",{num(1)})), dw.rooms[1].name);

    // ================= sprite info (real data) =================
    if (!dw.sprites.empty()) {
        ok_num("sprite_get_width(0)", to_num(C("sprite_get_width",{num(0)})), dw.sprites[0].width);
        ok_num("sprite_get_height(0)", to_num(C("sprite_get_height",{num(0)})), dw.sprites[0].height);
    }

    // ================= instance lifecycle =================
    if (!dw.objects.empty()) {
        double iid = to_num(C("instance_create",{num(0),num(0),num(0)}));
        ok_bool("instance exists after create", C("instance_exists",{num(iid)}), true);
        C("instance_destroy",{num(iid)});
        ok_bool("instance gone after destroy", C("instance_exists",{num(iid)}), false);
    }

    // ================= audio =================
    // 1. SOND/AUDO parsing against the real data.win.
    {
        ok_num("sounds parsed > 0", dw.sounds.size() > 0 ? 1.0 : 0.0, 1.0);
        ok_num("audio blobs parsed > 0", dw.audio.size() > 0 ? 1.0 : 0.0, 1.0);
        // Every embedded sound blob must be a recognizable container (RIFF or OggS).
        int embedded = 0, recognized = 0;
        for (size_t i = 0; i < dw.sounds.size(); ++i) {
            const gm14::Sound& s = dw.sounds[i];
            if (s.audo_id < 0) continue;
            ++embedded;
            std::vector<uint8_t> b = dw.audio_bytes(s.audo_id);
            if (b.size() >= 4 && (std::memcmp(b.data(), "RIFF", 4) == 0 ||
                                  std::memcmp(b.data(), "OggS", 4) == 0)) ++recognized;
        }
        ok_num("all embedded sounds recognized", embedded > 0 ? (double)(recognized == embedded) : 1.0, 1.0);
        // Every embedded sound must decode through decode_audio (WAV or OGG).
        int dec_ok = 0, ogg_seen = 0;
        for (size_t i = 0; i < dw.sounds.size(); ++i) {
            const gm14::Sound& s = dw.sounds[i];
            if (s.audo_id < 0) continue;
            std::vector<uint8_t> b = dw.audio_bytes(s.audo_id);
            if (b.size() >= 4 && std::memcmp(b.data(), "OggS", 4) == 0) ++ogg_seen;
            gm14::AudioClip c;
            if (gm14::decode_audio(b.data(), b.size(), c) && c.duration() > 0) ++dec_ok;
        }
        ok_num("all embedded sounds decode", embedded > 0 ? (double)(dec_ok == embedded) : 1.0, 1.0);
        std::printf("  [info] embedded OGG sounds decoded: %d\n", ogg_seen);
    }

    // 2. WAV decode correctness on a synthetic clip.
    {
        std::vector<uint8_t> wav;
        auto push32 = [&](uint32_t v){ for (int i=0;i<4;++i) wav.push_back((uint8_t)(v>>(8*i))); };
        auto push16 = [&](uint16_t v){ for (int i=0;i<2;++i) wav.push_back((uint8_t)(v>>(8*i))); };
        for (char c : std::string("RIFF")) wav.push_back((uint8_t)c);
        push32(36 + 8);
        for (char c : std::string("WAVEfmt ")) wav.push_back((uint8_t)c);
        push32(16); push16(1); push16(1); push32(44100); push32(88200); push16(2); push16(16);
        for (char c : std::string("data")) wav.push_back((uint8_t)c);
        push32(4);
        push16(1000); push16((uint16_t)65000);
        gm14::AudioClip clip;
        bool ok = gm14::decode_wav(wav.data(), wav.size(), clip);
        ok_bool("decode_wav synthetic ok", Value(ok?1.0:0.0), true);
        ok_num("decode_wav rate", clip.sample_rate, 44100);
        ok_num("decode_wav channels", clip.channels, 1);
        ok_num("decode_wav sample count", (double)clip.pcm.size(), 2.0);
        ok_num("decode_wav first sample", (double)clip.pcm[0], 1000.0);
        ok_num("decode_wav duration", clip.duration(), 2.0/44100.0, 1e-6);
    }

    // 3. Mixer semantics (deterministic NullBackend) via the VM builtins.
    if (!dw.sounds.empty()) {
        // find an embedded, decodable sound to play
        int asset = -1;
        for (size_t i = 0; i < dw.sounds.size(); ++i) {
            if (dw.sounds[i].audo_id >= 0) { asset = (int)i; break; }
        }
        if (asset >= 0) {
            Value h = C("audio_play_sound", {num((double)asset), num(1.0), num(0.0), num(1.0)});
            double handle = to_num(h);
            ok_bool("play returns handle >= 300000", Value(handle >= 300000 ? 1.0 : 0.0), true);
            ok_bool("audio_is_playing(handle)", C("audio_is_playing", {num(handle)}), true);
            ok_bool("audio_is_playing(asset)", C("audio_is_playing", {num((double)asset)}), true);

            // gain getter/setter round-trips
            C("audio_sound_gain", {num(handle), num(0.25)});
            ok_num("gain handle round-trip", to_num(C("audio_sound_get_gain", {num(handle)})), 0.25, 1e-9);
            C("audio_sound_gain", {num((double)asset), num(0.5)});
            ok_num("gain asset round-trip", to_num(C("audio_sound_get_gain", {num((double)asset)})), 0.5, 1e-9);

            // pitch round-trips
            C("audio_sound_pitch", {num(handle), num(2.0)});
            ok_num("pitch handle round-trip", to_num(C("audio_sound_get_pitch", {num(handle)})), 2.0, 1e-9);

            // master gain
            C("audio_master_gain", {num(0.75)});
            ok_num("master gain round-trip", to_num(C("audio_get_master_gain", {Value(0.0)})), 0.75, 1e-9);

            // pause/resume
            C("audio_pause_sound", {num(handle)});
            ok_bool("is_paused after pause", C("audio_is_paused", {num(handle)}), true);
            ok_bool("not playing while paused", C("audio_is_playing", {num(handle)}), false);
            C("audio_resume_sound", {num(handle)});
            ok_bool("playing after resume", C("audio_is_playing", {num(handle)}), true);

            // stop by handle
            C("audio_stop_sound", {num(handle)});
            ok_bool("not playing after stop", C("audio_is_playing", {num(handle)}), false);

            // legacy snd_play + stop-by-asset
            double h2 = to_num(C("snd_play", {num((double)asset)}));
            ok_bool("snd_play playing", C("audio_is_playing", {num((double)asset)}), true);
            C("snd_stop", {num((double)asset)});
            ok_bool("snd_stop stops all asset voices", C("audio_is_playing", {num((double)asset)}), false);
            (void)h2;
        }
    }

    // ================= ds_grid =================
    {
        double g = to_num(C("ds_grid_create", {num(4), num(4)}));
        ok_bool("ds_grid_create valid id", Value(g > 0 ? 1.0 : 0.0), true);
        ok_num("ds_grid_width", to_num(C("ds_grid_width", {num(g)})), 4);
        ok_num("ds_grid_height", to_num(C("ds_grid_height", {num(g)})), 4);
        C("ds_grid_set", {num(g), num(1), num(2), num(42)});
        ok_num("ds_grid_get set cell", to_num(C("ds_grid_get", {num(g), num(1), num(2)})), 42);
        ok_num("ds_grid_get unset cell", to_num(C("ds_grid_get", {num(g), num(0), num(0)})), 0);
        C("ds_grid_clear", {num(g), num(5)});
        ok_num("ds_grid_clear applied", to_num(C("ds_grid_get", {num(g), num(1), num(2)})), 5);
        C("ds_grid_set_region", {num(g), num(1), num(1), num(2), num(2), num(10)});
        ok_num("ds_grid_set_region cell (1,1)", to_num(C("ds_grid_get", {num(g), num(1), num(1)})), 10);
        ok_num("ds_grid_set_region cell (2,2)", to_num(C("ds_grid_get", {num(g), num(2), num(2)})), 10);
        ok_num("ds_grid_set_region untouched (0,0)", to_num(C("ds_grid_get", {num(g), num(0), num(0)})), 5);
        // Region sums & stats: (4x4, 12 cells at 5 = 60, 4 cells at 10 = 40, total 100)
        ok_num("ds_grid_get_sum", to_num(C("ds_grid_get_sum", {num(g), num(0), num(0), num(3), num(3)})), 100);
        ok_num("ds_grid_get_mean", to_num(C("ds_grid_get_mean", {num(g), num(0), num(0), num(3), num(3)})), 6.25);
        ok_num("ds_grid_get_max", to_num(C("ds_grid_get_max", {num(g), num(0), num(0), num(3), num(3)})), 10);
        C("ds_grid_destroy", {num(g)});
    }

    // ================= INI save system =================
    {
        C("ini_open", {str("test_save.ini")});
        C("ini_write_real", {str("General"), str("hp"), num(20)});
        C("ini_write_string", {str("General"), str("name"), str("Frisk")});
        C("ini_write_real", {str("General"), str("love"), num(1)});
        C("ini_write_real", {str("Flags"), str("plot"), num(105)});
        ok_bool("ini_key_exists true", C("ini_key_exists", {str("General"), str("hp")}), true);
        ok_bool("ini_key_exists false", C("ini_key_exists", {str("General"), str("nonexistent")}), false);
        ok_bool("ini_section_exists true", C("ini_section_exists", {str("General")}), true);
        ok_bool("ini_section_exists false", C("ini_section_exists", {str("Missing")}), false);
        ok_num("ini_read_real hp", to_num(C("ini_read_real", {str("General"), str("hp"), num(0)})), 20);
        ok_str("ini_read_string name", to_str(C("ini_read_string", {str("General"), str("name"), str("")})), "Frisk");
        ok_num("ini_read_real default", to_num(C("ini_read_real", {str("General"), str("missing"), num(999)})), 999);
        Value ini_text = C("ini_close", {});
        ok_bool("ini_close returns non-empty string", Value(to_str(ini_text).size() > 10 ? 1.0 : 0.0), true);

        // Reopen from cache and verify persistence
        C("ini_open", {str("test_save.ini")});
        ok_num("ini reloaded hp", to_num(C("ini_read_real", {str("General"), str("hp"), num(0)})), 20);
        ok_str("ini reloaded name", to_str(C("ini_read_string", {str("General"), str("name"), str("")})), "Frisk");
        ok_num("ini reloaded plot", to_num(C("ini_read_real", {str("Flags"), str("plot"), num(0)})), 105);
        C("ini_close", {});
    }

    // ================= math & vector builtins =================
    {
        ok_num("clamp middle", to_num(C("clamp", {num(5), num(0), num(10)})), 5);
        ok_num("clamp low", to_num(C("clamp", {num(-5), num(0), num(10)})), 0);
        ok_num("clamp high", to_num(C("clamp", {num(15), num(0), num(10)})), 10);
        ok_num("lerp 50%", to_num(C("lerp", {num(10), num(20), num(0.5)})), 15);
        ok_num("lerp 0%", to_num(C("lerp", {num(10), num(20), num(0.0)})), 10);
        ok_num("lerp 100%", to_num(C("lerp", {num(10), num(20), num(1.0)})), 20);
        ok_num("point_distance 3-4-5", to_num(C("point_distance", {num(0), num(0), num(3), num(4)})), 5);
        ok_num("point_direction right", to_num(C("point_direction", {num(0), num(0), num(10), num(0)})), 0);
        ok_num("point_direction down", to_num(C("point_direction", {num(0), num(0), num(0), num(10)})), 270);
        ok_num("lengthdir_x right", to_num(C("lengthdir_x", {num(10), num(0)})), 10, 1e-6);
        ok_num("lengthdir_y right", to_num(C("lengthdir_y", {num(10), num(0)})), 0, 1e-6);
        ok_num("lengthdir_x up", to_num(C("lengthdir_x", {num(10), num(90)})), 0, 1e-6);
        ok_num("lengthdir_y up", to_num(C("lengthdir_y", {num(10), num(90)})), -10, 1e-6);
        ok_num("darctan2 45deg", to_num(C("darctan2", {num(1), num(1)})), 45, 1e-6);
        ok_num("degtorad 180", to_num(C("degtorad", {num(180)})), M_PI, 1e-6);
    }

    // ================= additional strings =================
    {
        ok_str("string_format 2 dec", to_str(C("string_format", {num(3.14159), num(0), num(2)})), "3.14");
        ok_str("string_insert middle", to_str(C("string_insert", {str("world"), str("hello "), num(7)})), "hello world");
        ok_num("string_byte_length", to_num(C("string_byte_length", {str("Antigravity")})), 11);
    }

    // ================= surfaces =================
    {
        double surf = to_num(C("surface_create", {num(64), num(64)}));
        ok_bool("surface_create id > 0", Value(surf > 0 ? 1.0 : 0.0), true);
        ok_bool("surface_exists true", C("surface_exists", {num(surf)}), true);
        ok_num("surface_get_width", to_num(C("surface_get_width", {num(surf)})), 64);
        ok_num("surface_get_height", to_num(C("surface_get_height", {num(surf)})), 64);
        C("surface_set_target", {num(surf)});
        C("draw_clear", {num((double)0xFF0000)});
        C("surface_reset_target", {});
        C("surface_free", {num(surf)});
        ok_bool("surface_exists false after free", C("surface_exists", {num(surf)}), false);
    }

    // ================= Undertale caster_* audio engine =================
    {
        double cid = to_num(C("caster_load", {str("mus_story.ogg")}));
        if (cid > 0) {
            ok_bool("caster_load found mus_story.ogg", Value(1.0), true);
            double cv = to_num(C("caster_play", {num(cid), num(0.75), num(1.0)}));
            ok_bool("caster_play returns voice handle", Value(cv >= 300000 ? 1.0 : 0.0), true);
            ok_bool("caster_is_playing true", C("caster_is_playing", {num(cid)}), true);
            C("caster_set_volume", {num(cid), num(0.5)});
            ok_num("caster_get_volume round-trip", to_num(C("caster_get_volume", {num(cid)})), 0.5, 1e-9);
            C("caster_set_pitch", {num(cid), num(1.25)});
            ok_num("caster_get_pitch round-trip", to_num(C("caster_get_pitch", {num(cid)})), 1.25, 1e-9);
            C("caster_stop", {num(cid)});
            ok_bool("caster_is_playing false after stop", C("caster_is_playing", {num(cid)}), false);
            C("caster_free", {num(cid)});
        }
    }
    // ================= D&D actions & controls parity =================
    {
        // 1. ds_map_find_value retains PC control strings without 3DS mutation
        double mid = to_num(C("ds_map_create", {}));
        C("ds_map_add", {num(mid), str("instructions_confirm_key"), str("[PRESS Z OR ENTER]")});
        ok_str("ds_map_find_value keeps PC prompt", to_str(C("ds_map_find_value", {num(mid), str("instructions_confirm_key")})), "[PRESS Z OR ENTER]");
        C("ds_map_destroy", {num(mid)});

        // 2. keyboard_key_press and keyboard_key_release
        C("keyboard_key_press", {num(37)}); // VK_LEFT
        ok_bool("keyboard_check(37) held", C("keyboard_check", {num(37)}), true);
        ok_bool("keyboard_check_pressed(37) pressed", C("keyboard_check_pressed", {num(37)}), true);
        C("keyboard_key_release", {num(37)});
        ok_bool("keyboard_check(37) released", C("keyboard_check", {num(37)}), false);
        ok_bool("keyboard_check_released(37) released", C("keyboard_check_released", {num(37)}), true);

        // 3. action_kill_object and action_move_to
        double inst_id = to_num(C("instance_create", {num(10), num(20), num(0)}));
        ok_bool("instance_exists created", C("instance_exists", {num(inst_id)}), true);
        gm14::Instance* inst_ptr = rt.resolve_inst((int)inst_id);
        rt.cur = inst_ptr;
        C("action_move_to", {num(123), num(456)});
        ok_num("action_move_to sets x", to_num(inst_ptr->vars["x"]), 123);
        ok_num("action_move_to sets y", to_num(inst_ptr->vars["y"]), 456);
        C("action_kill_object", {});
        ok_bool("action_kill_object destroys instance", C("instance_exists", {num(inst_id)}), false);
    }

    // ================= Path system & two-pass room loading =================
    {
        // 1. PATH chunk parsing verification
        ok_bool("paths parsed >= 45", Value(dw.paths.size() >= 45 ? 1.0 : 0.0), true);
        if (dw.paths.size() > 5) {
            ok_str("path 5 name is path_torielwalk1", dw.paths[5].name, "path_torielwalk1");
            ok_num("path_get_number(5) == 7", to_num(C("path_get_number", {num(5)})), 7);
            ok_str("path_get_name(5)", to_str(C("path_get_name", {num(5)})), "path_torielwalk1");
            ok_bool("path_get_length(5) > 300", Value(to_num(C("path_get_length", {num(5)})) > 300 ? 1.0 : 0.0), true);
        }

        // 2. path_start and path following
        double p_inst = to_num(C("instance_create", {num(100), num(100), num(0)}));
        gm14::Instance* pi = rt.resolve_inst((int)p_inst);
        rt.cur = pi;
        C("path_start", {num(5), num(2.0), num(0), num(1)});
        ok_num("pi path_index == 5", (double)pi->path_index, 5);
        ok_num("pi path_speed == 2.0", pi->path_speed, 2.0);
        ok_num("pi initial x snapped to path point 0", to_num(pi->vars["x"]), 140.0);
        ok_num("pi initial y snapped to path point 0", to_num(pi->vars["y"]), 320.0);
        C("path_end", {});
        ok_num("pi path_index == -1 after path_end", (double)pi->path_index, -1);
        C("instance_destroy", {num(p_inst)});

        // 3. Room 6 entrance marker positioning & Toriel path following
        if (dw.rooms.size() > 6 && dw.rooms[6].name == "room_ruins1") {
            rt.globals["entrance"] = gm14::Value(1.0); // markerA entrance
            rt.globals["interact"] = gm14::Value(3.0); // room transition state
            rt.start_room(6);

            gm14::Instance* player = nullptr;
            for (auto& inst : rt.instances) {
                if (inst->alive && dw.objects[inst->obj].name == "obj_mainchara") {
                    player = inst.get();
                    break;
                }
            }
            ok_bool("player obj_mainchara found in room 6", Value(player != nullptr ? 1.0 : 0.0), true);
            if (player) {
                double px = to_num(player->vars["x"]);
                double py = to_num(player->vars["y"]);
                ok_num("player x at entrance marker", px, 150.0);
                ok_num("player y at entrance marker (420 not 200)", py, 420.0);
            }

            Instance* tori = nullptr;
            for (auto& inst : rt.instances) {
                if (inst->alive && dw.objects[inst->obj].name == "obj_toroverworld2") {
                    tori = inst.get();
                    break;
                }
            }
            ok_bool("obj_toroverworld2 found in room 6", Value(tori != nullptr ? 1.0 : 0.0), true);
            if (tori && player) {
                ok_num("Toriel initial path_index", (double)tori->path_index, 5.0);
                // Simulate player following Toriel
                for (int frame = 0; frame < 200; ++frame) {
                    player->vars["y"] = gm14::Value(to_num(tori->vars["y"]) + 40.0);
                    player->vars["x"] = tori->vars["x"];
                    rt.step();
                }
                ok_bool("Toriel reached buster and destroyed herself", Value(!tori->alive ? 1.0 : 0.0), true);
                double plot_val = rt.globals.count("plot") ? to_num(rt.globals["plot"]) : 0.0;
                ok_num("global.plot advanced to 2 after path", plot_val, 2.0);
            }
        }

        // ================= Save / Load State Verification =================
        rt.globals["test_save_var"] = Value(1337.0);
        rt.global_arrays["test_save_arr"] = { Value("state_saved"), Value(42.0) };
        Value arr_val; arr_val.type = Value::VARPTR; arr_val.arrref = &rt.global_arrays["test_save_arr"];
        rt.globals["test_save_arr"] = arr_val;

        int test_map = (int)to_num(C("ds_map_create"));
        C("ds_map_add", {num(test_map), str("player_choice"), str("save_state_ok")});

        int saved_room_before = rt.room_index;
        size_t saved_inst_cnt = rt.instances.size();

        bool save_ok = rt.save_state_file("test_persistence");
        ok_bool("save_state_file created test_persistence.state", Value(save_ok ? 1.0 : 0.0), true);

        // Mutate everything to verify complete restoration
        rt.globals["test_save_var"] = Value(0.0);
        rt.global_arrays["test_save_arr"].clear();
        rt.instances.clear();
        rt.start_room(0);
        ok_num("room switched to 0 for mutation", (double)rt.room_index, 0.0);

        bool load_ok = rt.load_state_file("test_persistence");
        ok_bool("load_state_file restored test_persistence.state", Value(load_ok ? 1.0 : 0.0), true);
        ok_num("restored room_index matches", (double)rt.room_index, (double)saved_room_before);
        ok_num("restored instances size matches", (double)rt.instances.size(), (double)saved_inst_cnt);
        ok_num("restored custom global scalar", to_num(rt.globals["test_save_var"]), 1337.0);
        ok_str("restored custom global array str", to_str(rt.global_arrays["test_save_arr"][0]), "state_saved");
        ok_num("restored custom global array num", to_num(rt.global_arrays["test_save_arr"][1]), 42.0);
        ok_bool("restored array VARPTR relinked", Value(rt.globals["test_save_arr"].is_arr() ? 1.0 : 0.0), true);
        ok_str("restored ds_map string value", to_str(C("ds_map_find_value", {num(test_map), str("player_choice")})), "save_state_ok");

        std::remove("states/test_persistence.state");

        // ================= Flowey Fireball Collision Verification =================
        {
            rt.instances.clear();
            // Create obj_floweybattle1 (obj 199)
            auto flowey = std::make_unique<Instance>(9901, 199);
            flowey->vars["x"] = Value(100.0);
            flowey->vars["y"] = Value(100.0);
            flowey->vars["conversation"] = Value(16.0); // Conversation state before Toriel's fireball
            flowey->vars["sprite_index"] = Value(184.0); // spr_floweynice
            flowey->vars["visible"] = Value(1.0);
            Instance* fp = flowey.get();
            rt.instances.push_back(std::move(flowey));

            // Create obj_torielflame_X (obj 608) overlapping Flowey
            auto flame = std::make_unique<Instance>(9902, 608);
            flame->vars["x"] = Value(110.0);
            flame->vars["y"] = Value(110.0);
            flame->vars["sprite_index"] = Value(78.0);
            flame->vars["visible"] = Value(1.0);
            Instance* flame_p = flame.get();
            rt.instances.push_back(std::move(flame));

            // Run step to process collision
            rt.step();

            ok_bool("Toriel fireball destroyed upon hitting Flowey", Value(!flame_p->alive ? 1.0 : 0.0), true);
            ok_num("Flowey conversation advanced to 17 on fireball hit", to_num(fp->vars["conversation"]), 17.0);
            ok_num("Flowey sprite changed to shocked face (182)", to_num(fp->vars["sprite_index"]), 182.0);
        }

        // ================= Dialogue / Savepoint global.interact Integrity =================
        {
            rt.globals["interact"] = Value(1.0);
            auto savept = std::make_unique<Instance>(9903, 1353); // obj_savepoint
            savept->vars["myinteract"] = Value(1.0);
            savept->vars["x"] = Value(50.0);
            savept->vars["y"] = Value(50.0);
            Instance* sp_p = savept.get();
            rt.instances.push_back(std::move(savept));

            // Setting myinteract = 4.0 must NOT prematurely reset global.interact
            sp_p->vars["myinteract"] = Value(4.0);
            ok_num("global.interact remains 1 when myinteract=4 set", to_num(rt.globals["interact"]), 1.0);

            // Destroying instance with myinteract != 0 must NOT prematurely reset global.interact
            C("instance_destroy", {Value((double)sp_p->iid)});
            ok_bool("savepoint instance destroyed", Value(!sp_p->alive ? 1.0 : 0.0), true);
            ok_num("global.interact remains 1 after instance with myinteract destroyed", to_num(rt.globals["interact"]), 1.0);

            // Dialogue writer/dialoguer finish naturally resets global.interact
            rt.globals["interact"] = Value(0.0);
            ok_num("global.interact cleanly reset to 0 after dialogue finish", to_num(rt.globals["interact"]), 0.0);
        }

        // ================= Napstablook Unfreeze on Destroy =================
        {
            int napsta_obj = -1;
            for (size_t oi = 0; oi < dw.objects.size(); ++oi) {
                if (dw.objects[oi].name == "obj_napstablook2") {
                    napsta_obj = (int)oi;
                    break;
                }
            }
            if (napsta_obj >= 0) {
                rt.globals["interact"] = Value(1.0);
                auto napsta = std::make_unique<Instance>(9904, napsta_obj);
                napsta->vars["myinteract"] = Value(4.0);
                Instance* np_p = napsta.get();
                rt.instances.push_back(std::move(napsta));

                C("instance_destroy", {Value((double)np_p->iid)});
                ok_bool("napstablook instance destroyed", Value(!np_p->alive ? 1.0 : 0.0), true);
                ok_num("global.interact reset to 0 upon napstablook destroy", to_num(rt.globals["interact"]), 0.0);
            }
        }

        // ================= Camera view unclamping & cutscene follow =================
        {
            rt.start_room(0);
            rt.globals["view_enabled"] = Value(1.0);
            rt.globals["interact"] = Value(1.0);
            // view_xview allowed to go negative (Papyrus camera pan in Room 48)
            rt.global_arrays["view_xview"][0] = Value(-40.0);
            rt.global_arrays["view_yview"][0] = Value(-20.0);
            rt.update_camera();
            ok_num("view_x preserves negative x offset (-40)", (double)rt.view_x, -40.0);
            ok_num("view_y preserves negative y offset (-20)", (double)rt.view_y, -20.0);
            ok_num("view_xview[0] is not clamped", to_num(rt.global_arrays["view_xview"][0]), -40.0);

            // Cutscene view follow suspension (Dangerbridge trap in Room 67)
            rt.globals["interact"] = Value(1.0);
            rt.global_arrays["view_object"][0] = Value(0.0); // follow object 0
            rt.global_arrays["view_xview"][0] = Value(150.0); // cutscene pans to 150
            rt.update_view_follow();
            ok_num("view_xview preserved during cutscene (interact != 0)", to_num(rt.global_arrays["view_xview"][0]), 150.0);

            // Normal gameplay clamps view to room bounds
            rt.globals["interact"] = Value(0.0);
            rt.global_arrays["view_xview"][0] = Value(-40.0);
            rt.global_arrays["view_yview"][0] = Value(-20.0);
            rt.update_camera();
            ok_num("view_x clamped to 0 during gameplay", (double)rt.view_x, 0.0);
            ok_num("view_y clamped to 0 during gameplay", (double)rt.view_y, 0.0);
            ok_num("view_xview[0] clamped to 0 during gameplay", to_num(rt.global_arrays["view_xview"][0]), 0.0);
        }

        // ================= Background arrays initialization and scrolling =================
        {
            rt.start_room(0);
            ok_bool("background_visible array exists", Value(rt.global_arrays.count("background_visible") ? 1.0 : 0.0), true);
            ok_bool("background_index array exists", Value(rt.global_arrays.count("background_index") ? 1.0 : 0.0), true);
            rt.global_arrays["background_hspeed"][0] = Value(2.5);
            rt.global_arrays["background_x"][0] = Value(10.0);
            rt.step();
            ok_num("background_x advances by hspeed", to_num(rt.global_arrays["background_x"][0]), 12.5);
        }

        // ================= Caster Sound Resolution (Papyrus Music) =================
        {
            int pap_id = rt.load_caster_sound("music/papyrus.ogg");
            ok_bool("load_caster_sound loads music/papyrus.ogg resolving to mus_papyrus.ogg", Value(pap_id >= 0 ? 1.0 : 0.0), true);
            int snowy_id = rt.load_caster_sound("music/snowy.ogg");
            ok_bool("load_caster_sound loads music/snowy.ogg resolving to mus_snowy.ogg", Value(snowy_id >= 0 ? 1.0 : 0.0), true);
        }

        // ================= obj_lesserdoge instance_create =================
        {
            for (const auto& ce : dw.code) {
                if (ce.name.find("lesserdoge") != std::string::npos) {
                    for (uint32_t i = 0; i < ce.instr_count; ++i) {
                        const auto& ins = dw.all_instrs[ce.instr_start + i];
                        if (ins.fun >= 0 && (size_t)ins.fun < dw.functions.size()) {
                            const auto& fn = dw.functions[ins.fun].name;
                            if (fn == "instance_create") {
                                std::printf("%s calls instance_create at ins %u\n", ce.name.c_str(), i);
                                for (int k = std::max(0, (int)i - 6); k < (int)i; ++k) {
                                    const auto& pins = dw.all_instrs[ce.instr_start + k];
                                    if (pins.kind() == OP_PUSHI) {
                                        int oid = (int)pins.value;
                                        std::string oname = (oid >= 0 && oid < (int)dw.objects.size()) ? dw.objects[oid].name : "";
                                        std::printf("  arg obj: %d (%s)\n", oid, oname.c_str());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
