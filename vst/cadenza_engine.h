/* =============================================================================
 * cadenza_engine.h - C++ port of the OFFLINE engine of Cadenza (Andrea Cavallo, MIT,
 * github.com/Andrea-Cavallo/cadenza, backend/internal/{theory,generator,renderer}).
 *
 * Same inputs (key root + scale, BPM, seed string, style family, bass offline style), same
 * SHA-256-seeded decisions, same 7 stems and the same renderer (evolution over 4 passes of the
 * 64-step motif = 16 bars, velocity grids, dynamic curves, timing offsets, gates, portamento CC65,
 * CC74 filter sweep). ref/parity.sh compares this port with the Go original event by event.
 *
 * Note names are kept as strings, spelled exactly like Cadenza (sharps, or flats for the roots
 * F Bb Eb Ab Db Gb), because Cadenza compares spelled names in a few places (approach notes,
 * bass colour tone) and hashes the key root by name.
 *
 * Additions that do not exist in Cadenza (no effect on the original scales):
 *   - scale "phrygian_dominant" (Goa phrygian, 1 b2 3 4 5 b6 b7) with its own progression pool;
 *     treated like a minor key wherever Cadenza checks for minor (palettes, profiles, sweep)
 *   - "minor_harmonic" selectable (Cadenza has its intervals and chord qualities; its pool falls
 *     back to the minor progressions, as in Cadenza's progressionPool default)
 * ========================================================================== */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sha256.h"

namespace cz {

using std::string;
using std::vector;

/* ---------------------------------------------------------------------------
 * theory (note.go, scale.go, chord.go, progression.go)
 * ------------------------------------------------------------------------- */
static const char *const SHARPS[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const FLATS[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

static inline bool use_flats(const string &r) {
    return r == "F" || r == "Bb" || r == "Eb" || r == "Ab" || r == "Db" || r == "Gb";
}
static inline int note_sem(const string &n) {
    static const struct { const char *n; int s; } T[] = {
        {"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"Fb", 4}, {"F", 5}, {"F#", 6},
        {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}, {"Cb", 11}};
    for (auto &t : T) if (n == t.n) return t.s;
    return -1;
}
/* NoteToMIDI: "A2" -> 45; -1 on any error (Go returns an error) */
static inline int note_to_midi(const string &note) {
    if (note.size() < 2) return -1;
    string name, oct;
    if (note.size() >= 3 && (note[1] == '#' || note[1] == 'b')) { name = note.substr(0, 2); oct = note.substr(2); }
    else { name = note.substr(0, 1); oct = note.substr(1); }
    int sem = note_sem(name);
    if (sem < 0 || oct.empty()) return -1;
    size_t i = 0;
    bool neg = false;
    if (oct[0] == '+' || oct[0] == '-') { neg = oct[0] == '-'; i = 1; }
    if (i >= oct.size()) return -1;
    long o = 0;
    for (; i < oct.size(); i++) {
        if (oct[i] < '0' || oct[i] > '9') return -1;
        o = o * 10 + (oct[i] - '0');
        if (o > 1000) return -1;
    }
    if (neg) o = -o;
    if (o < 0 || o > 9) return -1;
    int m = (int)(o + 1) * 12 + sem;
    return (m < 0 || m > 127) ? -1 : m;
}
static inline string midi_to_note(int m) {
    if (m < 0 || m > 127) return "MIDI(" + std::to_string(m) + ")";
    return string(SHARPS[m % 12]) + std::to_string(m / 12 - 1);
}
static inline string name_only(const string &n) {
    if (n.size() >= 3 && (n[1] == '#' || n[1] == 'b')) return n.substr(0, 2);
    if (n.size() >= 1) return n.substr(0, 1);
    return n;
}

struct Key {
    string root;   /* "A", "Bb", "F#" */
    string mode;   /* major minor dorian phrygian mixolydian lydian goa */
    string scale;  /* major minor_natural minor_harmonic dorian phrygian mixolydian lydian phrygian_dominant */
};

static inline const int *scale_iv(const string &s) {
    static const int MAJ[7] = {0, 2, 4, 5, 7, 9, 11}, MIN[7] = {0, 2, 3, 5, 7, 8, 10}, HMIN[7] = {0, 2, 3, 5, 7, 8, 11},
                     DOR[7] = {0, 2, 3, 5, 7, 9, 10}, PHR[7] = {0, 1, 3, 5, 7, 8, 10}, MIX[7] = {0, 2, 4, 5, 7, 9, 10},
                     LYD[7] = {0, 2, 4, 6, 7, 9, 11}, GOA[7] = {0, 1, 4, 5, 7, 8, 10};
    if (s == "major") return MAJ;
    if (s == "minor_natural") return MIN;
    if (s == "minor_harmonic") return HMIN;
    if (s == "dorian") return DOR;
    if (s == "phrygian") return PHR;
    if (s == "mixolydian") return MIX;
    if (s == "lydian") return LYD;
    if (s == "phrygian_dominant") return GOA;
    return nullptr;
}
static inline vector<string> scale_notes(const string &root, const string &scale) {
    const int *iv = scale_iv(scale);
    int rs = note_sem(root);
    if (!iv || rs < 0) return {};
    const char *const *chrom = use_flats(root) ? FLATS : SHARPS;
    vector<string> out(7);
    for (int i = 0; i < 7; i++) out[i] = chrom[(rs + iv[i]) % 12];
    return out;
}
static inline bool note_in_scale(const string &note, const string &root, const string &scale) {
    vector<string> sn = scale_notes(root, scale);
    int ns = note_sem(note);
    if (sn.empty() || ns < 0) return false;
    for (auto &s : sn) if (note_sem(s) == ns) return true;
    return false;
}
static inline vector<string> chord_notes(const string &root, const string &q) {
    static const int MAJ[3] = {0, 4, 7}, MIN[3] = {0, 3, 7}, DIM[3] = {0, 3, 6}, AUG[3] = {0, 4, 8}, S2[3] = {0, 2, 7},
                     S4[3] = {0, 5, 7};
    const int *iv = q == "major" ? MAJ : q == "minor" ? MIN : q == "dim" ? DIM : q == "aug" ? AUG : q == "sus2" ? S2 : q == "sus4" ? S4 : nullptr;
    int rs = note_sem(root);
    if (!iv || rs < 0) return {};
    const char *const *chrom = use_flats(root) ? FLATS : SHARPS;
    return {chrom[(rs + iv[0]) % 12], chrom[(rs + iv[1]) % 12], chrom[(rs + iv[2]) % 12]};
}
static inline const char *const *scale_qualities(const string &s) {
    static const char *const MAJ[7] = {"major", "minor", "minor", "major", "major", "minor", "dim"};
    static const char *const MIN[7] = {"minor", "dim", "major", "minor", "minor", "major", "major"};
    static const char *const HMIN[7] = {"minor", "dim", "aug", "minor", "major", "major", "dim"};
    static const char *const DOR[7] = {"minor", "minor", "major", "major", "minor", "dim", "major"};
    static const char *const PHR[7] = {"minor", "major", "major", "minor", "dim", "major", "minor"};
    static const char *const MIX[7] = {"major", "minor", "dim", "major", "minor", "minor", "major"};
    static const char *const LYD[7] = {"major", "major", "minor", "dim", "major", "minor", "minor"};
    /* Goa phrygian (phrygian dominant): I bII iii(dim) iv v(dim) bVI(aug) bvii */
    static const char *const GOA[7] = {"major", "major", "dim", "minor", "dim", "aug", "minor"};
    if (s == "major") return MAJ;
    if (s == "minor_natural") return MIN;
    if (s == "minor_harmonic") return HMIN;
    if (s == "dorian") return DOR;
    if (s == "phrygian") return PHR;
    if (s == "mixolydian") return MIX;
    if (s == "lydian") return LYD;
    if (s == "phrygian_dominant") return GOA;
    return nullptr;
}
static inline string quality_suffix(const string &q) {
    if (q == "minor") return "m";
    if (q == "dim") return "dim";
    if (q == "aug") return "aug";
    return "";
}

struct PChord { string root, quality; vector<string> notes; };
struct Prog { PChord c[4]; int pool = 0; int index = 0; bool ok = false; };

static const int PROG_MIN[12][4] = {{0, 5, 2, 6}, {0, 3, 5, 4}, {0, 6, 5, 4}, {0, 2, 6, 5}, {0, 3, 6, 2}, {0, 5, 6, 0},
                                    {0, 2, 5, 6}, {0, 3, 0, 6}, {0, 5, 3, 4}, {0, 6, 3, 5}, {0, 4, 5, 6}, {0, 5, 2, 3}};
static const int PROG_MAJ[12][4] = {{0, 4, 5, 3}, {0, 5, 3, 4}, {0, 3, 5, 4}, {0, 2, 3, 4}, {0, 4, 3, 5}, {0, 3, 0, 4},
                                    {0, 5, 4, 3}, {0, 2, 5, 4}, {0, 3, 2, 5}, {0, 4, 2, 5}, {0, 5, 2, 3}, {0, 1, 3, 4}};
static const int PROG_DOR[8][4] = {{0, 3, 0, 3}, {0, 3, 6, 0}, {0, 6, 3, 0}, {0, 1, 3, 6}, {0, 3, 2, 6}, {0, 1, 6, 3}, {0, 3, 1, 6}, {0, 6, 3, 1}};
static const int PROG_PHR[8][4] = {{0, 1, 0, 1}, {0, 1, 2, 1}, {0, 6, 5, 1}, {1, 0, 6, 0}, {0, 5, 1, 0}, {0, 3, 1, 0}, {0, 1, 5, 2}, {0, 6, 1, 0}};
static const int PROG_MIX[8][4] = {{0, 6, 3, 0}, {0, 3, 6, 0}, {0, 6, 0, 3}, {0, 1, 3, 6}, {0, 3, 1, 6}, {0, 5, 6, 3}, {0, 6, 5, 3}, {0, 4, 3, 6}};
static const int PROG_LYD[8][4] = {{0, 1, 0, 4}, {0, 4, 1, 0}, {0, 1, 4, 0}, {0, 2, 1, 4}, {0, 1, 5, 4}, {0, 4, 5, 1}, {0, 1, 2, 5}, {0, 5, 1, 4}};
/* Goa phrygian (not in Cadenza): the I-bII vamp and its relatives, all landing back on I */
static const int PROG_GOA[8][4] = {
    {0, 1, 0, 1},   /* I - bII - I - bII     the Goa vamp */
    {0, 1, 6, 0},   /* I - bII - bvii - I */
    {0, 6, 1, 0},   /* I - bvii - bII - I */
    {0, 3, 1, 0},   /* I - iv - bII - I */
    {0, 1, 3, 1},   /* I - bII - iv - bII */
    {1, 0, 6, 0},   /* bII - I - bvii - I */
    {0, 5, 1, 0},   /* I - bVI+ - bII - I    augmented colour */
    {0, 3, 6, 1}};  /* I - iv - bvii - bII */

/* progressionPool + SelectProgression; force > 0 picks template force-1 instead of the hash */
static inline Prog select_progression(const string &root, const string &scale, const string &seed, int force = 0) {
    Prog p;
    const char *const *q = scale_qualities(scale);
    vector<string> sn = scale_notes(root, scale);
    if (!q || sn.size() != 7) return p;
    const int(*tpl)[4] = PROG_MIN;
    int n = 12;
    if (scale == "major") { tpl = PROG_MAJ; n = 12; }
    else if (scale == "minor_natural") { tpl = PROG_MIN; n = 12; }
    else if (scale == "dorian") { tpl = PROG_DOR; n = 8; }
    else if (scale == "phrygian") { tpl = PROG_PHR; n = 8; }
    else if (scale == "mixolydian") { tpl = PROG_MIX; n = 8; }
    else if (scale == "lydian") { tpl = PROG_LYD; n = 8; }
    else if (scale == "phrygian_dominant") { tpl = PROG_GOA; n = 8; }
    Sha256 h = sha256(seed + root + scale);
    uint32_t v = (uint32_t)h.d[0] << 24 | (uint32_t)h.d[1] << 16 | (uint32_t)h.d[2] << 8 | h.d[3];
    int idx = (int)(v % (uint32_t)n);
    if (force > 0) idx = (force - 1) < n ? force - 1 : n - 1;
    for (int j = 0; j < 4; j++) {
        int deg = tpl[idx][j];
        p.c[j].root = sn[deg];
        p.c[j].quality = q[deg];
        p.c[j].notes = chord_notes(sn[deg], q[deg]);
    }
    p.pool = n;
    p.index = idx;
    p.ok = true;
    return p;
}

/* ---------------------------------------------------------------------------
 * pattern spec (schema/pattern_spec.go, only what the offline path uses)
 * ------------------------------------------------------------------------- */
struct Step {
    bool active = false;
    string note;
    bool accent = false, slide = false, ghost = false, legato = false, staccato = false;
};
static inline Step S(const string &note, bool accent = false, bool slide = false, bool ghost = false, bool legato = false,
                     bool staccato = false) {
    Step s;
    s.active = true; s.note = note; s.accent = accent; s.slide = slide; s.ghost = ghost; s.legato = legato; s.staccato = staccato;
    return s;
}
/* readable builders: flags by name */
enum { F_ACC = 1, F_SLIDE = 2, F_GHOST = 4, F_LEG = 8, F_STAC = 16 };
static inline Step N(const string &note, int f = 0) {
    return S(note, f & F_ACC, f & F_SLIDE, f & F_GHOST, f & F_LEG, f & F_STAC);
}
struct Evo { int from, to; const char *action; double intensity; };
struct Spec {
    string pattern_type;     /* bassline / arpeggio / melody / chord_pad / lead_stab */
    string profile;          /* StyleProfile name requested by the generator */
    vector<Step> steps;      /* 64 */
    vector<Evo> evo;
    string sweep;            /* "" = no CC74 */
    string seed;
};

static const int MOTIF = 64, SECT = 16;
using H = Sha256;
static inline int hb(const H &h, int i) { return h.d[((i % 32) + 32) % 32]; }
static inline H seed_hash(const string &s) { return sha256(s); }

struct Ctx {
    double bpm;
    Key key;
    Prog prog;
    string seed;
    string offline_style;   /* bass: melodic hypnotic driving minimal ("" = melodic) */
    string family;          /* groove rolling sub */
};

/* ---- helpers (offline.go) ---- */
static inline bool mode_minor(const Key &k) { return k.mode == "minor" || k.mode == "goa"; }
static inline bool minor_family(const Key &k) {
    return k.mode == "minor" || k.mode == "dorian" || k.mode == "phrygian" || k.mode == "goa";
}
static inline int normalize_degree(int d, int len) {
    if (len <= 0) return 0;
    d %= len;
    if (d < 0) d += len;
    return d;
}
static inline int character_degree(const Key &k) {
    const string &s = k.scale;
    if (s == "minor_natural" || s == "minor_harmonic") return 5;
    if (s == "dorian") return 5;
    if (s == "phrygian" || s == "phrygian_dominant") return 1;
    if (s == "mixolydian") return 6;
    if (s == "lydian") return 3;
    return 4;
}
static inline vector<string> scale_or_c(const Key &k) {
    vector<string> sn = scale_notes(k.root, k.scale);
    if (sn.size() < 7) sn = {"C", "D", "E", "F", "G", "A", "B"};
    return sn;
}
static inline string approach_note(const string &root, const Key &k) {
    vector<string> sn = scale_notes(k.root, k.scale);
    if (sn.empty()) return root;
    for (size_t i = 0; i < sn.size(); i++)
        if (sn[i] == root) return i == 0 ? sn.back() : sn[i - 1];
    return root;
}
static inline string chord_fifth(const PChord &c) {
    vector<string> n = chord_notes(c.root, c.quality);
    return n.size() < 3 ? c.root : n[2];
}
static inline string chord_third(const PChord &c) {
    vector<string> n = chord_notes(c.root, c.quality);
    return n.size() < 2 ? c.root : n[1];
}
static inline vector<string> chord_notes_or_root(const PChord &c) {
    vector<string> n = chord_notes(c.root, c.quality);
    if (n.size() < 3) return {c.root, c.root, c.root};
    return n;
}
static inline string scale_degree_name(const Key &k, int deg) {
    vector<string> sn = scale_notes(k.root, k.scale);
    if (sn.empty()) return k.root;
    return sn[normalize_degree(deg, (int)sn.size())];
}
static inline string bass_color_tone(const Key &k, const PChord &c) {
    vector<string> ct = chord_notes(c.root, c.quality);
    if (ct.empty()) return chord_third(c);
    string color = scale_degree_name(k, character_degree(k));
    for (auto &t : ct) if (t == color) return color;
    return chord_third(c);
}
static inline string closest_chord_tone(const vector<string> &ct, const vector<string> &sn, int degree) {
    string target = sn[degree % (int)sn.size()];
    int tm = note_to_midi(target + "4");
    if (tm < 0) return ct[0];
    string best = ct[0];
    int bd = 0x7fffffff;
    for (auto &c : ct) {
        int m = note_to_midi(c + "4");
        if (m < 0) continue;
        int d = std::abs(m - tm);
        if (d > 6) d = 12 - d;
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}
static inline string note_octave(const string &n) {
    if (n.empty()) return "4";
    char l = n.back();
    if (l >= '0' && l <= '9') return string(1, l);
    return "4";
}
static inline string melody_note(const string &name, const string &pref, bool higher) {
    string oct = pref;
    if (higher && oct == "5") oct = "6";
    string note = name + oct;
    int m = note_to_midi(note);
    if (m < 0) return name + pref;
    if (m < 60) return name + "5";
    if (m > 84) return name + "4";
    return note;
}
static inline string arp_clamp(const string &note) {
    string name = name_only(note), full = name + note_octave(note);
    int m = note_to_midi(full);
    if (m < 0) return full;
    if (m < 48 || m > 84) return name + "4";
    return full;
}
static inline string chromatic_approach(const string &rootOct) {
    int m = note_to_midi(rootOct);
    if (m < 0 || m <= 24) return rootOct;
    return midi_to_note(m - 1);
}
static inline int active_count(const Step *s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += s[i].active;
    return c;
}
static inline int active_count(const vector<Step> &s) { return active_count(s.data(), (int)s.size()); }

static inline void mix_gate_articulation(vector<Step> &st, const H &h) {
    for (size_t i = 0; i < st.size(); i++) {
        if (!st[i].active || st[i].slide || st[i].ghost) continue;
        switch (hb(h, (int)(i % 32)) % 4) {
        case 0: st[i].legato = true; break;
        case 1: st[i].staccato = true; break;
        }
    }
}
static inline void ensure_gate_variation(vector<Step> &st) {
    bool hl = false, hs = false;
    for (auto &s : st) { hl = hl || s.legato; hs = hs || s.staccato; }
    if (hl && hs) return;
    for (auto &s : st) {
        if (!s.active || s.ghost || s.slide) continue;
        if (!hl) { s.legato = true; s.staccato = false; hl = true; continue; }
        if (!hs) { s.staccato = true; s.legato = false; return; }
    }
}
static inline string normalize_offline_style(const string &s) {
    if (s == "hypnotic" || s == "driving" || s == "minimal") return s;
    return "melodic";
}
static inline string family_offline_style(const string &fam, const string &legacy) {
    if (fam == "rolling") return "driving";
    if (fam == "sub") return "hypnotic";
    if (fam == "groove") return "melodic";
    return normalize_offline_style(legacy);
}

/* ---- bass ---- */
static inline void fill_bass_sub(vector<Step> &st, int b, int pat, const string &root, const string &fifth, const string &color,
                                 const string &highRoot, const string &approach) {
    switch (pat) {
    case 0: st[b] = N(root, F_ACC); st[b + 1] = N(color); st[b + 2] = N(fifth); st[b + 3] = N(root, F_SLIDE); break;
    case 1: st[b] = N(root, F_ACC); st[b + 1] = Step(); st[b + 2] = N(fifth); st[b + 3] = N(root, F_GHOST); break;
    case 2: st[b] = N(root, F_ACC); st[b + 1] = N(highRoot); st[b + 2] = Step(); st[b + 3] = N(root, F_SLIDE); break;
    case 3: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = Step(); st[b + 2] = N(root, F_GHOST); st[b + 3] = N(fifth, F_SLIDE); break;
    case 4: st[b] = N(root, F_ACC); st[b + 1] = N(color); st[b + 2] = N(root, F_GHOST); st[b + 3] = N(fifth, F_SLIDE); break;
    case 5: st[b] = Step(); st[b + 1] = N(root, F_ACC); st[b + 2] = N(fifth, F_GHOST); st[b + 3] = N(root, F_STAC); break;
    case 6: st[b] = N(root, F_ACC); st[b + 1] = N(root); st[b + 2] = N(approach, F_GHOST); st[b + 3] = N(root, F_SLIDE); break;
    case 7: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = N(root, F_GHOST | F_STAC); st[b + 2] = N(fifth, F_SLIDE); st[b + 3] = N(root, F_GHOST | F_STAC); break;
    case 8: st[b] = N(root, F_ACC); st[b + 1] = N(root, F_GHOST | F_STAC); st[b + 2] = N(approach, F_GHOST | F_SLIDE); st[b + 3] = N(root, F_SLIDE); break;
    case 9: {
        string ca = chromatic_approach(root);
        st[b] = N(root, F_ACC | F_LEG); st[b + 1] = N(ca, F_GHOST | F_STAC); st[b + 2] = N(root); st[b + 3] = N(fifth, F_GHOST | F_SLIDE);
        break;
    }
    case 10: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = N(highRoot, F_GHOST | F_STAC); st[b + 2] = N(root, F_GHOST); st[b + 3] = N(root, F_SLIDE); break;
    case 11: st[b] = N(root, F_ACC); st[b + 1] = N(root); st[b + 2] = N(root, F_GHOST); st[b + 3] = N(fifth, F_SLIDE); break;
    case 12: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = Step(); st[b + 2] = Step(); st[b + 3] = Step(); break;
    case 13: st[b] = N(root, F_LEG); st[b + 1] = Step(); st[b + 2] = N(fifth, F_LEG); st[b + 3] = Step(); break;
    case 14: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = Step(); st[b + 2] = Step(); st[b + 3] = N(root, F_GHOST); break;
    default: st[b] = N(root, F_ACC | F_LEG); st[b + 1] = Step(); st[b + 2] = N(root, F_GHOST | F_STAC); st[b + 3] = N(fifth, F_SLIDE); break;
    }
}
static inline vector<int> groove_palette(double bpm, const Key &k) {
    bool m = minor_family(k);
    if (m && bpm >= 126) return {0, 5, 6};
    if (m) return {1, 3, 5};
    return {0, 4, 1};
}
static inline vector<int> rolling_palette(double bpm, const Key &k) {
    bool m = minor_family(k);
    if (m && bpm >= 126) return {7, 8, 10, 11};
    if (m) return {7, 8, 9, 11};
    return {7, 9, 10, 11};
}
static inline vector<int> sub_palette(double bpm) {
    if (bpm < 120) return {12, 14, 13};
    return {12, 15, 14};
}
static inline void bass_max_density(vector<Step> &st, int maxPer) {
    int secs = (int)st.size() / 16;
    for (int sec = 0; sec < secs; sec++) {
        Step *sl = &st[sec * 16];
        while (active_count(sl, 16) > maxPer) {
            bool removed = false;
            for (int i = 15; i >= 0; i--)
                if (sl[i].active && sl[i].ghost && !sl[i].accent) { sl[i].active = false; removed = true; break; }
            if (removed) continue;
            for (int i = 15; i >= 0; i--)
                if (sl[i].active && !sl[i].accent && !sl[i].slide) { sl[i].active = false; removed = true; break; }
            if (removed) continue;
            /* FIX (not in Cadenza): with only accents and slides left the Go loop never ends -- the sub
             * bass hangs for about one seed in four. Thin further: unaccented slides, then anything but
             * the section downbeat. ref/go carries the same fix so the parity test can run. */
            for (int i = 15; i >= 0; i--)
                if (sl[i].active && !sl[i].accent) { sl[i].active = false; removed = true; break; }
            if (removed) continue;
            for (int i = 15; i >= 1; i--)
                if (sl[i].active) { sl[i].active = false; removed = true; break; }
            if (!removed) break;
        }
    }
}
static inline void bass_min_density(vector<Step> &st, const Prog &p, int minPer, const string &oct) {
    for (int i = 0; i < 4; i++) {
        Step *sl = &st[i * 16];
        string ghost = p.c[i].root + oct;
        while (active_count(sl, 16) < minPer) {
            bool filled = false;
            for (int off = 2; off < 16; off += 4)
                if (!sl[off].active) { sl[off] = N(ghost, F_GHOST); filled = true; break; }
            if (!filled) {
                for (int off = 1; off < 16; off += 4)
                    if (!sl[off].active) { sl[off] = N(ghost, F_GHOST); break; }
                break;
            }
        }
    }
}
static inline string passing_note_before(const string &target, const Key &k) {
    int m = note_to_midi(target + "4");
    if (m >= 0) {
        string cand = name_only(midi_to_note(m - 1));
        if (note_in_scale(cand, k.root, k.scale)) return cand;
    }
    return approach_note(target, k);
}
static inline void apply_passing_notes(const string &pt, vector<Step> &st, const Prog &p, const Key &k, const H &h) {
    int maxActive = pt == "bassline" ? 13 * ((int)st.size() / 16) : pt == "melody" ? 8 * ((int)st.size() / 16) : 0;
    if (!maxActive) return;
    int chance = 15 + hb(h, 20) % 11;
    for (int i = 0; i < 4; i++) {
        if (active_count(st) >= maxActive || hb(h, 21 + i) % 100 >= chance) continue;
        int base = i * 16;
        for (int pos = base + 1; pos <= base + 15 && pos < (int)st.size(); pos++) {
            if (!st[pos].active || st[pos - 1].active) continue;
            string target = name_only(st[pos].note);
            string pass = passing_note_before(target, k);
            if (pass.empty() || pass == target) continue;
            st[pos - 1] = N(pass + note_octave(st[pos].note), F_GHOST | F_STAC);
            break;
        }
    }
}
static inline void apply_bass_style(const string &style, vector<Step> &st, const Prog &p, const H &h) {
    if (style == "hypnotic") {
        for (size_t i = 0; i < st.size(); i += 4) {
            Step *g = &st[i];
            int kept = 0;
            for (int j = 0; j < 4; j++) {
                if (g[j].active && g[j].accent && kept < 2) kept++;
                else if (g[j].active && g[j].accent) g[j].active = false;
            }
            for (int j = 0; j < 4; j++) {
                if (g[j].active && !g[j].ghost && !g[j].accent && kept < 2) kept++;
                else if (g[j].active && !g[j].ghost && !g[j].accent) g[j].active = false;
            }
            for (int j = 0; j < 4; j++) {
                if (g[j].active && g[j].ghost && kept < 2) kept++;
                else if (g[j].active && g[j].ghost) g[j].active = false;
            }
            for (int j = 0; j < 4; j++)
                if (g[j].active) { g[j].legato = true; g[j].staccato = false; g[j].ghost = false; }
        }
        for (int i = 0; i < 4; i++) if (st[i * 16].active) st[i * 16].accent = true;
    } else if (style == "driving") {
        for (int i = 0; i < 4; i++)
            for (int sub = 0; sub < 16; sub++) {
                int pos = i * 16 + sub;
                if (!st[pos].active) continue;
                if (sub % 4 == 1) { st[pos].staccato = true; st[pos].legato = false; }
                if (sub % 4 == 3 && hb(h, pos % 32) % 3 == 0) st[pos].slide = true;
            }
    } else if (style == "minimal") {
        for (size_t i = 0; i < st.size(); i++) if (st[i].ghost && i % 16 != 0) st[i].active = false;
    } else {
        mix_gate_articulation(st, h);
    }
    (void)p;
}
static inline vector<Evo> bass_evolution(double bpm) {
    if (bpm >= 128) return {{1, 1, "build", 0.7}, {2, 2, "build", 0.85}, {3, 3, "peak", 1.0}, {4, 4, "release", 0.6}};
    return {{1, 1, "introduce", 0.5}, {2, 2, "build", 0.7}, {3, 3, "peak", 0.9}, {4, 4, "release", 0.5}};
}
static inline string choose_bass_profile(const Key &k, double bpm, const H &h) {
    if (bpm < 118) { const char *c[2] = {"bass_sub", "bass_progressive"}; return c[hb(h, 0) % 2]; }
    if (bpm >= 128) {
        if (mode_minor(k)) return "bass_driving";
        const char *c[2] = {"bass_driving", "bass_progressive"};
        return c[hb(h, 0) % 2];
    }
    if (mode_minor(k)) { const char *c[2] = {"bass_progressive", "bass_driving"}; return c[hb(h, 0) % 2]; }
    return "bass_progressive";
}

static inline Spec bassline_groove(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "bass");
    Spec s;
    s.pattern_type = "bassline";
    s.profile = choose_bass_profile(k, c.bpm, h);
    string style = normalize_offline_style(c.offline_style);
    s.steps.assign(MOTIF, Step());
    vector<int> pal = groove_palette(c.bpm, k);
    for (int i = 0; i < 4; i++) {
        const PChord &ch = c.prog.c[i];
        int base = i * 16;
        string root = ch.root + "2", fifth = chord_fifth(ch) + "2", color = bass_color_tone(k, ch) + "2";
        string highRoot = ch.root + "3";
        if (std::max(0, note_to_midi(highRoot)) > 55) highRoot = root;
        string approach = approach_note(ch.root, k) + "2";
        string highFifth = chord_fifth(ch) + "3";
        if (std::max(0, note_to_midi(highFifth)) > 55) highFifth = fifth;
        int pi = hb(h, i * 7 + 2) % (int)pal.size();
        int p0 = pal[pi], p1 = pal[(pi + 1 + hb(h, i * 7 + 4)) % (int)pal.size()];
        fill_bass_sub(s.steps, base + 0, p0, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 4, p1, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 8, p0, fifth, root, color, highFifth, approach);
        fill_bass_sub(s.steps, base + 12, p1, fifth, root, color, highFifth, approach);
    }
    apply_bass_style(style, s.steps, c.prog, h);
    if (style != "hypnotic" && style != "minimal") apply_passing_notes("bassline", s.steps, c.prog, k, h);
    ensure_gate_variation(s.steps);
    bass_max_density(s.steps, 13);
    bass_min_density(s.steps, c.prog, 8, "2");
    s.evo = bass_evolution(c.bpm);
    s.sweep = "medium";
    s.seed = c.seed;
    return s;
}
static inline Spec bassline_rolling(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "bass_rolling");
    Spec s;
    s.pattern_type = "bassline";
    s.profile = "bass_rolling";
    s.steps.assign(MOTIF, Step());
    vector<int> pal = rolling_palette(c.bpm, k);
    for (int i = 0; i < 4; i++) {
        const PChord &ch = c.prog.c[i];
        int base = i * 16;
        string root = ch.root + "2", fifth = chord_fifth(ch) + "2", color = bass_color_tone(k, ch) + "2";
        string approach = approach_note(ch.root, k) + "2";
        string highRoot = ch.root + "3";
        if (std::max(0, note_to_midi(highRoot)) > 55) highRoot = root;
        int pi = hb(h, i * 7 + 2) % (int)pal.size();
        int p0 = pal[pi], p1 = pal[(pi + 1 + hb(h, i * 7 + 4)) % (int)pal.size()];
        fill_bass_sub(s.steps, base + 0, p0, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 4, p1, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 8, p0, fifth, root, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 12, p1, fifth, root, color, highRoot, approach);
    }
    bass_min_density(s.steps, c.prog, 14, "2");
    ensure_gate_variation(s.steps);
    s.evo = bass_evolution(c.bpm);
    s.sweep = "dramatic";
    s.seed = c.seed;
    return s;
}
static inline Spec bassline_sub(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "bass_sub");
    Spec s;
    s.pattern_type = "bassline";
    s.profile = "bass_sub";
    s.steps.assign(MOTIF, Step());
    vector<int> pal = sub_palette(c.bpm);
    for (int i = 0; i < 4; i++) {
        const PChord &ch = c.prog.c[i];
        int base = i * 16;
        string root = ch.root + "1", fifth = chord_fifth(ch) + "1", color = bass_color_tone(k, ch) + "1";
        string approach = approach_note(ch.root, k) + "1";
        string highRoot = ch.root + "2";
        if (std::max(0, note_to_midi(highRoot)) > 43) highRoot = root;
        int pi = hb(h, i * 7 + 2) % (int)pal.size();
        int p0 = pal[pi], p1 = pal[(pi + 1 + hb(h, i * 7 + 4)) % (int)pal.size()];
        fill_bass_sub(s.steps, base + 0, p0, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 4, p1, root, fifth, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 8, p0, fifth, root, color, highRoot, approach);
        fill_bass_sub(s.steps, base + 12, p1, fifth, root, color, highRoot, approach);
    }
    bass_min_density(s.steps, c.prog, 4, "1");
    bass_max_density(s.steps, 6);
    ensure_gate_variation(s.steps);
    s.evo = bass_evolution(c.bpm);
    s.sweep = "medium";
    s.seed = c.seed;
    return s;
}

/* ---- arpeggio ---- */
static inline vector<string> arp_pool(const vector<string> &n, const vector<string> &sn, int vs) {
    string ext = sn[6];
    if (vs % 2 == 1) ext = sn[1];
    string e = arp_clamp(ext + "5");
    switch (vs % 4) {
    case 0: return {arp_clamp(n[0] + "3"), arp_clamp(n[2] + "4"), arp_clamp(n[1] + "5"), e};
    case 1: return {arp_clamp(n[1] + "3"), arp_clamp(n[2] + "4"), arp_clamp(n[0] + "5"), e};
    case 2: return {arp_clamp(n[2] + "3"), arp_clamp(n[0] + "4"), arp_clamp(n[1] + "5"), e};
    default: return {arp_clamp(n[0] + "3"), arp_clamp(n[1] + "4"), arp_clamp(n[2] + "4"), e};
    }
}
static inline void fill_arp_sub(vector<Step> &st, int b, int pat, const vector<string> &p) {
    switch (pat % 6) {
    case 0: st[b] = N(p[0], F_ACC); st[b + 1] = N(p[1], F_STAC); st[b + 2] = N(p[2]); st[b + 3] = N(p[3], F_STAC); break;
    case 1: st[b] = N(p[2], F_ACC); st[b + 1] = N(p[1]); st[b + 2] = N(p[0], F_STAC); st[b + 3] = N(p[1]); break;
    case 2: st[b] = N(p[0], F_ACC); st[b + 1] = N(p[2], F_STAC); st[b + 2] = N(p[1]); st[b + 3] = N(p[0], F_STAC); break;
    case 3: st[b] = N(p[0], F_ACC); st[b + 1] = N(p[2], F_STAC); st[b + 2] = N(p[0]); st[b + 3] = N(p[2], F_ACC); break;
    case 4: st[b] = N(p[0], F_ACC); st[b + 1] = N(p[2], F_STAC); st[b + 2] = N(p[0]); st[b + 3] = N(p[3], F_LEG); break;
    default: st[b] = N(p[0], F_ACC); st[b + 1] = N(p[1], F_STAC); st[b + 2] = N(p[2]); st[b + 3] = N(p[1], F_STAC); break;
    }
}
static inline int choose_arp_pattern(const H &h, const Key &k, int section) {
    uint8_t bias = (uint8_t)(character_degree(k) + (int)k.root.size());
    return (uint8_t)(hb(h, section + 4) + bias + (uint8_t)section) % 6;
}
static inline int ensure_distinct(int p, std::initializer_list<int> used, int maxv) {
    for (;;) {
        bool f = false;
        for (int u : used) if (p == u) { f = true; break; }
        if (!f) return p;
        p = (p + 1) % maxv;
    }
}
static inline void apply_arp_style(const string &style, vector<Step> &st, const H &h) {
    if (style == "hypnotic") {
        for (auto &s : st) if (s.active) { s.legato = true; s.staccato = false; }
        for (int i = 0; i < 4; i++) if (st[i * 16].active) st[i * 16].accent = true;
    } else if (style == "driving") {
        for (size_t i = 0; i < st.size(); i++) {
            if (!st[i].active) continue;
            if ((i / 2) % 2 == 0) { st[i].legato = true; st[i].staccato = false; }
            else { st[i].staccato = true; st[i].legato = false; }
        }
    } else if (style == "minimal") {
        for (auto &s : st) if (s.active) { s.staccato = true; s.legato = false; s.accent = false; s.ghost = false; }
        for (int i = 0; i < 4; i++) if (st[i * 16].active) { st[i * 16].legato = true; st[i * 16].staccato = false; }
    } else {
        mix_gate_articulation(st, h);
    }
}
static inline string choose_arp_profile(const Key &k, double bpm, const H &h) {
    if (bpm < 116) return "arp_flowing";
    if (bpm >= 130) { const char *c[2] = {"arp_staccato", "arp_epic"}; return c[hb(h, 1) % 2]; }
    if (mode_minor(k)) {
        if (bpm >= 126) { const char *c[2] = {"arp_epic", "arp_staccato"}; return c[hb(h, 1) % 2]; }
        const char *c[2] = {"arp_flowing", "arp_epic"};
        return c[hb(h, 1) % 2];
    }
    const char *c[2] = {"arp_flowing", "arp_epic"};
    return c[hb(h, 1) % 2];
}
static inline Spec arpeggio(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "arp");
    Spec s;
    s.pattern_type = "arpeggio";
    s.profile = choose_arp_profile(k, c.bpm, h);
    string style = family_offline_style(c.family, c.offline_style);
    s.steps.assign(MOTIF, Step());
    int vs = hb(h, 5) % 4;
    vector<string> sn = scale_or_c(k);
    for (int i = 0; i < 4; i++) {
        const PChord &ch = c.prog.c[i];
        vector<string> n = chord_notes(ch.root, ch.quality);
        if (n.size() < 3) n = {ch.root, ch.root, ch.root};
        vector<string> pool = arp_pool(n, sn, vs);
        int base = i * 16;
        int p0 = choose_arp_pattern(h, k, i * 4 + 0), p1 = choose_arp_pattern(h, k, i * 4 + 1),
            p2 = choose_arp_pattern(h, k, i * 4 + 2), p3 = choose_arp_pattern(h, k, i * 4 + 3);
        p1 = ensure_distinct(p1, {p0}, 6);
        p2 = ensure_distinct(p2, {p0, p1}, 6);
        p3 = ensure_distinct(p3, {p0, p1, p2}, 6);
        fill_arp_sub(s.steps, base + 0, p0, pool);
        fill_arp_sub(s.steps, base + 4, p1, pool);
        fill_arp_sub(s.steps, base + 8, p2, pool);
        fill_arp_sub(s.steps, base + 12, p3, pool);
    }
    apply_arp_style(style, s.steps, h);
    ensure_gate_variation(s.steps);
    s.sweep = (c.bpm >= 124 || mode_minor(k)) ? "dramatic" : "medium";
    s.evo = {{1, 1, "build", 0.7}, {2, 2, "peak", 0.9}, {3, 3, "peak", 1.0}, {4, 4, "release", 0.7}};
    s.seed = c.seed;
    return s;
}

/* ---- melody (contour phrase builder) ---- */
enum Contour { C_ARCH, C_QA, C_TENSION, C_DESC };
enum Role { R_NONE, R_TARGET, R_PICKUP, R_ECHO, R_FILL };
static const uint8_t RHY[4][3][16] = {
    {{1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 3, 0, 2}, {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 3, 0, 0}, {1, 2, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 3, 0}},
    {{1, 2, 0, 1, 0, 0, 3, 0, 0, 1, 0, 1, 0, 3, 0, 2}, {1, 0, 2, 1, 0, 3, 0, 0, 0, 1, 2, 0, 1, 0, 3, 0}, {2, 1, 0, 0, 1, 0, 3, 0, 0, 0, 1, 0, 2, 1, 0, 0}},
    {{1, 0, 1, 0, 1, 2, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 2, 0, 1, 0, 1, 0}, {1, 2, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0}},
    {{1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 0, 3, 0, 0, 0}, {1, 0, 1, 0, 0, 1, 0, 0, 2, 0, 0, 1, 0, 3, 0, 0}, {1, 0, 0, 0, 1, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 3}}};
static const int HEIGHTS[4][16] = {
    {0, 0, 1, 1, 1, 0, 0, 1, 0, 0, -1, -1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 1, 1, 0, 0, -1, -1, 0, 0, 0, 0},
    {1, 0, 0, 0, 0, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0}};
struct Intent { bool active; int role; bool prefer_high; int height; };
struct Section { int type; int target_degree; Intent it[16]; };

static inline vector<int> hypnotic_motif(const H &h, int len, int n) {
    vector<int> m(len);
    static const int START[3] = {0, 3, 4}, WIDE[6] = {3, 4, 5, -3, -4, -2};
    m[0] = START[hb(h, 14) % 3];
    for (int i = 1; i < len; i++) {
        int iv = WIDE[hb(h, 15 + i) % 6];
        int next = (m[i - 1] + iv) % n;
        if (next < 0) next += n;
        if (i > 1) {
            int prevDiff = m[i - 1] - m[i - 2], stored = next - m[i - 1];
            bool same = (prevDiff > 0 && stored > 0) || (prevDiff < 0 && stored < 0);
            if (same && std::abs(prevDiff) >= 3 && std::abs(stored) >= 3) {
                int alt = (m[i - 1] - iv) % n;
                if (alt < 0) alt += n;
                int ad = alt - m[i - 1];
                bool altSame = (prevDiff > 0 && ad > 0) || (prevDiff < 0 && ad < 0);
                if (!altSame || std::abs(ad) < 3) next = alt;
            }
        }
        m[i] = next;
    }
    return m;
}
static inline void contour_articulation(Step &s, int ct, int pos) {
    switch (ct) {
    case C_ARCH:
        if (pos >= 7 && pos <= 9) { s.accent = true; s.legato = true; }
        else if (pos > 9) s.staccato = true;
        break;
    case C_QA:
        if (pos <= 6) s.staccato = true;
        else { s.accent = true; s.legato = true; }
        break;
    case C_TENSION:
        if (pos < 8) s.legato = true;
        else s.accent = true;
        break;
    case C_DESC:
        if (pos == 0) { s.accent = true; s.legato = true; }
        else s.staccato = true;
        break;
    }
}
static inline void fill_melody_section(vector<Step> &st, int base, const Section &sec, const vector<string> &ct,
                                       const vector<string> &sn, const Key &k, const H &h) {
    string target = closest_chord_tone(ct, sn, sec.target_degree);
    int prev = -1, debt = 0, n = (int)sn.size();
    for (int step = 0; step < 16; step++) {
        const Intent &in = sec.it[step];
        if (!in.active) continue;
        string name;
        switch (in.role) {
        case R_PICKUP: name = approach_note(target, k); break;
        case R_ECHO: {
            int d = normalize_degree(sec.target_degree + 1, n);
            if (in.height < 0) d = normalize_degree(sec.target_degree - 1, n);
            else if (in.height == 0 && hb(h, (base + step) % 32) % 2 == 0) d = normalize_degree(sec.target_degree - 1, n);
            name = n > 0 ? sn[d] : target;
            break;
        }
        case R_FILL: {
            int off = in.height < 0 ? -2 : 2;
            name = n > 0 ? sn[normalize_degree(sec.target_degree + off, n)] : target;
            break;
        }
        default: name = target;
        }
        bool high = in.prefer_high;
        if (in.height > 0) high = true;
        else if (in.height < 0) high = false;
        if (debt != 0 && in.role == R_TARGET) { high = debt > 0; debt = 0; }
        string note = melody_note(name, "5", high);
        Step s = N(note);
        switch (in.role) {
        case R_PICKUP: s.ghost = true; s.staccato = true; break;
        case R_ECHO: s.ghost = true; break;
        case R_FILL: s.staccato = true; break;
        default: contour_articulation(s, sec.type, step);
        }
        st[base + step] = s;
        int cur = note_to_midi(note);
        if (cur >= 0) {
            if (prev >= 0) {
                int leap = cur - prev;
                debt = leap > 5 ? -1 : leap < -5 ? 1 : 0;
            }
            prev = cur;
        }
    }
}
static inline void apply_melody_style(const string &style, vector<Step> &st, const Prog &p, const H &h) {
    if (style == "hypnotic") {
        for (auto &s : st)
            if (s.active) {
                if (s.ghost) s.active = false;
                else { s.legato = true; s.staccato = false; }
            }
    } else if (style == "driving") {
        for (auto &s : st) if (s.active && !s.accent && !s.legato) s.staccato = true;
    } else if (style == "minimal") {
        for (int sec = 0; sec < 4; sec++) {
            int base = sec * 16;
            for (int i = 0; i < 16; i++) st[base + i].active = false;
            vector<string> n = chord_notes_or_root(p.c[sec]);
            int nn = (int)n.size();
            st[base + 0] = N(melody_note(n[0], "5", false), F_ACC | F_LEG);
            st[base + 4] = N(melody_note(n[1 % nn], "5", false), F_STAC);
            st[base + 8] = N(melody_note(n[2 % nn], "5", false), F_LEG);
            st[base + 12] = N(melody_note(n[0], "5", false), F_GHOST);
        }
    } else {
        mix_gate_articulation(st, h);
    }
}
static inline string choose_melody_profile(const Key &k, double bpm, const H &h) {
    if (bpm < 118) return "melody_expressive";
    if (mode_minor(k)) return "melody_hypnotic";
    if (bpm >= 124) { const char *c[2] = {"melody_hypnotic", "melody_expressive"}; return c[hb(h, 2) % 2]; }
    return "melody_expressive";
}
static inline Spec melody(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "melody");
    Spec s;
    s.pattern_type = "melody";
    s.profile = choose_melody_profile(k, c.bpm, h);
    string style = family_offline_style(c.family, c.offline_style);
    vector<string> sn = scale_or_c(k);
    s.steps.assign(MOTIF, Step());
    int mlen = 3 + hb(h, 6) % 3;
    vector<int> motif = hypnotic_motif(h, mlen, (int)sn.size());
    static const int OPTS[4][2] = {{C_ARCH, C_QA}, {C_QA, C_ARCH}, {C_TENSION, C_ARCH}, {C_DESC, C_QA}};
    for (int i = 0; i < 4; i++) {
        Section sec;
        int bias = hb(h, (i * 7 + 3) % 32) % 2;
        sec.type = OPTS[i][bias];
        int variant = hb(h, (i * 11 + 17) % 32) % 3;
        bool preferHigh = i != 3;
        sec.target_degree = motif[i % mlen];
        for (int st = 0; st < 16; st++) {
            int code = RHY[sec.type][variant][st];
            bool act = code >= 1 && code <= 4;
            int role = code == 1 ? R_TARGET : code == 2 ? R_PICKUP : code == 3 ? R_ECHO : code == 4 ? R_FILL : R_NONE;
            sec.it[st] = {act, role, act && preferHigh, HEIGHTS[sec.type][st]};
        }
        const PChord &ch = c.prog.c[i];
        vector<string> ct = chord_notes(ch.root, ch.quality);
        if (ct.empty()) ct = {ch.root};
        fill_melody_section(s.steps, i * 16, sec, ct, sn, k, h);
    }
    apply_melody_style(style, s.steps, c.prog, h);
    if (style != "hypnotic" && style != "minimal") apply_passing_notes("melody", s.steps, c.prog, k, h);
    for (int i = 0; i < 4; i++) {   /* ensureMelodyMinDensity */
        Step *sl = &s.steps[i * 16];
        if (active_count(sl, 16) >= 4) continue;
        vector<string> n = chord_notes_or_root(c.prog.c[i]);
        sl[0] = N(n[0] + "5", (i == 2 ? F_ACC : 0) | F_LEG);
    }
    ensure_gate_variation(s.steps);
    s.evo = {{1, 1, "introduce", 0.4}, {2, 2, "build", 0.6}, {3, 3, "peak", 0.8}, {4, 4, "release", 0.4}};
    s.sweep = "";   /* melody: mod-wheel intent only, which the renderer does not output */
    s.seed = c.seed;
    return s;
}

/* ---- chord pad + lead stab (offline_pad_lead.go) ---- */
static inline string pad_note(const string &name, const string &oct) {
    string full = name + oct;
    int m = note_to_midi(full);
    if (m < 0) return full;
    if (m < 48 || m > 79) return name + "4";
    return full;
}
static inline Spec chord_pad(const Ctx &c) {
    const Key &k = c.key;
    H h = seed_hash(c.seed + k.root + k.scale + "chord_pad");
    Spec s;
    s.pattern_type = "chord_pad";
    s.steps.assign(MOTIF, Step());
    for (int i = 0; i < 4; i++) {
        const PChord &ch = c.prog.c[i];
        int base = i * 16;
        vector<string> n = chord_notes(ch.root, ch.quality);
        if (n.size() < 3) n = {ch.root, ch.root, ch.root};
        if (c.family == "rolling") {
            s.steps[base + 0] = N(pad_note(n[0], "3"), F_ACC | F_LEG);
            s.steps[base + 8] = N(pad_note(n[2], "4"), F_LEG);
        } else if (c.family == "sub") {
            s.steps[base + 0] = N(pad_note(n[0], "2"), F_ACC | F_LEG);
            s.steps[base + 4] = N(pad_note(n[2], "3"), F_LEG);
            s.steps[base + 6] = N(pad_note(n[1], "3"), F_GHOST);
            s.steps[base + 8] = N(pad_note(n[0], "2"), F_LEG);
            s.steps[base + 12] = N(pad_note(n[2], "3"), F_LEG);
            if (i == 2 && hb(h, (base + 14) % 32) % 2 == 0) s.steps[base + 14] = N(pad_note(n[0], "3"), F_GHOST);
        } else {
            s.steps[base + 0] = N(pad_note(n[0], "3"), F_ACC | F_LEG);
            s.steps[base + 4] = N(pad_note(n[2], "4"), F_LEG);
            s.steps[base + 8] = N(pad_note(n[1], "5"), F_LEG);
            s.steps[base + 12] = N(pad_note(n[0], "3"), F_GHOST);
        }
    }
    ensure_gate_variation(s.steps);
    s.profile = c.family == "rolling" ? "chord_pad_rolling" : c.family == "sub" ? "chord_pad_sub" : "chord_pad_groove";
    s.sweep = c.family == "sub" ? "medium" : "subtle";
    s.evo = {{1, 1, "introduce", 0.4}, {2, 2, "build", 0.6}, {3, 3, "peak", 0.8}, {4, 4, "release", 0.4}};
    s.seed = c.seed;
    return s;
}
/* the voicing a pad hit belongs to (used by the plugin's CHORD mode; not part of Cadenza) */
static inline vector<int> pad_voicing(const PChord &ch, const string &family) {
    vector<string> n = chord_notes(ch.root, ch.quality);
    if (n.size() < 3) n = {ch.root, ch.root, ch.root};
    vector<string> v;
    if (family == "rolling") v = {pad_note(n[0], "3"), pad_note(n[2], "4")};
    else if (family == "sub") v = {pad_note(n[0], "2"), pad_note(n[2], "3"), pad_note(n[1], "3")};
    else v = {pad_note(n[0], "3"), pad_note(n[2], "4"), pad_note(n[1], "5")};
    vector<int> out;
    for (auto &x : v) { int m = note_to_midi(x); if (m >= 0) out.push_back(m); }
    return out;
}
static inline Spec lead_stab(const Ctx &c) {
    Ctx mc = c;
    mc.seed = c.seed + "_lead";
    Spec mel = melody(mc);
    Spec s;
    s.pattern_type = "lead_stab";
    s.profile = "lead_stab";
    s.steps = mel.steps;
    for (auto &x : s.steps) if (x.active) { x.ghost = false; x.legato = false; x.staccato = true; }
    if (c.family == "sub") {
        for (int i = 0; i < 4; i++) {
            Step *sl = &s.steps[i * 16];
            bool kept = false;
            for (int j = 0; j < 16; j++) {
                if (sl[j].active && sl[j].accent && !kept) { kept = true; continue; }
                if (sl[j].active && !kept) { kept = true; continue; }
                sl[j].active = false;
            }
        }
    }
    if (c.family == "rolling")
        s.evo = {{1, 1, "build", 0.6}, {2, 2, "peak", 0.9}, {3, 3, "peak", 1.0}, {4, 4, "release", 0.5}};
    else if (c.family == "sub")
        s.evo = {{1, 1, "introduce", 0.2}, {2, 2, "build", 0.3}, {3, 3, "peak", 0.4}, {4, 4, "release", 0.2}};
    else
        s.evo = {{1, 1, "introduce", 0.4}, {2, 2, "build", 0.6}, {3, 3, "peak", 0.8}, {4, 4, "release", 0.4}};
    s.sweep = "subtle";
    s.seed = c.seed;
    return s;
}

/* ---------------------------------------------------------------------------
 * style profiles (renderer/styleprofile), only the fields the renderer reads
 * ------------------------------------------------------------------------- */
struct SweepRange { const char *style; int lo, hi; };
struct Profile {
    const char *name, *pattern_type;
    bool downbeat_rigid; int offsets[16]; int noffsets;
    int grid[16]; int ghost_vel, accent_boost, max_vel; const char *curve;
    double normal, accent, ghost, slide, staccato, legato;
    int cc5; bool cc65;
    const char *sweep_curve; int sweep_res, jitter; bool kickback; SweepRange ranges[3]; int nranges;
};
static const Profile PROFILES[] = {
    {"bass_progressive", "bassline", true, {0, 2, -1, 3, 0, 2, -1, 4, 0, 2, -1, 3, 0, 2, -1, 5}, 16,
     {118, 75, 85, 70, 110, 75, 88, 72, 115, 78, 85, 70, 108, 75, 90, 80}, 42, 8, 120, "crescendo",
     0.72, 0.88, 0.28, 1.05, 0, 0, 35, true, "s_curve", 16, 2, true, {{"subtle", 55, 85}, {"medium", 40, 100}, {"dramatic", 25, 115}}, 3},
    {"bass_techno_driving", "bassline", true, {0, 3, -2, 4, 0, 3, -1, 5, 0, 2, -2, 4, 0, 3, -1, 6}, 16,
     {120, 82, 95, 78, 115, 80, 92, 75, 118, 82, 95, 78, 112, 80, 90, 82}, 38, 10, 120, "plateau",
     0.60, 0.78, 0.22, 1.10, 0, 0, 45, true, "exponential", 16, 3, true, {{"subtle", 50, 80}, {"medium", 35, 100}, {"dramatic", 20, 115}}, 3},
    {"bass_sub", "bassline", true, {0}, 16,
     {90, 85, 88, 82, 90, 85, 88, 82, 90, 85, 88, 82, 90, 85, 88, 82}, 40, 3, 95, "flat",
     0.95, 0.98, 0.30, 1.00, 0, 0, 0, false, "flat", 4, 0, false, {{"subtle", 60, 70}, {"medium", 55, 75}, {"dramatic", 50, 80}}, 3},
    {"bass_rolling", "bassline", true, {0, 1, -1, 2, 0, 1, -1, 2, 0, 1, -1, 2, 0, 1, -1, 2}, 16,
     {112, 78, 82, 75, 108, 80, 84, 76, 112, 78, 82, 75, 108, 80, 84, 76}, 44, 10, 115, "crescendo",
     0.65, 0.80, 0.22, 1.05, 0.18, 0.90, 50, true, "exponential", 16, 3, true, {{"subtle", 45, 90}, {"medium", 30, 105}, {"dramatic", 20, 118}}, 3},
    {"arp_flowing", "arpeggio", false, {0, 1, -1, 2, 0, 1, -1, 2, 0, 1, -1, 2, 0, 1, -1, 2}, 16,
     {95, 88, 92, 85, 98, 90, 93, 87, 96, 89, 91, 86, 94, 88, 90, 85}, 60, 5, 110, "arch",
     0.48, 0.60, 0.25, 0, 0.28, 0.78, 0, false, "exponential", 32, 1, false, {{"subtle", 65, 90}, {"medium", 50, 105}, {"dramatic", 35, 115}}, 3},
    {"arp_epic", "arpeggio", true, {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0}, 16,
     {100, 75, 80, 72, 95, 72, 78, 70, 98, 74, 82, 73, 100, 76, 85, 74}, 55, 8, 115, "tension",
     0.55, 0.65, 0.35, 0, 0.28, 0.82, 0, false, "s_curve", 32, 1, true, {{"subtle", 50, 95}, {"medium", 30, 110}, {"dramatic", 20, 120}}, 3},
    {"arp_staccato", "arpeggio", true, {0, -1, 1, -1, 0, 1, -1, 1, 0, -1, 1, 0, 0, 1, -1, 1}, 16,
     {95, 90, 92, 88, 95, 88, 90, 85, 95, 90, 92, 88, 95, 88, 90, 85}, 50, 5, 110, "flat",
     0.20, 0.25, 0.15, 0, 0.12, 0.30, 0, false, "linear", 16, 2, false, {{"subtle", 70, 100}, {"medium", 60, 110}, {"dramatic", 50, 120}}, 3},
    {"melody_expressive", "melody", false, {0, -2, 3, -1, 0, 2, -3, 1, 0, -2, 3, 0, 2, -1, 3, -2}, 16,
     {105, 92, 88, 95, 110, 85, 90, 98, 108, 93, 87, 100, 105, 90, 95, 88}, 50, 10, 120, "crescendo",
     0.65, 0.85, 0.30, 0, 0, 0.98, 0, false, "", 0, 0, false, {}, 0},
    {"melody_hypnotic", "melody", true, {0, 1, -1, 0, 0, 2, -1, 0, 0, 1, 0, -1, 0, 1, -2, 0}, 16,
     {100, 88, 85, 88, 95, 85, 88, 85, 98, 88, 85, 88, 95, 85, 88, 85}, 52, 5, 110, "crescendo",
     0.90, 0.95, 0.45, 0, 0, 0.98, 0, false, "", 0, 0, false, {}, 0},
    {"chord_pad_groove", "chord_pad", true, {0}, 16,
     {78, 60, 62, 58, 72, 60, 62, 58, 78, 60, 62, 58, 72, 60, 62, 58}, 38, 5, 85, "flat",
     0.92, 0.95, 0.50, 1.00, 0, 0.98, 0, false, "flat", 4, 0, false, {{"subtle", 70, 85}, {"medium", 60, 90}}, 2},
    {"chord_pad_rolling", "chord_pad", true, {0}, 0,
     {60, 45, 45, 45, 45, 45, 45, 45, 60, 45, 45, 45, 45, 45, 45, 45}, 32, 3, 65, "flat",
     0.96, 0.98, 0.60, 0, 0, 0.99, 0, false, "flat", 4, 0, false, {{"subtle", 72, 82}}, 1},
    {"chord_pad_sub", "chord_pad", true, {0}, 0,
     {78, 62, 65, 60, 75, 62, 65, 60, 78, 62, 65, 60, 75, 62, 65, 60}, 40, 5, 85, "flat",
     0.97, 0.99, 0.55, 0, 0, 1.00, 0, false, "s_curve", 8, 0, false, {{"subtle", 60, 80}, {"medium", 50, 90}}, 2},
    {"lead_stab", "lead_stab", true, {0, 1, -1, 2, 0, 1, -1, 3, 0, 1, -1, 2, 0, 1, -1, 3}, 16,
     {100, 72, 78, 68, 95, 70, 75, 65, 102, 72, 78, 68, 95, 70, 75, 65}, 0, 8, 110, "crescendo",
     0.20, 0.25, 0.12, 0.25, 0.15, 0, 0, false, "flat", 4, 0, false, {{"subtle", 65, 85}}, 1},
};
static inline const Profile *profile_by_name(const char *n) {
    for (auto &p : PROFILES) if (string(p.name) == n) return &p;
    return nullptr;
}
/* renderAndSaveNamed: LoadForType(stem, spec.StyleProfile) (name and pattern type must match),
 * else DefaultForTypeExtended(stem). E.g. "bass_driving" is no registered name -> bass_progressive. */
static inline const Profile *resolve_profile(const string &stem, const string &requested) {
    for (auto &p : PROFILES)
        if (requested == p.name && stem == p.pattern_type) return &p;
    const char *def = stem == "bassline" ? "bass_progressive" : stem == "bassline_rolling" ? "bass_rolling"
                    : stem == "bassline_sub" ? "bass_sub" : stem == "arpeggio" ? "arp_flowing" : stem == "melody" ? "melody_expressive"
                    : stem == "chord_pad" ? "chord_pad_groove" : "lead_stab";
    return profile_by_name(def);
}

/* ---------------------------------------------------------------------------
 * renderer (renderer.go, evolution.go, velocity.go, timing.go, gate.go, portamento.go, sweep.go)
 * ------------------------------------------------------------------------- */
static const int TPS = 120;   /* ticks per 16th at 480 per quarter */
enum { EV_ON = 0, EV_OFF = 1, EV_CC = 2 };
struct Ev { int type; long tick; int a, b; };            /* on: note vel; off: note; cc: controller value */
struct RNote { long on, off; int note, vel, step; bool slide; };
struct Render {
    const Profile *profile = nullptr;
    vector<Ev> ev;          /* generation order (= the Go renderer's) */
    vector<RNote> notes;    /* step = 0..255 over the 4 passes */
};

static inline string shift_octave(const string &n, int d) {
    if (n.size() < 2) return n;
    char l = n.back();
    if (l < '0' || l > '9') return n;
    int o = (l - '0') + d;
    if (o < 0 || o > 9) return n;
    return n.substr(0, n.size() - 1) + (char)('0' + o);
}
static inline void deactivate_steps(vector<Step> &s, int count) {
    if (count <= 0) return;
    int d = 0, n = (int)s.size();
    for (int i = n - 1; i >= 0 && d < count; i -= 2) { if (i == 0) continue; if (s[i].active) { s[i].active = false; d++; } }
    for (int i = n - 2; i >= 0 && d < count; i -= 2) { if (i == 0) continue; if (s[i].active) { s[i].active = false; d++; } }
}
static inline void activate_steps(vector<Step> &s, int count) {
    if (count <= 0) return;
    int a = 0, n = (int)s.size();
    for (int i = 0; i < n && a < count; i += 2) if (!s[i].active) { s[i].active = true; a++; }
    for (int i = 1; i < n && a < count; i += 2) if (!s[i].active) { s[i].active = true; a++; }
}
static inline void octave_shift(vector<Step> &s, int delta, double frac) {
    vector<int> act;
    for (size_t i = 0; i < s.size(); i++) if (s[i].active && !s[i].note.empty()) act.push_back((int)i);
    int count = (int)((double)act.size() * frac);
    if (count < 1) count = 1;
    for (int i = 0; i < count && i < (int)act.size(); i++) s[act[i]].note = shift_octave(s[act[i]].note, delta);
}
static inline vector<Step> bar_steps(const vector<Step> &steps, int bar, const vector<Evo> &evo) {
    vector<Step> r = steps;
    int barNum = bar + 1;
    for (auto &e : evo) {
        if (barNum < e.from || barNum > e.to) continue;
        string a = e.action;
        if (a == "octave_up") octave_shift(r, 1, e.intensity);
        else if (a == "octave_down") octave_shift(r, -1, e.intensity);
        else if (a == "introduce") {
            int cur = active_count(r);
            int t = (int)((double)cur * 0.6);
            if (t < 4) t = 4;
            if (t > cur) t = cur;
            deactivate_steps(r, cur - t);
        } else if (a == "build") {
            int count = (int)(e.intensity * 3);
            int added = 0;
            for (size_t i = 1; i < r.size() && added < count; i += 2)
                if (!r[i].active && !r[i].note.empty()) { r[i].active = true; r[i].ghost = true; added++; }
        } else if (a == "peak") {
            activate_steps(r, 2);
            octave_shift(r, 1, 0.3);
        } else if (a == "release") {
            for (size_t i = 0; i < r.size(); i++) if (r[i].ghost && i % 2 == 1) r[i].active = false;
            int t = (int)((double)active_count(r) * 0.8);
            deactivate_steps(r, active_count(r) - t);
        }
    }
    /* mutateMotif for bar > 0 && bar % 4 == 0: never with Cadenza's 4 passes */
    return r;
}
static inline double evo_vel_scale(int bar, const vector<Evo> &evo) {
    int barNum = bar + 1;
    for (auto &e : evo) {
        if (barNum < e.from || barNum > e.to) continue;
        string a = e.action;
        if (a == "introduce") return 0.8 * e.intensity / 0.3;
        if (a == "build") return 1.0;
        if (a == "peak") return 1.0 + 0.1 * e.intensity / 0.8;
        if (a == "release") return 0.9;
    }
    return 1.0;
}
static inline double evo_intensity(int bar, const vector<Evo> &evo) {
    int barNum = bar + 1;
    for (auto &e : evo) if (barNum >= e.from && barNum <= e.to) return e.intensity;
    return 0.5;
}
static inline double dynamic_curve(int bar, int total, const Profile *p) {
    string c = p->curve;
    double b = bar, t = total;
    if (c == "crescendo") return 0.7 + 0.3 * b / (double)(total - 1);
    if (c == "arch") {
        double mid = t * 0.75;
        if (b < mid) return 0.75 + 0.25 * b / mid;
        return 1.0 - 0.15 * (b - mid) / (t - mid);
    }
    if (c == "plateau") {
        double re = t * 0.375, pe = t * 0.75;
        if (b < re) return 0.7 + 0.3 * b / re;
        if (b < pe) return 1.0;
        return 1.0 - 0.15 * (b - pe) / (t - pe);
    }
    if (c == "tension") {
        double q = t / 4.0;
        if (b < q) return 0.6 + 0.2 * b / q;
        if (b < 2 * q) return 0.8 + 0.3 * (b - q) / q;
        if (b < 3 * q) return 1.1 - 0.4 * (b - 2 * q) / q;
        return 0.7;
    }
    return 1.0;
}
static inline long resolve_tick(int bar, int idx, int spb, const Profile *p) {
    long base = (long)(bar * spb + idx) * TPS;
    if (idx == 0 && p->downbeat_rigid) return base;
    if (p->noffsets == 0) return base;
    long t = base + p->offsets[idx % p->noffsets];
    return t < 0 ? 0 : t;
}
static inline int resolve_velocity(int idx, bool accent, bool ghost, const Profile *p, double scale) {
    if (ghost) {
        int v = (int)((double)p->ghost_vel * scale);
        if (v < 1) v = 1;
        if (v > 60) v = 60;
        return v;
    }
    int v = p->grid[idx % 16];
    if (accent) v += p->accent_boost;
    v = (int)((double)v * scale);
    if (v > p->max_vel) v = p->max_vel;
    if (v < 1) v = 1;
    return v;
}
static inline long gate_ticks(const Step &s, const Profile *p, long tick, long next) {
    if (s.slide && next > tick && next > 0) return next - tick + 5;
    double r;
    if (s.slide && p->slide > 0) r = p->slide;
    else if (s.legato && p->legato > 0) r = p->legato;
    else if (s.staccato && p->staccato > 0) r = p->staccato;
    else if (s.ghost) r = p->ghost;
    else if (s.accent) r = p->accent;
    else r = p->normal;
    if (r <= 0) r = 0.72;
    return (long)((double)TPS * r);
}
static inline double apply_curve(double x, const char *c) {
    string s = c;
    if (s == "s_curve") return x * x * (3.0 - 2.0 * x);
    if (s == "exponential") return x * x;   /* math.Pow(x, 2) is exact here */
    return x;
}
static inline void sweep_events(Render &R, int bar, int total, const string &style, const string &seed, const string &pt,
                                const Profile *p, const vector<Evo> &evo, int spb) {
    if (p->sweep_res == 0) return;
    int lo = 0, hi = 0;
    bool found = false;
    for (int i = 0; i < p->nranges; i++) if (style == p->ranges[i].style) { lo = p->ranges[i].lo; hi = p->ranges[i].hi; found = true; }
    if (!found) for (int i = 0; i < p->nranges; i++) if (string("medium") == p->ranges[i].style) { lo = p->ranges[i].lo; hi = p->ranges[i].hi; }
    double bmin = lo, bmax = hi;
    double inten = evo_intensity(bar, evo);
    double spread = (bmax - bmin) * inten;
    double minv = bmax - spread, maxv = bmax;
    int res = p->sweep_res, jit = p->jitter, cycle = 4;
    double phase = pt == "arpeggio" ? 0.25 : pt == "melody" ? 0.5 : 0.0;
    long barStart = (long)bar * spb * TPS;
    long stepTicks = (long)(spb * TPS) / res;
    for (int step = 0; step < res; step++) {
        int cb = bar % cycle;
        double base = ((double)(cb * res + step) + phase * (double)(cycle * res)) / (double)(cycle * res);
        double prog = std::fmod(base, 1.0);
        if (prog < 0) prog += 1.0;
        int cycleCount = bar / cycle;
        double v;
        if (style == "dramatic") {
            if (cycleCount % 2 == 0) {
                if (prog < 0.5) v = minv + (maxv - minv) * apply_curve(prog * 2, p->sweep_curve);
                else v = maxv - (maxv - minv) * apply_curve((prog - 0.5) * 2, p->sweep_curve);
            } else v = minv + (maxv - minv) * apply_curve(prog, p->sweep_curve);
        } else v = minv + (maxv - minv) * apply_curve(prog, p->sweep_curve);
        if (jit > 0) {
            uint32_t hh = fnv1a32(seed + ":" + std::to_string(bar) + ":" + std::to_string(step));
            int j = (int)(hh % (uint32_t)(jit * 2 + 1)) - jit;
            v += (double)j;
        }
        v = std::max(0.0, std::min(127.0, v));
        if (p->kickback && bar == total - 1 && step == res - 1) { v = maxv - 10; if (v > 127) v = 127; }
        R.ev.push_back({EV_CC, barStart + (long)step * stepTicks, 74, (int)std::round(v)});
    }
}
/* Render(): 4 passes of the 64-step motif (16 bars) like Cadenza (Meta.Bars = 4) */
static inline Render render(const Spec &s, const Profile *p, int passes = 4) {
    Render R;
    R.profile = p;
    if (p->cc65) R.ev.push_back({EV_CC, 0, 5, p->cc5});
    int spb = (int)s.steps.size();
    for (int bar = 0; bar < passes; bar++) {
        vector<Step> bs = bar_steps(s.steps, bar, s.evo);
        double vs = evo_vel_scale(bar, s.evo);
        vs *= dynamic_curve(bar, passes, p);
        if (!s.sweep.empty()) sweep_events(R, bar, passes, s.sweep, s.seed, s.pattern_type, p, s.evo, spb);
        for (int i = 0; i < spb; i++) {
            const Step &st = bs[i];
            if (!st.active || st.note.empty()) continue;
            int m = note_to_midi(st.note);
            if (m < 0) continue;
            long tick = resolve_tick(bar, i, spb, p);
            int vel = resolve_velocity(i, st.accent, st.ghost, p, vs);
            long next = 0;
            if (st.slide) {
                next = resolve_tick(bar + 1, 0, spb, p);
                for (int j = i + 1; j < spb; j++) if (bs[j].active) { next = resolve_tick(bar, j, spb, p); break; }
            }
            long gate = gate_ticks(st, p, tick, next);
            if (p->cc65 && tick >= 10) R.ev.push_back({EV_CC, tick - 5, 65, st.slide ? 127 : 0});
            R.ev.push_back({EV_ON, tick, m, vel});
            R.ev.push_back({EV_OFF, tick + gate, m, 0});
            R.notes.push_back({tick, tick + gate, m, vel, bar * spb + i, st.slide});
        }
    }
    return R;
}

/* ---------------------------------------------------------------------------
 * the 7 stems
 * ------------------------------------------------------------------------- */
static const int NSTEMS = 7;
static const char *const STEM_TYPE[NSTEMS] = {"bassline", "bassline_rolling", "bassline_sub", "arpeggio", "melody", "chord_pad", "lead_stab"};
static inline Spec make_stem(int i, const Ctx &c) {
    switch (i) {
    case 0: return bassline_groove(c);
    case 1: return bassline_rolling(c);
    case 2: return bassline_sub(c);
    case 3: return arpeggio(c);
    case 4: return melody(c);
    case 5: return chord_pad(c);
    default: return lead_stab(c);
    }
}
static inline Render render_stem(int i, const Ctx &c, Spec *spec_out = nullptr) {
    Spec s = make_stem(i, c);
    const Profile *p = resolve_profile(STEM_TYPE[i], s.profile);
    Render r = render(s, p);
    if (spec_out) *spec_out = s;
    return r;
}

/* key from UI indices */
static const char *const ROOTS[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const SCALES[8] = {"minor_natural", "major", "dorian", "phrygian", "mixolydian", "lydian", "minor_harmonic", "phrygian_dominant"};
static const char *const MODES[8] = {"minor", "major", "dorian", "phrygian", "mixolydian", "lydian", "minor", "goa"};
static inline Key make_key(int root, int scale) {
    root = ((root % 12) + 12) % 12;
    if (scale < 0 || scale > 7) scale = 0;
    return {ROOTS[root], MODES[scale], SCALES[scale]};
}

} // namespace cz
