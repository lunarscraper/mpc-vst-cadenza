/* Offline x86 test of cadenza_vst.cpp (built -DNO_ALSA by test.sh): instances, audio path of both
 * variants, played notes == the engine's render (count, pitch and velocity per lane), no hanging notes,
 * swing, FORM, lane on/off, channel, octave, variation, Generate/Variate, Goa phrygian, pad chords,
 * CC74 out, MIDI CC control (async regeneration), tempo follow, chunk and preset round trips.
 * Usage: host_test <plugin.so> */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

#include "cadenza_engine.h"

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

static VstTimeInfo ti;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) { return op == 7 ? (intptr_t)&ti : 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static int find(AEffect *e, const char *name, int nth = 0) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name) && nth-- == 0) return i; }
    return -1;
}
static std::string disp(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 7, i, 0, b, 0); return b; }
static void set_int(AEffect *e, int i, int v, int lo, int hi) { e->setParameter(e, i, (float)(v - lo) / (hi - lo)); }
static void set_opt(AEffect *e, int i, int o, int n) { e->setParameter(e, i, (float)o / (n - 1)); }

struct Ev { long block; int st, n, v; };
static const double TEMPO_DEFAULT = 128;
/* blocks of 128 frames at <tempo> BPM / 44.1 kHz from ppq 0; collects the MIDI trace with block numbers */
static std::vector<Ev> play(AEffect *e, long blocks, double tempo = TEMPO_DEFAULT, bool stop_at_end = true) {
    float l[128], r[128]; float *o[2] = {l, r};
    float il[128], ir[128]; float *iv[2] = {il, ir};
    for (int i = 0; i < 128; i++) { il[i] = 0.25f; ir[i] = -0.5f; }
    float **in = e->numInputs ? iv : nullptr;
    std::vector<Ev> out;
    fflush(stdout);
    char *buf = nullptr; size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    FILE *old = stdout; stdout = mem;
    std::vector<size_t> marks;
    ti.flags = (1 << 1) | (1 << 9) | (1 << 10); ti.tempo = tempo; ti.ppqPos = 0; ti.sampleRate = 44100;
    std::thread audio([&] {   /* like a host: process runs on its own audio thread */
        for (long b = 0; b < blocks; b++) {
            e->processReplacing(e, in, o, 128);
            ti.ppqPos += 128 * (tempo / 60.0) / 44100.0;
            fflush(mem); marks.push_back(len);
        }
        if (stop_at_end) { ti.flags = (1 << 9) | (1 << 10); e->processReplacing(e, in, o, 128); fflush(mem); marks.push_back(len); }
    });
    audio.join();
    stdout = old; fclose(mem);
    size_t pos = 0;
    for (size_t b = 0; b < marks.size(); b++) {
        while (pos < marks[b]) {
            unsigned st, n, v;
            if (std::sscanf(buf + pos, "MIDI %x %x %x", &st, &n, &v) == 3) out.push_back({(long)b, (int)st, (int)n, (int)v});
            const char *nl = (const char *)std::memchr(buf + pos, '\n', len - pos);
            pos = nl ? (size_t)(nl - buf) + 1 : len;
        }
    }
    free(buf);
    return out;
}
/* blocks covering exactly <beats> quarter notes at <tempo> (the last pulse stays below beats*96) */
static long blocks_for(double beats, double tempo) {
    double per = 128 * (tempo / 60.0) / 44100.0;
    return (long)std::floor((beats - 1e-6) / per);
}
static void idle(AEffect *a, int n = 1) {   /* stopped blocks: CCs, swaps, display */
    float l[128], r[128], il[128] = {0}, ir[128] = {0}; float *o[2] = {l, r}, *iv[2] = {il, ir};
    ti.flags = (1 << 9) | (1 << 10); ti.tempo = TEMPO_DEFAULT;
    std::thread audio([&] { for (int k = 0; k < n; k++) a->processReplacing(a, a->numInputs ? iv : nullptr, o, 128); });
    audio.join();
}
static void cc(AEffect *a, int ch, int num, int val) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m;
    m.midiData[0] = (unsigned char)(0xB0 | (ch - 1)); m.midiData[1] = (unsigned char)num; m.midiData[2] = (unsigned char)val;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    a->dispatcher(a, 25, 0, 0, &ev, 0);
    idle(a);
    usleep(80000);   /* the worker thread regenerates */
    idle(a);
}
static int hanging(const std::vector<Ev> &ev) {
    std::map<int, int> h;
    for (auto &x : ev) { int k = (x.st & 15) * 128 + x.n; if ((x.st & 0xF0) == 0x90 && x.v) h[k]++; else if ((x.st & 0xF0) == 0x80) h[k] = 0; }
    int n = 0; for (auto &kv : h) n += kv.second > 0; return n;
}
static std::vector<Ev> ons(const std::vector<Ev> &ev, int ch) {
    std::vector<Ev> r;
    for (auto &x : ev) if ((x.st & 0xF0) == 0x90 && x.v && (x.st & 15) == ch - 1) r.push_back(x);
    return r;
}

int main(int argc, char **argv) {
    setenv("CADENZA_TRACE", "1", 1);
    if (!std::getenv("CADENZA_BANK")) { unlink("/tmp/cadenza_test_bank.txt"); setenv("CADENZA_BANK", "/tmp/cadenza_test_bank.txt", 1); }
    void *h = dlopen(argc > 1 ? argv[1] : "./build/x86/cadenza-x86.so", RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\nFAILED\n", dlerror()); return 1; }
    auto entry = (AEffect *(*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    AEffect *a = entry(master), *b = entry(master);
    CHECK(a && b && a->magic == 0x56737450, "instances");
    std::printf("params: %d, inputs: %d, category: %d\n", a->numParams, a->numInputs, (int)a->dispatcher(a, 35, 0, 0, nullptr, 0));
    {   /* audio path */
        float il[128], ir[128], ol[128], orr[128];
        for (int i = 0; i < 128; i++) { il[i] = i / 128.0f; ir[i] = -i / 256.0f; ol[i] = orr[i] = 9.0f; }
        float *iv[2] = {il, ir}, *ov[2] = {ol, orr};
        ti.flags = 0;
        std::thread([&] { a->processReplacing(a, a->numInputs ? iv : nullptr, ov, 128); }).join();
        if (a->numInputs) {
            CHECK(a->numInputs == 2 && !(a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 1, "effect: category Effect");
            CHECK(!std::memcmp(il, ol, sizeof il) && !std::memcmp(ir, orr, sizeof ir), "effect passes audio through");
            float keep[128]; std::memcpy(keep, il, sizeof il);
            std::thread([&] { a->processReplacing(a, iv, iv, 128); }).join();
            CHECK(!std::memcmp(keep, il, sizeof il), "effect in-place pass-through");
        } else {
            CHECK((a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 2, "instrument: synth flag");
            CHECK(ol[5] == 0.0f && orr[5] == 0.0f, "instrument writes silence");
        }
    }
    int key = find(a, "Key"), scale = find(a, "Scale"), style = find(a, "Style"), seed = find(a, "Seed"),
        prog = find(a, "Progression"), gen = find(a, "Generate"), var = find(a, "Variate"), form = find(a, "Form"),
        feel = find(a, "Bass Feel"), tempo = find(a, "Tempo Ref"), swing = find(a, "Swing"), gate = find(a, "Gate"),
        pad = find(a, "Pad"), ccout = find(a, "CC Out"), preset = find(a, "Preset"), load = find(a, "Load"),
        save = find(a, "Save"), cch = find(a, "Control Ch"), status = find(a, "Status"), chords = find(a, "Chords");
    CHECK(key >= 0 && scale >= 0 && style >= 0 && seed >= 0 && prog >= 0 && gen >= 0 && var >= 0 && form >= 0 && feel >= 0 &&
          tempo >= 0 && swing >= 0 && gate >= 0 && pad >= 0 && ccout >= 0 && preset >= 0 && load >= 0 && save >= 0 &&
          cch >= 0 && status >= 0 && chords >= 0, "global params");
    int on[7], ch[7], oct[7], lvar[7], dice[7], pat[7];
    for (int l = 0; l < 7; l++) {
        on[l] = find(a, "On", l); ch[l] = find(a, "Channel", l); oct[l] = find(a, "Octave", l);
        lvar[l] = find(a, "Variation", l); dice[l] = find(a, "Dice", l); pat[l] = find(a, "Steps", l);
        CHECK(on[l] >= 0 && ch[l] >= 0 && oct[l] >= 0 && lvar[l] >= 0 && dice[l] >= 0 && pat[l] >= 0, "lane %d params", l + 1);
    }
    std::printf("defaults: %s %s %s seed %s | %s | %s | tempo %s\n", disp(a, key).c_str(), disp(a, scale).c_str(),
                disp(a, style).c_str(), disp(a, seed).c_str(), disp(a, chords).c_str(), disp(a, status).c_str(), disp(a, tempo).c_str());
    CHECK(disp(a, key) == "A" && disp(a, scale) == "MINOR" && disp(a, style) == "GROOVE" && disp(a, seed) == "1", "defaults");
    CHECK(disp(a, lvar[0]) == "ORIG" && disp(a, oct[0]) == "0" && disp(a, ch[6]) == "7", "lane defaults");

    /* reference: the engine itself with the plugin's defaults (A minor, seed "1", groove, melodic, 128 BPM) */
    cz::Ctx ref;
    ref.key = cz::make_key(9, 0); ref.bpm = 128; ref.seed = "1"; ref.family = "groove"; ref.offline_style = "melodic";
    ref.prog = cz::select_progression(ref.key.root, ref.key.scale, ref.seed);
    std::string want_chords;
    for (int i = 0; i < 4; i++) want_chords += (i ? " " : "") + ref.prog.c[i].root + cz::quality_suffix(ref.prog.c[i].quality);
    CHECK(disp(a, chords) == want_chords, "chords %s == engine %s", disp(a, chords).c_str(), want_chords.c_str());

    /* played notes == rendered notes, lane by lane (PAD = SINGLE: one note per hit like Cadenza) */
    set_opt(a, pad, 0, 2);
    long B16 = blocks_for(64, 128);
    {
        std::vector<Ev> ev = play(a, B16);
        int total = 0;
        for (int l = 0; l < 7; l++) {
            cz::Render r = cz::render_stem(l, ref);
            std::vector<Ev> o = ons(ev, l + 1);
            bool same = o.size() == r.notes.size();
            for (size_t k = 0; same && k < o.size(); k++) same = o[k].n == r.notes[k].note && o[k].v == r.notes[k].vel;
            CHECK(same, "lane %d: played %zu notes, engine %zu (first %d/%d vs %d/%d)", l + 1, o.size(), r.notes.size(),
                  o.empty() ? -1 : o[0].n, o.empty() ? -1 : o[0].v, r.notes.empty() ? -1 : r.notes[0].note, r.notes.empty() ? -1 : r.notes[0].vel);
            if (!same && std::getenv("CADENZA_DEBUG")) { for (auto &x : o) std::printf("%ld:%d/%d ", x.block, x.n, x.v); std::printf("\n"); }
            total += (int)o.size();
        }
        std::printf("16 bars: %zu MIDI events, %d note-ons, hanging after stop %d\n", ev.size(), total, hanging(ev));
        CHECK(total > 300, "a full arrangement plays: %d", total);
        CHECK(hanging(ev) == 0, "no hanging notes after stop");
        CHECK(disp(a, status).find("stopped") == 0, "status after stop: %s", disp(a, status).c_str());
        std::printf("lane 1 bar 1: %s | lane 4: %s\n", disp(a, pat[0]).c_str(), disp(a, pat[3]).c_str());
        CHECK(disp(a, pat[0]).size() >= 20, "steps text: %s", disp(a, pat[0]).c_str());
    }
    /* a second 16-bar loop plays the same again (position = pulse index modulo the loop) */
    {
        std::vector<Ev> ev = play(a, 2 * B16);
        CHECK(ons(ev, 4).size() == 2 * cz::render_stem(3, ref).notes.size(), "loop repeats: %zu arp notes", ons(ev, 4).size());
        CHECK(hanging(ev) == 0, "no hanging notes over the loop seam");
    }
    /* FORM = PEAK: the third pass looped every 4 bars */
    {
        set_opt(a, form, 3, 5);
        std::vector<Ev> ev = play(a, blocks_for(16, 128));
        cz::Render r = cz::render_stem(4, ref);
        size_t want = 0;
        for (auto &n : r.notes) want += n.on >= 2 * 7680 && n.on < 3 * 7680;
        CHECK(ons(ev, 5).size() == want, "PEAK: melody %zu notes, pass 3 has %zu", ons(ev, 5).size(), want);
        CHECK(disp(a, status).find("PEAK") != std::string::npos, "status shows PEAK: %s", disp(a, status).c_str());
        set_opt(a, form, 0, 5);
    }
    /* lane off / channel / octave */
    {
        a->setParameter(a, on[1], 0.0f);
        set_int(a, ch[0], 9, 1, 16);
        set_int(a, oct[3], 1, -2, 2);
        std::vector<Ev> ev = play(a, B16);
        CHECK(ons(ev, 2).empty(), "lane 2 off: %zu notes", ons(ev, 2).size());
        CHECK(ons(ev, 1).empty() && ons(ev, 9).size() == cz::render_stem(0, ref).notes.size(), "lane 1 on channel 9: %zu", ons(ev, 9).size());
        std::vector<Ev> arp = ons(ev, 4);
        cz::Render r = cz::render_stem(3, ref);
        CHECK(!arp.empty() && arp[0].n == r.notes[0].note + 12, "octave +1: %d vs %d", arp.empty() ? -1 : arp[0].n, r.notes[0].note);
        CHECK(hanging(ev) == 0, "no hanging notes");
        a->setParameter(a, on[1], 1.0f);
        set_int(a, ch[0], 1, 1, 16);
        set_int(a, oct[3], 0, -2, 2);
    }
    /* swing 75 %: odd 16ths come 12 pulses (half a 16th) later; arp hits every 16th */
    {
        std::vector<Ev> s0 = ons(play(a, 200), 4);
        set_int(a, swing, 75, 50, 75);
        std::vector<Ev> s1 = ons(play(a, 200), 4);
        set_int(a, swing, 50, 50, 75);
        CHECK(s0.size() >= 3 && s1.size() >= 3, "arp plays");
        if (s0.size() >= 3 && s1.size() >= 3) {
            /* a 16th at 128 BPM = 5168 frames = 40.4 blocks */
            std::printf("arp straight %ld %ld %ld | swung %ld %ld %ld\n", s0[0].block, s0[1].block, s0[2].block, s1[0].block, s1[1].block, s1[2].block);
            CHECK(s1[0].block == s0[0].block && std::labs(s1[2].block - s0[2].block) <= 1, "even steps stay on the grid");
            CHECK(s1[1].block - s0[1].block >= 18 && s1[1].block - s0[1].block <= 22, "odd step delayed ~20 blocks: %ld", s1[1].block - s0[1].block);
        }
    }
    /* variation: only that lane changes; 0 brings Cadenza's pattern back */
    {
        std::string p[7];
        for (int l = 0; l < 7; l++) p[l] = disp(a, pat[l]);
        std::string melody0;
        for (auto &x : ons(play(a, B16), 5)) melody0 += std::to_string(x.n) + ",";
        set_int(a, lvar[4], 7, 0, 99);
        std::string melody7;
        for (auto &x : ons(play(a, B16), 5)) melody7 += std::to_string(x.n) + ",";
        CHECK(melody7 != melody0, "variation 7 changes the melody");
        CHECK(disp(a, pat[0]) == p[0] && disp(a, pat[3]) == p[3] && disp(a, chords) == want_chords, "other lanes and chords unchanged");
        set_int(a, lvar[4], 0, 0, 99);
        std::string back;
        for (auto &x : ons(play(a, B16), 5)) back += std::to_string(x.n) + ",";
        CHECK(back == melody0, "variation 0 restores Cadenza's melody");
        a->setParameter(a, dice[2], 1.0f);
        CHECK(disp(a, lvar[2]) != "ORIG", "dice sets a variation: %s", disp(a, lvar[2]).c_str());
        a->setParameter(a, var, 1.0f);
        int nonzero = 0;
        for (int l = 0; l < 7; l++) nonzero += disp(a, lvar[l]) != "ORIG";
        CHECK(nonzero == 7, "variate gives every lane a variation (%d)", nonzero);
        CHECK(disp(a, chords) == want_chords, "variate keeps the harmony");
        a->setParameter(a, gen, 1.0f);
        nonzero = 0;
        for (int l = 0; l < 7; l++) nonzero += disp(a, lvar[l]) != "ORIG";
        CHECK(nonzero == 0, "generate resets the variations");
        std::printf("generate -> seed %s: %s\n", disp(a, seed).c_str(), disp(a, chords).c_str());
        set_int(a, seed, 1, 0, 9999);
        CHECK(disp(a, chords) == want_chords, "seed 1 again -> same chords");
    }
    /* Goa phrygian in E: I-bII family, every chord root in E F G# A B C D */
    {
        set_opt(a, key, 4, 12);
        set_opt(a, scale, 7, 8);
        std::string c = disp(a, chords);
        std::printf("E GOA PHRYG: %s\n", c.c_str());
        bool roots_ok = disp(a, scale) == "GOA PHRYG";
        for (size_t p0 = 0; p0 < c.size();) {   /* every chord root is a note of E phrygian dominant */
            size_t sp = c.find(' ', p0);
            std::string tok = c.substr(p0, sp == std::string::npos ? std::string::npos : sp - p0);
            std::string root = tok.size() > 1 && tok[1] == '#' ? tok.substr(0, 2) : tok.substr(0, 1);
            roots_ok = roots_ok && (root == "E" || root == "F" || root == "G#" || root == "A" || root == "B" || root == "C" || root == "D");
            if (sp == std::string::npos) break;
            p0 = sp + 1;
        }
        CHECK(roots_ok && c.find("E") != std::string::npos, "goa chords: %s", c.c_str());
        std::vector<Ev> ev = play(a, B16, 145);
        std::map<int, int> pcs;
        for (auto &x : ev) if ((x.st & 0xF0) == 0x90 && x.v) pcs[x.n % 12]++;
        static const int GOA_E[7] = {4, 5, 8, 9, 11, 0, 2};
        int inside = 0, all = 0;
        for (auto &kv : pcs) { bool f = false; for (int g : GOA_E) f = f || kv.first == g; all += kv.second; if (f) inside += kv.second; }
        std::printf("goa: %d of %d notes in E phrygian dominant (the rest: the rolling bass's chromatic approach)\n", inside, all);
        CHECK(pcs[8] > 0, "the major third G# is used");
        CHECK(all > 0 && inside * 100 >= all * 95, "notes stay in E phrygian dominant: %d of %d", inside, all);
        CHECK(hanging(ev) == 0, "no hanging notes");
        idle(a, 2); usleep(100000); idle(a, 2);
        CHECK(disp(a, status).find("145 BPM") != std::string::npos, "tempo follows the host (HOST): %s", disp(a, status).c_str());
        set_int(a, tempo, 128, 99, 160);
        CHECK(disp(a, tempo) == "128 BPM" && disp(a, status).find("128 BPM") != std::string::npos, "fixed tempo ref: %s", disp(a, status).c_str());
        set_int(a, tempo, 99, 99, 160);
        set_opt(a, key, 9, 12);
        set_opt(a, scale, 0, 8);
    }
    /* PAD = CHORD: three voices per hit (groove voicing root3 fifth4 third5) */
    {
        set_opt(a, pad, 1, 2);
        set_int(a, tempo, 128, 99, 160);
        std::vector<Ev> pd = ons(play(a, blocks_for(4, 128)), 6);
        int first_block = pd.empty() ? -1 : (int)pd[0].block, n0 = 0;
        for (auto &x : pd) n0 += x.block == first_block;
        CHECK(n0 == 3, "pad chord: %d voices on the first hit", n0);
        set_opt(a, pad, 0, 2);
    }
    /* CC OUT = SWEEP: CC74 on the bass lane's channel */
    {
        std::vector<Ev> ev = play(a, blocks_for(16, 128));
        int c74 = 0;
        for (auto &x : ev) c74 += x.st == 0xB0 && x.n == 74;
        CHECK(c74 == 0, "no CCs by default (%d)", c74);
        set_opt(a, ccout, 1, 4);
        ev = play(a, blocks_for(16, 128));
        c74 = 0;
        for (auto &x : ev) c74 += x.st == 0xB0 && x.n == 74;
        CHECK(c74 >= 16, "sweep sends CC74 on channel 1: %d", c74);
        set_opt(a, ccout, 0, 4);
    }
    /* MIDI CC on Control Ch 16 (default): CC 100 key, CC 23 lane 2 octave, CC 101 scale; regenerated by the worker */
    {
        CHECK(disp(a, cch) == "16", "control channel default 16: %s", disp(a, cch).c_str());
        std::string c0 = disp(a, chords);
        cc(a, 16, 100, 0);
        CHECK(disp(a, key) == "C" && disp(a, chords) != c0 && disp(a, chords).rfind("C", 0) == 0, "CC 100 -> key C: %s", disp(a, chords).c_str());
        cc(a, 1, 100, 127);
        CHECK(disp(a, key) == "C", "CC on another channel ignored");
        cc(a, 16, 22, 127);
        CHECK(disp(a, oct[1]) == "+2", "CC 22 -> lane 2 octave %s", disp(a, oct[1]).c_str());
        cc(a, 16, 22, 64);
        cc(a, 16, 100, 106);   /* 106/127 * 11 = 9.2 -> A */
        CHECK(disp(a, key) == "A" && disp(a, chords) == want_chords, "back to A: %s / %s", disp(a, key).c_str(), disp(a, chords).c_str());
        a->setParameter(a, cch, 0.0f);
        cc(a, 16, 100, 0);
        CHECK(disp(a, key) == "A", "Control Ch OFF ignores CCs");
        a->setParameter(a, cch, 1.0f);
    }
    /* chunk round trip: same parameters, same pattern; into another instance too */
    {
        set_int(a, lvar[3], 42, 0, 99);
        set_opt(a, style, 1, 3);
        std::string pats[7];
        for (int l = 0; l < 7; l++) pats[l] = disp(a, pat[l]);
        std::string c0 = disp(a, chords);
        void *ck = nullptr;
        intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
        std::string chunk((char *)ck, n);
        CHECK(n > 100 && chunk.find("l4_var=42;") != std::string::npos && chunk.find("bpm=128;") != std::string::npos, "chunk (%ld bytes): %s", (long)n, chunk.c_str());
        a->setParameter(a, gen, 1.0f);
        set_opt(a, scale, 3, 8);
        a->dispatcher(a, 24, 0, n, (void *)chunk.data(), 0);
        bool same = disp(a, chords) == c0;
        for (int l = 0; l < 7; l++) same = same && disp(a, pat[l]) == pats[l];
        CHECK(same && disp(a, style) == "ROLLING", "chunk restores the pattern");
        b->dispatcher(b, 24, 0, n, (void *)chunk.data(), 0);
        CHECK(disp(b, chords) == c0 && disp(b, pat[3]) == pats[3] && disp(b, pat[1]) == pats[1], "chunk into another instance");
    }
    /* presets: save slot 3, change, load slot 3 */
    {
        set_int(a, preset, 3, 1, 32);
        std::string c0 = disp(a, chords), p4 = disp(a, pat[3]);
        a->setParameter(a, save, 1.0f);
        a->setParameter(a, gen, 1.0f);
        set_opt(a, key, 2, 12);
        a->setParameter(a, load, 1.0f);
        CHECK(disp(a, chords) == c0 && disp(a, pat[3]) == p4, "preset 3 restores: %s", disp(a, chords).c_str());
        CHECK(disp(a, preset) == "3", "preset number kept");
        usleep(300000);   /* the bank file is written off the audio thread */
        const char *bp = std::getenv("CADENZA_BANK");
        FILE *f = bp ? std::fopen(bp, "r") : nullptr;
        char line[64] = {0};
        CHECK(f && std::fgets(line, sizeof line, f) && !std::strncmp(line, "3:v=1;", 6), "bank file has slot 3: %s", line);
        if (f) std::fclose(f);
    }
    /* every key x scale x style generates and plays without hanging notes */
    {
        int bad = 0;
        for (int k = 0; k < 12; k += 5)
            for (int s = 0; s < 8; s++)
                for (int st = 0; st < 3; st++) {
                    set_opt(a, key, k, 12); set_opt(a, scale, s, 8); set_opt(a, style, st, 3);
                    std::vector<Ev> ev = play(a, blocks_for(16, 128));
                    int n = 0; for (auto &x : ev) n += (x.st & 0xF0) == 0x90 && x.v;
                    if (n < 20 || hanging(ev)) { bad++; std::printf("  %s %s %s: %d notes, %d hanging\n", disp(a, key).c_str(), disp(a, scale).c_str(), disp(a, style).c_str(), n, hanging(ev)); }
                }
        CHECK(bad == 0, "%d of 72 key/scale/style combinations failed", bad);
    }
    a->dispatcher(a, 1, 0, 0, nullptr, 0);
    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
