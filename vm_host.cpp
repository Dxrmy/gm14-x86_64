// Host test for the C++ VM: boot the game, move the player, dump a frame.
#include "vm.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s data.win [room] [frames]\n", argv[0]); return 1; }
    try {
        gm14::DataWin dw(argv[1]);
        gm14::Runtime rt(dw);

        int cur_frame = 0;
        int logged = 0;
        auto orig = rt.builtins["draw_text_transformed"];
        rt.builtins["draw_text_transformed"] = [&](gm14::Runtime& r, std::vector<gm14::Value>& a) {
            if (cur_frame == 150 && logged < 12) {
                ++logged;
                std::printf("DTT x=%.1f y=%.1f text='%s' font=%d args=%zu", gm14::to_num(a[0]), gm14::to_num(a[1]),
                            gm14::to_str(a[2]).c_str(), r.draw_font, a.size());
                for (size_t i = 3; i < a.size(); ++i) std::printf(" a%zu=%.2f", i, gm14::to_num(a[i]));
                std::printf("\n");
            }
            return orig(r, a);
        };
        rt.start_room(0);
        rt.change_room(1); // room_introstory

        for (int f = 0; f < 151; ++f) {
            cur_frame = f;
            rt.step();
            auto img = rt.draw();
            if (f == 150) {
                stbi_write_png("test_intro_150.png", img.w, img.h, 4, img.rgba.data(), img.w * 4);
                for (auto& inst : rt.instances) {
                    if (inst->alive && dw.objects[inst->obj].name == "OBJ_WRITER") {
                        auto g = [&](const char* k){ return inst->vars.count(k) ? gm14::to_num(inst->vars[k]) : -999.0; };
                        std::printf("WRITER stringpos=%.0f halt=%.0f alarm0=%d writingx=%.0f writingy=%.0f myfont=%.0f\n",
                                    g("stringpos"), g("halt"), inst->alarms[0], g("writingx"), g("writingy"), g("myfont"));
                        auto os = inst->vars.find("originalstring");
                        if (os != inst->vars.end()) std::printf("originalstring='%s'\n", gm14::to_str(os->second).substr(0, 60).c_str());
                    }
                }
            }
        }
        return 0;
        for (int f = 0; f < 3; ++f) rt.step();
        rt.change_room(1);
        for (int f = 0; f < 40; ++f) {
            rt.step();
            if (f == 6) {
                auto it = rt.global_arrays.find("msg");
                std::printf("global msg arr: %s size=%zu\n", it == rt.global_arrays.end() ? "MISSING" : "found", it == rt.global_arrays.end() ? 0 : it->second.size());
                if (it != rt.global_arrays.end()) for (size_t i = 0; i < it->second.size() && i < 4; ++i) {
                    auto& v = it->second[i];
                    std::printf("  msg[%zu] = [%s] '%s'\n", i, v.is_str() ? "str" : "num", v.is_str() ? v.str.substr(0, 80).c_str() : std::to_string(v.num).c_str());
                }
                for (auto& inst : rt.instances) {
                    if (inst->alive && dw.objects[inst->obj].name == "OBJ_WRITER") {
                        auto ms = inst->arrays.find("mystring");
                        std::printf("writer mystring: %s\n", ms == inst->arrays.end() ? "MISSING" : "found");
                        if (ms != inst->arrays.end() && !ms->second.empty()) {
                            auto& v = ms->second[0];
                            std::printf("  mystring[0] = [%s] '%s'\n", v.is_str() ? "str" : "num", v.is_str() ? v.str.substr(0, 80).c_str() : std::to_string(v.num).c_str());
                        }
                    }
                }
            }
            if (false) {
                for (auto& inst : rt.instances) {
                    if (inst->alive && dw.objects[inst->obj].name == "OBJ_WRITER") {
                        std::printf("--- f=%d alarms0=%d\n", f, inst->alarms[0]);
                        for (auto& kv : inst->vars) {
                            std::string s = kv.second.is_str() ? kv.second.str : std::to_string(kv.second.num);
                            if (s.size() > 60) s = s.substr(0, 60);
                            std::printf("  %s = %s\n", kv.first.c_str(), s.c_str());
                        }
                    }
                }
            }
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
    return 0;
}
