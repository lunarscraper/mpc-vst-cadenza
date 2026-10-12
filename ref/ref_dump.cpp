/* C++ side of the parity test: same output format as ref/go/cmd/dump (the Go original).
 * usage: ref_dump <root> <scale> <bpm> <seed> <family> [offlineStyle] */
#include <cstdio>
#include <cstdlib>
#include "../vst/cadenza_engine.h"

int main(int argc, char **argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: ref_dump root scale bpm seed family [style]\n"); return 2; }
    cz::Ctx c;
    std::string scale = argv[2];
    std::string mode = scale == "minor_natural" ? "minor" : scale == "phrygian_dominant" ? "goa" : scale;
    c.key = {argv[1], mode, scale};
    c.bpm = std::atof(argv[3]);
    c.seed = argv[4];
    c.family = argv[5];
    c.offline_style = argc > 6 ? argv[6] : "";
    c.prog = cz::select_progression(c.key.root, c.key.scale, c.seed);
    std::printf("PROG");
    for (int i = 0; i < 4; i++) std::printf(" %s%s", c.prog.c[i].root.c_str(), cz::quality_suffix(c.prog.c[i].quality).c_str());
    std::printf("\n");
    for (int i = 0; i < cz::NSTEMS; i++) {
        cz::Render r = cz::render_stem(i, c);
        std::printf("STEM %s %s\n", cz::STEM_TYPE[i], r.profile->name);
        for (auto &e : r.ev) {
            if (e.type == cz::EV_ON) std::printf("N %ld %d %d\n", e.tick, e.a, e.b);
            else if (e.type == cz::EV_OFF) std::printf("O %ld %d\n", e.tick, e.a);
            else std::printf("C %ld %d %d\n", e.tick, e.a, e.b);
        }
    }
    return 0;
}
