// Host test/driver: parse a data.win, print stats, render rooms to PNG.
#include "dw.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s data.win [room...]\n", argv[0]); return 1; }
    try {
        gm14::DataWin dw(argv[1]);
        std::printf("game: %s (%s) bytecode=%d window=%ux%u\n",
                    dw.game_name.c_str(), dw.display_name.c_str(),
                    dw.bytecode_version, dw.window_width, dw.window_height);
        std::printf("chunks=%zu strings=%zu code=%zu vars=%zu funcs=%zu\n",
                    dw.chunk_order.size(), dw.strings.size(), dw.code.size(),
                    dw.variables.size(), dw.functions.size());
        size_t ninstr = 0, nref_v = 0, nref_f = 0;
        for (auto& e : dw.code) {
            for (uint32_t k = 0; k < e.instr_count; ++k) {
                const gm14::Instruction& i = dw.all_instrs[e.instr_start + k];
                ninstr++;
                if (i.var >= 0) nref_v++;
                if (i.fun >= 0) nref_f++;
            }
        }
        std::printf("instructions=%zu resolved var refs=%zu fn refs=%zu\n", ninstr, nref_v, nref_f);
        std::printf("tpags=%zu sprites=%zu bgnds=%zu objects=%zu rooms=%zu\n",
                    dw.tpags.size(), dw.sprites.size(), dw.bgnds.size(),
                    dw.objects.size(), dw.rooms.size());

        if (argc == 2) {
            std::printf("room order (first 10):");
            for (int i = 0; i < 10 && i < (int)dw.room_order.size(); ++i)
                std::printf(" %u", dw.room_order[i]);
            std::printf("\n");
            return 0;
        }
        for (int a = 2; a < argc; ++a) {
            int ri = std::atoi(argv[a]);
            if (ri < 0 || ri >= (int)dw.rooms.size()) { std::fprintf(stderr, "bad room %d\n", ri); continue; }
            gm14::Image img = dw.render_room(ri);
            char fn[256];
            std::snprintf(fn, sizeof(fn), "out_room_%d_%s.png", ri, dw.rooms[ri].name.c_str());
            stbi_write_png(fn, img.w, img.h, 4, img.rgba.data(), img.w * 4);
            std::printf("wrote %s (%dx%d)\n", fn, img.w, img.h);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
