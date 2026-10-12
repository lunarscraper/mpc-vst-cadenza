/* =============================================================================
 * cadenza_vst.cpp - Cadenza: the offline engine of Cadenza (Andrea Cavallo, MIT) as a VST2 MIDI
 * generator for the MPC OS plugin host (MPC / Force). Hand-written VST2 shell after mpc-vst-drumgen
 * (itself after mpc-vst-euclidier / Euclidier FX).
 *
 * ENGINE (cadenza_engine.h, a 1:1 port, checked event by event against the Go original by
 * ref/parity.sh): key + scale + style family + seed -> one chord progression (4 chords, one per
 * 4 bars) and 7 stems on it, rendered over 4 passes of the 4-bar motif = 16 bars with Cadenza's
 * evolution (intro / build / peak / release), velocity grids, timing offsets, gates, CC74 sweep and
 * CC65 portamento:
 *   1 GROOVE bass   2 ROLLING bass (acid)   3 SUB bass   4 ARP   5 MELODY   6 PAD   7 LEAD stab
 * Every lane sends on its own MIDI channel (default 1..7), so one instance drives seven tracks.
 * Additions: scales Goa phrygian (phrygian dominant) and harmonic minor, per-lane VARIATION
 * (0 = Cadenza's own pattern, 1..99 = same harmony, new motif: the stem's seed + ".n"), FORM
 * (all 16 bars or one 4-bar part looped), PAD = full chord per hit, swing, gate scale, octave.
 *
 * Generation costs about a millisecond on the device, so it never runs on the audio thread:
 * setParameter (UI thread) builds synchronously outside the audio lock, CCs and tempo changes wake
 * a worker thread; the audio thread only swaps in the finished schedules.
 *
 * OUTPUT like DrumGen / Euclidier FX: MPC OS ignores VST MIDI out, so notes go to the plugin's own
 * ALSA seq port "<PLUG_NAME>" / "MIDI Out". Clock: host ppqPos at 96 pulses per quarter (PpqClock),
 * position = pulse index modulo the loop, so lanes sit on the MPC grid; 16-bar form starts at bar 1.
 *
 * TEMPO: Cadenza picks grooves and profiles by BPM. TEMPO REF = HOST uses the tempo of the project
 * (taken again when the transport starts; the generated pattern keeps the BPM it was made with,
 * stored in the project), or a fixed 100..160.
 *
 * CONTROL BY MIDI CC (Control In port, subscribed to every hardware input, + CCs sent to the track),
 * only on "Control Ch" (OFF, 1..16; default 16):
 *   CC 10*L + k, lane L = 1..7: k = 0 on, 1 channel, 2 octave, 3 variation, 4 dice (>= 64)
 *   CC 100 key, 101 scale, 102 style, 103 bass feel, 104 seed, 105 progression, 106 generate,
 *   107 variate, 108 form, 109 swing, 110 gate, 111 pad, 112 cc out, 113 tempo ref, 114 preset,
 *   115 load, 116 save
 *
 * TWO VARIANTS from this file (params.h decides, ../vst-fx/vst.json "effect": true):
 *   Cadenza     instrument (0 in / 2 out, silence) - uses one of the 8 plugin instrument slots
 *   Cadenza FX  insert effect (2 in / 2 out, audio untouched) - uses no instrument slot
 * Diagnostics: /tmp/cadenza_vst.log, one line every 5 s.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <dlfcn.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "cadenza_engine.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

#define BUILD_ID "cadenza-1.0.0"
#ifdef PLUG_EFFECT
#define PLUG_MODE "effect"
#else
#define PLUG_MODE "instrument"
#endif

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------------------
 * PpqClock (from drumgen_vst.cpp / euclidier_vst.cpp): pulse N IS ppq N/RES.
 * ------------------------------------------------------------------------- */
static const int RES = 96;                 /* pulses per quarter */
static const int PPS = RES / 4;            /* pulses per 16th step */
static const int PPB = RES * 4;            /* pulses per 4/4 bar */
static const int TPP = 480 / RES;          /* Cadenza ticks (480 per quarter) per pulse = 5 */
struct PpqClock {
    long long sent = 0;
    bool resync = true;
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;
};
static bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                            long long *from, long long *to, bool *jumped) {
    const long long TOL = RES / 2;
    double end = ppq + frames * (tempo / 60.0) / sr;
    long long first = (long long)std::ceil(ppq * RES - 1e-6);
    long long last = (long long)std::ceil(end * RES - 1e-6) - 1;
    *jumped = false;
    if (c->resync) { c->sent = first - 1; c->resync = false; }
    else if (first > c->sent + 1 + TOL || last < c->sent - TOL) { *jumped = true; c->jumps++; c->sent = first - 1; }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

/* ===========================================================================
 * Parameters: indices looked up by key once (the list order is params.json's)
 * ========================================================================= */
static const int NL = cz::NSTEMS;   /* 7 lanes */
static const char *const FAMILY[3] = {"groove", "rolling", "sub"};
static const char *const BASS_FEEL[4] = {"melodic", "hypnotic", "driving", "minimal"};
static const char *const PART_NAMES[4] = {"INTRO", "BUILD", "PEAK", "RELEASE"};
static const int TEMPO_HOST = 99;

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static int P_KEY, P_SCALE, P_STYLE, P_SEED, P_PROG, P_GEN, P_VAR, P_FORM, P_FEEL, P_TEMPO, P_SWING, P_GATE, P_PAD,
    P_CCOUT, P_PRESET, P_LOAD, P_SAVE, P_CCCH, P_STATUS, P_CHORDS;
static int P_ON[NL], P_CH[NL], P_OCT[NL], P_LVAR[NL], P_DICE[NL], P_PAT[NL];
static std::once_flag g_idx_once;
static void init_indices() {
    P_KEY = param_index("key"); P_SCALE = param_index("scale"); P_STYLE = param_index("style");
    P_SEED = param_index("seed"); P_PROG = param_index("prog"); P_GEN = param_index("generate");
    P_VAR = param_index("variate"); P_FORM = param_index("form"); P_FEEL = param_index("bass_feel");
    P_TEMPO = param_index("tempo_ref"); P_SWING = param_index("swing"); P_GATE = param_index("gate");
    P_PAD = param_index("pad_mode"); P_CCOUT = param_index("cc_out"); P_PRESET = param_index("preset");
    P_LOAD = param_index("preset_load"); P_SAVE = param_index("preset_save"); P_CCCH = param_index("cc_ch");
    P_STATUS = param_index("status"); P_CHORDS = param_index("chords");
    char k[24];
    for (int l = 0; l < NL; l++) {
#define IDX(arr, suffix) std::snprintf(k, sizeof k, "l%d_" suffix, l + 1); arr[l] = param_index(k);
        IDX(P_ON, "on") IDX(P_CH, "ch") IDX(P_OCT, "oct") IDX(P_LVAR, "var") IDX(P_DICE, "dice") IDX(P_PAT, "pat")
#undef IDX
    }
}
static bool is_readout(int i) {
    if (i == P_STATUS || i == P_CHORDS) return true;
    for (int l = 0; l < NL; l++) if (i == P_PAT[l]) return true;
    return false;
}
static bool is_value_param(int i) { return !PARAMS[i].momentary && !popup_is(i) && !is_readout(i); }
/* parameters that change what is generated (everything else applies at playback) */
static bool shapes_pattern(int i) {
    if (!is_value_param(i) || i == P_PRESET || i == P_CCCH) return false;
    for (int l = 0; l < NL; l++) if (i == P_ON[l] || i == P_CH[l]) return false;
    return true;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* ===========================================================================
 * Presets: 32 slots, one line each ("<slot>:<state>") in <plugin dir>/cadenza_BANK.txt
 * ========================================================================= */
static std::string plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&plugin_dir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.rfind('/');
        if (s != std::string::npos) return p.substr(0, s);
    }
    return "/tmp";
}
static const int NPRESETS = 32;
static std::mutex g_bank_mx;
static std::string g_bank[NPRESETS];
static bool g_bank_loaded = false;
static std::string bank_file() {
    const char *env = std::getenv("CADENZA_BANK");
    return env ? env : plugin_dir() + "/cadenza_BANK.txt";
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    FILE *f = std::fopen(bank_file().c_str(), "r");
    if (!f) return;
    static char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        int slot = std::atoi(line);
        char *c = std::strchr(line, ':');
        if (!c || slot < 1 || slot > NPRESETS) continue;
        std::string s(c + 1);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        g_bank[slot - 1] = s;
    }
    std::fclose(f);
}
static void bank_write_async() {   /* file I/O off the audio thread */
    std::string out;
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        for (int i = 0; i < NPRESETS; i++) if (!g_bank[i].empty()) out += std::to_string(i + 1) + ":" + g_bank[i] + "\n";
    }
    std::thread([out]() {
        std::string p = bank_file();
        std::lock_guard<std::mutex> lk(g_bank_mx);
        std::string tmp = p + ".tmp";
        if (FILE *f = std::fopen(tmp.c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            std::rename(tmp.c_str(), p.c_str());
        }
    }).detach();
}

/* ===========================================================================
 * Generation result: per lane a looped schedule in pulses + display text per bar
 * ========================================================================= */
struct SEv { int pulse; uint8_t kind, d1, d2; };   /* kind 0 = CC, 1 = note off, 2 = note on (Cadenza's order) */
static bool sev_less(const SEv &a, const SEv &b) { return a.pulse != b.pulse ? a.pulse < b.pulse : a.kind < b.kind; }
struct Gen {
    std::vector<SEv> sched[NL];
    int len = 16 * PPB;              /* loop length in pulses */
    int bars = 16;
    int first_part = 0;              /* the part (pass) the loop starts with */
    int bpm = 128;
    std::string chords;
    std::string txt[NL][16];         /* "A2  X.x- X... ..." per bar of the loop */
};
struct Snap {   /* everything generation reads, taken under the parameter lock */
    int key, scale, style, seed, prog, form, feel, tempo_ref, swing, gate, pad, ccout, bpm;
    int oct[NL], var[NL];
};

static void build_gen(const Snap &s, Gen *g) {
    cz::Ctx c;
    c.key = cz::make_key(s.key, s.scale);
    c.bpm = s.bpm;
    c.family = FAMILY[std::max(0, std::min(2, s.style))];
    c.offline_style = BASS_FEEL[std::max(0, std::min(3, s.feel))];
    std::string seed = std::to_string(s.seed);
    c.prog = cz::select_progression(c.key.root, c.key.scale, seed, s.prog);
    g->bpm = s.bpm;
    g->chords.clear();
    for (int i = 0; i < 4; i++) g->chords += (i ? " " : "") + c.prog.c[i].root + cz::quality_suffix(c.prog.c[i].quality);
    int p0 = s.form <= 0 ? 0 : s.form - 1, np = s.form <= 0 ? 4 : 1;
    g->first_part = p0;
    g->bars = np * 4;
    g->len = g->bars * PPB;
    long r0 = (long)p0 * cz::MOTIF * cz::TPS, r1 = r0 + (long)np * cz::MOTIF * cz::TPS;
    int sw = (int)std::lround((s.swing - 50) * PPS / 50.0);   /* 0..12 pulses on odd 16ths */
    bool sweep = s.ccout == 1 || s.ccout == 3, porta = s.ccout == 2 || s.ccout == 3;
    for (int l = 0; l < NL; l++) {
        c.seed = s.var[l] > 0 ? seed + "." + std::to_string(s.var[l]) : seed;
        cz::Render r = cz::render_stem(l, c);
        std::vector<SEv> &ev = g->sched[l];
        ev.clear();
        int shift = 12 * s.oct[l];
        auto add_note = [&](int pon, int poff, int note, int vel) {
            note += shift;
            if (note < 0 || note > 127) return;
            ev.push_back({pon % g->len, 2, (uint8_t)note, (uint8_t)std::max(1, std::min(127, vel))});
            ev.push_back({poff % g->len, 1, (uint8_t)note, 0});
        };
        std::vector<int> voicing[4];
        if (l == 5 && s.pad == 1)
            for (int k = 0; k < 4; k++) voicing[k] = cz::pad_voicing(c.prog.c[k], c.family);
        /* display: per bar 16 chars, X >= 100, x normal, - soft (<= 60), . rest; first note's name in front */
        char grid[16][17];
        std::string first[16];
        for (int b = 0; b < g->bars; b++) { std::memset(grid[b], '.', 16); grid[b][16] = 0; first[b].clear(); }
        for (const cz::RNote &n : r.notes) {
            if (n.on < r0 || n.on >= r1) continue;
            long ron = n.on - r0, roff = n.off - r0;
            if (!n.slide && s.gate != 100) roff = ron + std::max(1L, (n.off - n.on) * s.gate / 100);
            int pon = (int)((ron + TPP / 2) / TPP), poff = (int)((roff + TPP / 2) / TPP);
            if (poff <= pon) poff = pon + 1;
            int in_bar = (n.step % cz::MOTIF) % 16;
            if (in_bar % 2 == 1) { pon += sw; poff += sw; }
            if (l == 5 && s.pad == 1) {
                const std::vector<int> &v = voicing[(n.step % cz::MOTIF) / 16];
                int off = 0;
                bool found = false;
                for (int x : v) if (x % 12 == n.note % 12) { off = n.note - x; found = true; break; }
                if (!found) add_note(pon, poff, n.note, n.vel);
                for (int x : v) add_note(pon, poff, x + off, n.vel);
            } else {
                add_note(pon, poff, n.note, n.vel);
            }
            int bar = (int)(ron / (16 * cz::TPS)), st = (int)((ron / cz::TPS) % 16);
            if (bar >= 0 && bar < g->bars) {
                char ch = n.vel >= 100 ? 'X' : n.vel <= 60 ? '-' : 'x';
                if (grid[bar][st] == '.' || ch == 'X') grid[bar][st] = ch;
                if (first[bar].empty()) first[bar] = cz::midi_to_note(std::max(0, std::min(127, n.note + shift)));
            }
        }
        for (const cz::Ev &e : r.ev) {
            if (e.type != cz::EV_CC) continue;
            bool want = (e.a == 74 && sweep) || ((e.a == 65 || e.a == 5) && porta);
            if (!want) continue;
            if (e.a == 5) { ev.push_back({0, 0, 5, (uint8_t)e.b}); continue; }
            if (e.tick < r0 || e.tick >= r1) continue;
            int p = (int)((e.tick - r0 + TPP / 2) / TPP) % g->len;
            ev.push_back({p, 0, (uint8_t)e.a, (uint8_t)std::max(0, std::min(127, e.b))});
        }
        std::stable_sort(ev.begin(), ev.end(), sev_less);
        for (int b = 0; b < g->bars; b++) {
            std::string t = first[b].empty() ? "--" : first[b];
            while (t.size() < 4) t += ' ';
            for (int k = 0; k < 16; k++) { if (k && k % 4 == 0) t += ' '; t += grid[b][k]; }
            g->txt[l][b] = t;
        }
    }
}

/* ===========================================================================
 * Per-instance state
 * ========================================================================= */
struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::mutex mx;                          /* audio lock: schedules, held notes, ALSA */
    std::atomic<float> cache[NPARAMS];      /* what the host sees (normalised) */
    volatile int release[NPARAMS] = {0};    /* momentary triggers / popups to report back */
    float open[NPARAMS] = {0};              /* popup "open" flags (popup.h): wrapper-only */
    std::unique_ptr<Gen> gen;               /* current schedules (swapped under mx) */
    uint8_t held[NL][128];                  /* note-on count per lane and note */
    uint8_t held_ch[NL][128];
    uint32_t rng = 0x12345678u;
    float sr = 44100.0f;
    bool was_playing = false;
    PpqClock clk;
    long long last_pulse = -1;
    int shown_bar = -1;
    bool refresh = false;
    uint8_t inq[64][3];
    int in_n = 0;
    long blocks = 0, no_ti = 0, notes = 0, cc_in = 0, cc_used = 0, gens = 0;
    double t_last_log = 0, last_ppq = 0, last_tempo = 0;
    char chunk[4096];
    std::atomic<int> bpm_hint{128};          /* last host tempo (or the project's), rounded */
    std::atomic<int> gen_bpm{128};           /* the tempo the current pattern was made with */
    bool lane_was_on[NL];
    std::atomic<std::thread::id> audio_tid{std::thread::id()};
    /* generation */
    std::mutex gen_mx;                      /* serialises snapshot -> build -> install */
    std::mutex wmx;
    std::condition_variable wcv;
    bool wdirty = false, wquit = false;
    std::thread worker;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int port = -1, ctl_port = -1;
#endif
};

static int ival(Plugin *w, int i) { return i < 0 ? 0 : norm_to_ui(&PARAMS[i], w->cache[i].load()); }
static void setv(Plugin *w, int i, int v) { if (i >= 0) w->cache[i].store(ui_to_norm(&PARAMS[i], v)); }
static uint32_t xr(Plugin *w) { uint32_t x = w->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return w->rng = x; }

/* ---- MIDI out / Control In (ALSA) ------------------------------------------------ */
static void apply_cc(Plugin *w, int ch, int cc, int val);
#ifndef NO_ALSA
static void ctl_subscribe(Plugin *w, int client, int port) {
    if (client == snd_seq_client_id(w->seq) || client == SND_SEQ_CLIENT_SYSTEM) return;
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_port_info(w->seq, client, port, pi) < 0) return;
    unsigned cap = snd_seq_port_info_get_capability(pi), type = snd_seq_port_info_get_type(pi);
    if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return;
    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) return;
    if (!(type & SND_SEQ_PORT_TYPE_HARDWARE)) return;   /* not other plugins' virtual ports */
    int rc = snd_seq_connect_from(w->seq, w->ctl_port, client, port);
    LOG("[cadenza_vst] control in <- %d:%d '%s'%s\n", client, port, snd_seq_port_info_get_name(pi),
        rc < 0 ? " (already connected or refused)" : "");
}
static void ctl_subscribe_all(Plugin *w) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) ctl_subscribe(w, c, snd_seq_port_info_get_port(pi));
    }
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[cadenza_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, PLUG_NAME);
    w->port = snd_seq_create_simple_port(w->seq, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[cadenza_vst] ALSA client '" PLUG_NAME "' (%d) port %d\n", snd_seq_client_id(w->seq), w->port);
    w->ctl_port = snd_seq_create_simple_port(w->seq, "Control In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (w->ctl_port < 0) { LOG("[cadenza_vst] no Control In port\n"); return; }
    snd_seq_connect_from(w->seq, w->ctl_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    ctl_subscribe_all(w);
}
static void midi_out(Plugin *w, int st, int d1, int d2) {
    if (!w->seq || w->port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    int type = st & 0xF0, ch = st & 0x0F;
    if (type == 0x90 && d2) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
    else if (type == 0xB0) snd_seq_ev_set_controller(&ev, ch, d1, d2);
    else return;
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_poll(Plugin *w) {   /* audio thread, w->mx held */
    if (!w->seq || w->ctl_port < 0) return;
    snd_seq_event_t *ev = nullptr;
    for (int guard = 0; guard < 256 && snd_seq_event_input_pending(w->seq, 1) > 0; guard++) {
        if (snd_seq_event_input(w->seq, &ev) < 0 || !ev) break;
        if (ev->type == SND_SEQ_EVENT_CONTROLLER) { w->cc_in++; apply_cc(w, ev->data.control.channel, (int)ev->data.control.param, ev->data.control.value); }
        else if (ev->type == SND_SEQ_EVENT_PORT_START) ctl_subscribe(w, ev->data.addr.client, ev->data.addr.port);
    }
}
static void alsa_close(Plugin *w) { if (w->seq) snd_seq_close(w->seq); w->seq = nullptr; }
#else
/* Offline test build: CADENZA_TRACE=1 prints every outgoing message. */
static void alsa_open(Plugin *) {}
static void midi_out(Plugin *, int st, int d1, int d2) {
    static int trace = std::getenv("CADENZA_TRACE") ? 1 : 0;
    if (trace) std::printf("MIDI %02x %02x %02x\n", st, d1, d2);
}
static void alsa_poll(Plugin *) {}
static void alsa_close(Plugin *) {}
#endif

/* ---- held notes ------------------------------------------------------------------- */
static void lane_all_off(Plugin *w, int l) {
    for (int n = 0; n < 128; n++)
        if (w->held[l][n]) { midi_out(w, 0x80 | w->held_ch[l][n], n, 0); w->held[l][n] = 0; }
}
static void all_off(Plugin *w) { for (int l = 0; l < NL; l++) lane_all_off(w, l); }

/* ---- generation ------------------------------------------------------------------- */
static Snap snapshot(Plugin *w) {
    Snap s;
    s.key = ival(w, P_KEY); s.scale = ival(w, P_SCALE); s.style = ival(w, P_STYLE); s.seed = ival(w, P_SEED);
    s.prog = ival(w, P_PROG); s.form = ival(w, P_FORM); s.feel = ival(w, P_FEEL); s.tempo_ref = ival(w, P_TEMPO);
    s.swing = ival(w, P_SWING); s.gate = ival(w, P_GATE); s.pad = ival(w, P_PAD); s.ccout = ival(w, P_CCOUT);
    s.bpm = s.tempo_ref > TEMPO_HOST ? s.tempo_ref : w->bpm_hint.load();
    for (int l = 0; l < NL; l++) { s.oct[l] = ival(w, P_OCT[l]); s.var[l] = ival(w, P_LVAR[l]); }
    return s;
}
static bool same_sched(const std::vector<SEv> &a, const std::vector<SEv> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].pulse != b[i].pulse || a[i].kind != b[i].kind || a[i].d1 != b[i].d1 || a[i].d2 != b[i].d2) return false;
    return true;
}
/* snapshot -> build (no audio lock) -> install (audio lock, quick). Any thread but the audio thread. */
static void regenerate(Plugin *w) {
    std::lock_guard<std::mutex> glk(w->gen_mx);
    Snap s = snapshot(w);
    std::unique_ptr<Gen> g(new Gen());
    double t0 = now_ms();
    build_gen(s, g.get());
    double t1 = now_ms();
    std::lock_guard<std::mutex> lk(w->mx);
    for (int l = 0; l < NL; l++)
        if (!w->gen || !same_sched(w->gen->sched[l], g->sched[l])) lane_all_off(w, l);   /* no hanging notes */
    w->gen_bpm.store(g->bpm);
    w->gen = std::move(g);
    w->gens++;
    w->shown_bar = -1;
    w->refresh = true;
    LOG("[cadenza_vst] generated %s %s seed %d @ %d bpm in %.2f ms: %s\n", cz::ROOTS[s.key % 12],
        PARAMS[P_SCALE].opts[std::max(0, std::min(PARAMS[P_SCALE].nopts - 1, s.scale))], s.seed, s.bpm, t1 - t0, w->gen->chords.c_str());
}
static void worker_main(Plugin *w) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(w->wmx);
            w->wcv.wait(lk, [w] { return w->wdirty || w->wquit; });
            if (w->wquit) return;
            w->wdirty = false;
        }
        regenerate(w);
    }
}
static void request_async(Plugin *w) {
    { std::lock_guard<std::mutex> lk(w->wmx); w->wdirty = true; }
    w->wcv.notify_one();
}
/* from parameter changes: build right away, unless we are on the audio thread */
static void request_regen(Plugin *w) {
    if (std::this_thread::get_id() == w->audio_tid.load()) request_async(w);
    else regenerate(w);
}

/* ---- state string (project chunk and preset slots): "key=value;" ---- */
static std::string state_string(Plugin *w, bool with_globals) {
    std::string s = "v=1;";
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_value_param(i)) continue;
        if (!with_globals && (i == P_CCCH || i == P_PRESET)) continue;
        std::snprintf(buf, sizeof buf, "%s=%d;", PARAMS[i].key, ival(w, i));
        s += buf;
    }
    s += "bpm=" + std::to_string(w->gen_bpm.load()) + ";";   /* no lock: also called with w->mx held */
    return s;
}
/* sets the values only; the caller regenerates */
static void apply_state(Plugin *w, const char *str, bool with_globals) {
    std::string copy(str);
    char *save = nullptr;
    for (char *tok = strtok_r(&copy[0], ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        char *eq = std::strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!std::strcmp(tok, "bpm")) { int b = std::atoi(v); if (b >= 40 && b <= 300) w->bpm_hint.store(b); continue; }
        int i = param_index(tok);
        if (i < 0 || !is_value_param(i)) continue;
        if (!with_globals && (i == P_CCCH || i == P_PRESET)) continue;
        setv(w, i, std::atoi(v));
    }
}
static bool preset_load(Plugin *w) {   /* true: values changed, regenerate */
    int slot = ival(w, P_PRESET) - 1;
    std::string s;
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        bank_load_locked();
        s = g_bank[slot];
    }
    LOG("[cadenza_vst] preset %d %s\n", slot + 1, s.empty() ? "empty" : "loaded");
    if (s.empty()) return false;
    apply_state(w, s.c_str(), false);
    return true;
}
static void preset_save(Plugin *w) {
    int slot = ival(w, P_PRESET) - 1;
    std::string st = state_string(w, false);
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        bank_load_locked();
        g_bank[slot] = st;
    }
    bank_write_async();
    LOG("[cadenza_vst] preset %d saved\n", slot + 1);
}

/* ---- parameter changes with side effects ------------------------------------------ */
/* returns true if the pattern has to be generated again */
static bool trigger(Plugin *w, int i) {
    if (i == P_GEN) {
        setv(w, P_SEED, (int)(xr(w) % 10000));
        for (int l = 0; l < NL; l++) setv(w, P_LVAR[l], 0);
        return true;
    }
    if (i == P_VAR) { for (int l = 0; l < NL; l++) setv(w, P_LVAR[l], 1 + (int)(xr(w) % 99)); return true; }
    if (i == P_LOAD) return preset_load(w);
    if (i == P_SAVE) { preset_save(w); return false; }
    for (int l = 0; l < NL; l++)
        if (i == P_DICE[l]) {
            int old = ival(w, P_LVAR[l]), v;
            do v = 1 + (int)(xr(w) % 99); while (v == old);
            setv(w, P_LVAR[l], v);
            return true;
        }
    return false;
}

/* ---- control by MIDI CC (see header); audio thread, w->mx held ----------------------- */
static int cc_param(int cc) {
    static const int *const LANE[5] = {P_ON, P_CH, P_OCT, P_LVAR, P_DICE};
    if (cc >= 10 && cc <= 79 && cc % 10 <= 4) return LANE[cc % 10][cc / 10 - 1];
    const int GLOB[17] = {P_KEY, P_SCALE, P_STYLE, P_FEEL, P_SEED, P_PROG, P_GEN, P_VAR, P_FORM, P_SWING, P_GATE,
                          P_PAD, P_CCOUT, P_TEMPO, P_PRESET, P_LOAD, P_SAVE};
    if (cc >= 100 && cc <= 116) return GLOB[cc - 100];
    return -1;
}
static void apply_cc(Plugin *w, int ch, int cc, int val) {
    if (P_CCCH < 0) return;
    int sel = ival(w, P_CCCH);                 /* 0 = OFF, else channel 1..16 */
    if (sel == 0 || ch != sel - 1) return;
    int i = cc_param(cc);
    if (i < 0) return;
    const param_t *p = &PARAMS[i];
    val = std::max(0, std::min(127, val));
    w->cc_used++;
    if (p->momentary) { if (val >= 64 && trigger(w, i)) request_async(w); return; }
    int old = ival(w, i);
    w->cache[i].store(ui_to_norm(p, norm_to_ui(p, val / 127.0f)));
    if (ival(w, i) != old && shapes_pattern(i)) request_async(w);
    w->refresh = true;
}

/* ---- playback (audio thread, w->mx held) -------------------------------------------- */
static void run_pulse(Plugin *w, long long idx) {
    if (idx < 0 || !w->gen) return;                        /* pre-roll before bar 1 */
    const Gen &g = *w->gen;
    int pos = (int)(idx % g.len);
    int bar = pos / PPB;
    if (bar != w->shown_bar) { w->shown_bar = bar; w->refresh = true; }
    for (int l = 0; l < NL; l++) {
        const std::vector<SEv> &ev = g.sched[l];
        SEv key = {pos, 0, 0, 0};
        auto it = std::lower_bound(ev.begin(), ev.end(), key, [](const SEv &a, const SEv &b) { return a.pulse < b.pulse; });
        bool on = ival(w, P_ON[l]) != 0;
        int ch = (ival(w, P_CH[l]) - 1) & 15;
        for (; it != ev.end() && it->pulse == pos; ++it) {
            if (it->kind == 1) {                            /* note off: only when the last note-on of it ends */
                uint8_t &h = w->held[l][it->d1];
                if (h && --h == 0) midi_out(w, 0x80 | w->held_ch[l][it->d1], it->d1, 0);
            } else if (!on) {
                continue;
            } else if (it->kind == 2) {
                uint8_t &h = w->held[l][it->d1];
                if (h < 255) h++;
                w->held_ch[l][it->d1] = (uint8_t)ch;
                midi_out(w, 0x90 | ch, it->d1, it->d2);
                w->notes++;
            } else {
                midi_out(w, 0xB0 | ch, it->d1, it->d2);
            }
        }
        if (!on && w->lane_was_on[l]) lane_all_off(w, l);  /* switched off while notes were held */
        w->lane_was_on[l] = on;
    }
    w->last_pulse = idx;
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    w->audio_tid = std::this_thread::get_id();
#ifdef PLUG_EFFECT
    for (int ch = 0; ch < 2; ch++) {     /* insert: the track's audio goes through untouched (in may == out) */
        if (in && in[ch]) { if (out[ch] != in[ch]) std::memmove(out[ch], in[ch], (size_t)n * sizeof(float)); }
        else std::memset(out[ch], 0, (size_t)n * sizeof(float));
    }
#else
    (void)in;
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;
#endif
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    if (!ti) w->no_ti++;
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    bool update_display = false;
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo >= 20 && ti->tempo <= 400) w->bpm_hint.store((int)std::lround(ti->tempo));
    {
        std::lock_guard<std::mutex> lk(w->mx);
        for (int i = 0; i < w->in_n; i++)              /* CCs sent to the track */
            if ((w->inq[i][0] & 0xF0) == 0xB0) apply_cc(w, w->inq[i][0] & 0x0F, w->inq[i][1], w->inq[i][2]);
        w->in_n = 0;
        alsa_poll(w);
        if (playing && !w->was_playing) {
            w->clk.resync = true;
            /* TEMPO REF = HOST: a pattern made at another tempo is made again for this one */
            if (ival(w, P_TEMPO) <= TEMPO_HOST && w->gen && w->gen->bpm != w->bpm_hint.load()) request_async(w);
        }
        if (!playing && w->was_playing) { all_off(w); w->last_pulse = -1; w->shown_bar = -1; w->refresh = true; }
        w->was_playing = playing;
        if (playing && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            double sr = ti->sampleRate > 0 ? ti->sampleRate : (double)w->sr;
            long long from = 0, to = -1;
            bool jumped = false;
            if (ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, n, &from, &to, &jumped)) {
                if (jumped) { all_off(w); LOG("[cadenza_vst] ppq jump to %.4f\n", ti->ppqPos); }
                for (long long idx = from; idx <= to; idx++) run_pulse(w, idx);
            }
            w->last_ppq = ti->ppqPos; w->last_tempo = ti->tempo;
        }
        if (w->refresh) { w->refresh = false; update_display = true; }
    }
    w->blocks++;
    double t1 = now_ms();
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[cadenza_vst] %ld blocks, playing %d, ppq %.3f, bpm %.2f | clock: %ld pulses, %ld caught up, %ld overlapped,"
                " %ld jumps | notes %ld | generated %ld | no timeinfo %ld | control cc %ld seen, %ld used\n",
                w->blocks, (int)w->was_playing, w->last_ppq, w->last_tempo, w->clk.pulses, w->clk.gaps, w->clk.dups,
                w->clk.jumps, w->notes, w->gens, w->no_ti, w->cc_in, w->cc_used);
        w->blocks = w->no_ti = w->notes = w->cc_in = w->cc_used = w->gens = 0;
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->t_last_log = t1;
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    if (update_display) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

/* ---- parameters ------------------------------------------------------------------ */
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (is_readout(i)) return;
    if (p->momentary) {
        if (n > 0.5f) {
            w->release[i] = 1;
            if (trigger(w, i)) request_regen(w);
            std::lock_guard<std::mutex> lk(w->mx);
            w->refresh = true;
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            idx = std::max(0, std::min(p->nopts - 1, idx));
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    int old = ival(w, i);
    w->cache[i].store(clamp01(n));      /* unrounded: slow Q-Link turns accumulate */
    if (ival(w, i) != old && shapes_pattern(i)) request_regen(w);
    {
        std::lock_guard<std::mutex> lk(w->mx);
        w->refresh = true;
    }
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    if (is_readout(i)) return 0.0f;
    return PARAMS[i].momentary ? 0.0f : w->cache[i].load();
}
static void param_display(Plugin *w, int i, char *dst) {   /* w->mx held */
    const param_t *p = &PARAMS[i];
    char buf[64];
    if (p->momentary) { copy_str(dst, "", 24); return; }
    const Gen *g = w->gen.get();
    int bar = w->shown_bar >= 0 ? w->shown_bar : 0;
    if (i == P_STATUS) {
        if (!g) { copy_str(dst, "-", 48); return; }
        int part = g->first_part + (g->bars == 16 ? bar / 4 : 0);
        if (w->last_pulse < 0) std::snprintf(buf, sizeof buf, "stopped  %s  %d BPM", PART_NAMES[part & 3], g->bpm);
        else std::snprintf(buf, sizeof buf, "Bar %d/%d  %s  %d BPM", bar + 1, g->bars, PART_NAMES[part & 3], g->bpm);
        copy_str(dst, buf, 48); return;
    }
    if (i == P_CHORDS) { copy_str(dst, g ? g->chords.c_str() : "-", 48); return; }
    for (int l = 0; l < NL; l++)
        if (i == P_PAT[l]) { copy_str(dst, g && bar < g->bars ? g->txt[l][bar].c_str() : "", 48); return; }
    int ui = norm_to_ui(p, popup_is(i) ? w->open[i] : w->cache[i].load());
    if (i == P_TEMPO) {
        if (ui <= TEMPO_HOST) std::snprintf(buf, sizeof buf, "HOST %d", g ? g->bpm : w->bpm_hint.load());
        else std::snprintf(buf, sizeof buf, "%d BPM", ui);
        copy_str(dst, buf, 24); return;
    }
    for (int l = 0; l < NL; l++) {
        if (i == P_LVAR[l]) {
            if (ui == 0) copy_str(dst, "ORIG", 24);
            else { std::snprintf(buf, sizeof buf, "%d", ui); copy_str(dst, buf, 24); }
            return;
        }
        if (i == P_OCT[l]) { std::snprintf(buf, sizeof buf, ui > 0 ? "+%d" : "%d", ui); copy_str(dst, buf, 24); return; }
    }
    if (p->nopts) { copy_str(dst, p->opts[ui], 24); return; }
    std::snprintf(buf, sizeof buf, "%d%s", ui, p->unit);
    copy_str(dst, buf, 24);
}

/* ---- dispatcher ---------------------------------------------------------------- */
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        {
            std::lock_guard<std::mutex> lk(w->wmx);
            w->wquit = true;
        }
        w->wcv.notify_one();
        if (w->worker.joinable()) w->worker.join();
        {
            std::lock_guard<std::mutex> lk(w->mx);
            all_off(w);
            for (int l = 0; l < NL; l++) midi_out(w, 0xB0 | ((ival(w, P_CH[l]) - 1) & 15), 123, 0);   /* All Notes Off */
        }
        alsa_close(w);
        LOG("[cadenza_vst] closed\n");
        delete w;
        return 1;
#ifdef PLUG_EFFECT
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
#else
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
#endif
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_readout(idx);
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, "", 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        std::lock_guard<std::mutex> lk(w->mx);
        param_display(w, idx, (char *)p);
        return 1;
    }
    case effSetSampleRate: if (o > 0) w->sr = o; return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        std::lock_guard<std::mutex> lk(w->mx);
        for (int i = 0; ev && i < ev->numEvents && w->in_n < 64; i++) {
            if (ev->events[i]->type != 1) continue;
            std::memcpy(w->inq[w->in_n++], ((VstMidiEvent *)ev->events[i])->midiData, 3);
        }
        return 1;
    }
    case effCanDo: {
        const char *s = (const char *)p;
        return (!std::strcmp(s, "receiveVstTimeInfo") || !std::strcmp(s, "receiveVstEvents") ||
                !std::strcmp(s, "receiveVstMidiEvent")) ? 1 : -1;
    }
    case effGetChunk: {
        std::string s = state_string(w, true);
        std::lock_guard<std::mutex> lk(w->mx);
        copy_str(w->chunk, s.c_str(), sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::string s((const char *)p, (size_t)v);
        apply_state(w, s.c_str(), true);
        request_regen(w);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/cadenza_vst.log", "a");
    std::call_once(g_idx_once, init_indices);
    Plugin *w = new Plugin();
    w->master = master;
    w->rng ^= (uint32_t)time(nullptr) ^ (uint32_t)(uintptr_t)w;
    if (!w->rng) w->rng = 1;
    for (int i = 0; i < NPARAMS; i++) w->cache[i].store(PARAMS[i].def);
    std::memset(w->held, 0, sizeof w->held);
    std::memset(w->held_ch, 0, sizeof w->held_ch);
    for (int l = 0; l < NL; l++) w->lane_was_on[l] = true;
    regenerate(w);
    w->worker = std::thread(worker_main, w);
    alsa_open(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
#ifdef PLUG_EFFECT
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
#else
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
#endif
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[cadenza_vst] " PLUG_NAME " up (" BUILD_ID ", " PLUG_MODE "), %d params\n", NPARAMS);
    return e;
}
