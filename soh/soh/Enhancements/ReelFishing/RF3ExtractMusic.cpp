// Reel Fishing III mod (step 76b, P4): see RF3ExtractMusic.h. A port of tools/rf3_music.py's "build" path
// (read_bank, layers, parse_sq, find_loop, the SPU envelope, pan_gains, cc_scale, Track, simulate,
// convergence_sample, SourceCache, bent_source, render without the optional reverb, to_int16, write_wav, crc64 and the
// XML of cmd_build); the formats and the driver's rules are in the project's docs/FINDINGS-68-music.md and
// RF3-MECHANICS.md section 9. Every float operation is in the Python tool's order, so the WAVs come out byte for byte
// the same (tools/rf3_extract_check.py checks it). Tracks render on up to 4 threads (step 77a).

#include "RF3ExtractMusic.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

#include "RF3ExtractAudio.h"

namespace rf3extract {

namespace {

using Bytes = std::vector<uint8_t>;

constexpr int kRate = 32000;       // SoH's mixing rate (SOH_RATE)
constexpr int64_t kSpuTick = 48000; // the SPU's envelope clock
constexpr double kCore0Gain = 0.5;
constexpr double kTailLimitS = 20.0;
const char* const kArchiveFolder = "rf3";
constexpr int kFirstTrack = 5; // m04 (flowing water only) is left out of the game's archive (GAME_EXCLUDED)
constexpr int kLastTrack = 21;
// TRACK_INFO's names (what SoH's Audio Editor shows), m04..m21
const char* const kTrackNames[] = { "RF3 Title",       "RF3 Ending",           "RF3 Salmon River",   "RF3 Lake 5",
                                    "RF3 Unused A",    "RF3 Fish Tank 0",      "RF3 Lake 4",         "RF3 Fish Tank 1",
                                    "RF3 Lakes and Rivers", "RF3 Home",        "RF3 River 1",        "RF3 River 4",
                                    "RF3 Lake 1",      "RF3 River 0",          "RF3 Unused B",       "RF3 Big River",
                                    "RF3 Home First Visit", "RF3 Ending Alt" };

struct MusicError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void Fail(const std::string& msg) {
    throw MusicError(msg);
}

void Need(const Bytes& d, size_t o, size_t n) {
    if (o + n > d.size() || o + n < o) {
        Fail("read past the end");
    }
}
uint8_t R8(const Bytes& d, size_t o) {
    Need(d, o, 1);
    return d[o];
}
int8_t RS8(const Bytes& d, size_t o) {
    return static_cast<int8_t>(R8(d, o));
}
uint16_t RU16(const Bytes& d, size_t o) {
    Need(d, o, 2);
    return U16(d, o);
}
uint32_t RU32(const Bytes& d, size_t o) {
    Need(d, o, 4);
    return U32(d, o);
}
std::vector<std::optional<size_t>> Table(const Bytes& d, size_t off) {
    Need(d, off, 16);
    Need(d, off + 16, 4ull * (static_cast<size_t>(U32(d, off + 12)) + 1));
    return ChunkTable(d, off);
}

// ---------------------------------------------------------------- .hd

struct VagE {
    uint32_t offset;
    int rate;
    int loop;
};
struct SmplE {
    int vag, vel0, vel1, base, detune, pan, volume, adsr1, adsr2, attr;
};
struct SsetE {
    int vel0, vel1;
    std::vector<int> smpls;
};
struct SplitE {
    int sset, lo, hi, bendLo, bendHi, volume, pan, transpose, detune;
};
struct ProgE {
    int volume, pan, transpose, detune;
    std::vector<SplitE> splits;
};
struct Bank {
    std::vector<std::optional<VagE>> vags;
    std::vector<std::optional<SmplE>> smpls;
    std::vector<std::optional<SsetE>> ssets;
    std::vector<std::optional<ProgE>> progs;
};

Bank ReadBank(const Bytes& d) {
    const int64_t head = Find(d, "IECSdaeH", 0);
    if (head < 0) {
        Fail("not a Sony .hd (no IECSdaeH chunk)");
    }
    const size_t h = static_cast<size_t>(head);
    const size_t prog = RU32(d, h + 20), sset = RU32(d, h + 24), smpl = RU32(d, h + 28), vagi = RU32(d, h + 32);
    Bank b;
    for (const auto& e : Table(d, vagi)) {
        b.vags.push_back(e ? std::optional<VagE>(VagE{ RU32(d, *e), RU16(d, *e + 4), R8(d, *e + 6) }) : std::nullopt);
    }
    for (const auto& e : Table(d, smpl)) {
        if (!e) {
            b.smpls.push_back(std::nullopt);
            continue;
        }
        const size_t o = *e;
        b.smpls.push_back(SmplE{ RU16(d, o), R8(d, o + 2), R8(d, o + 4), R8(d, o + 11), RS8(d, o + 12), R8(d, o + 13),
                                 R8(d, o + 16), RU16(d, o + 0x12), RU16(d, o + 0x14), R8(d, o + 0x29) });
    }
    for (const auto& e : Table(d, sset)) {
        if (!e) {
            b.ssets.push_back(std::nullopt);
            continue;
        }
        SsetE s{ R8(d, *e + 1), R8(d, *e + 2), {} };
        for (int k = 0; k < R8(d, *e + 3); k++) {
            s.smpls.push_back(RU16(d, *e + 4 + 2 * k));
        }
        b.ssets.push_back(s);
    }
    for (const auto& e : Table(d, prog)) {
        if (!e) {
            b.progs.push_back(std::nullopt);
            continue;
        }
        const size_t o = *e;
        const uint32_t splitRel = RU32(d, o);
        const int n = R8(d, o + 4), size = R8(d, o + 5);
        ProgE p{ R8(d, o + 6), R8(d, o + 7), RS8(d, o + 8), RS8(d, o + 9), {} };
        for (int k = 0; k < n; k++) {
            const size_t s = o + splitRel + static_cast<size_t>(k) * size;
            p.splits.push_back(SplitE{ RU16(d, s), R8(d, s + 2) & 0x7F, R8(d, s + 4) & 0x7F, RU16(d, s + 6),
                                       RU16(d, s + 8), R8(d, s + 16), R8(d, s + 17), RS8(d, s + 18), RS8(d, s + 19) });
        }
        b.progs.push_back(std::move(p));
    }
    return b;
}

struct Layer {
    const SplitE* sp;
    const SmplE* sm;
    const ProgE* p;
};

// layers: every sample a note-on starts (all splits covering the key, all samples of the set in the velocity range).
std::vector<Layer> Layers(const Bank& bank, int program, int key, int velocity) {
    std::vector<Layer> out;
    if (program >= static_cast<int>(bank.progs.size()) || !bank.progs[program]) {
        return out;
    }
    const ProgE& p = *bank.progs[program];
    for (const SplitE& sp : p.splits) {
        if (!(sp.lo <= key && key <= sp.hi)) {
            continue;
        }
        const SsetE* ss = sp.sset < static_cast<int>(bank.ssets.size()) && bank.ssets[sp.sset] ? &*bank.ssets[sp.sset]
                                                                                             : nullptr;
        if (!ss || !(ss->vel0 <= velocity && velocity <= ss->vel1)) {
            continue;
        }
        for (int si : ss->smpls) {
            const SmplE* sm = si < static_cast<int>(bank.smpls.size()) && bank.smpls[si] ? &*bank.smpls[si] : nullptr;
            if (!sm || !((sm->vel0 & 0x7F) <= velocity && velocity <= (sm->vel1 & 0x7F))) {
                continue;
            }
            if (sm->vag >= static_cast<int>(bank.vags.size()) || !bank.vags[sm->vag]) {
                continue;
            }
            out.push_back({ &sp, sm, &p });
        }
    }
    return out;
}

// ---------------------------------------------------------------- .sq

enum class Kind { On, Off, Prog, Cc, Bend, Tempo, End };

struct Event {
    int64_t tick;
    Kind kind;
    int ch;
    int64_t a;
    int b;
    int pass = 1;
};

void ParseSq(const Bytes& d, int* resolution, std::vector<Event>* events) {
    const int64_t midi = Find(d, "IECSidiM", 0);
    if (midi < 0) {
        Fail("not a Sony .sq (no IECSidiM chunk)");
    }
    std::vector<size_t> entries;
    for (const auto& e : Table(d, static_cast<size_t>(midi))) {
        if (e) {
            entries.push_back(*e);
        }
    }
    if (entries.size() != 1) {
        Fail("expected one sequence in the .sq, found " + std::to_string(entries.size()));
    }
    const size_t start = entries[0];
    size_t pos = start + RU32(d, start);
    *resolution = RU16(d, start + 4);
    int64_t tick = 0;
    int status = -1;
    bool noDelta = false;
    auto vlq = [&](size_t* p) {
        // read_vlq; data[pos] past the end raises in Python
        size_t q = *p;
        while (true) {
            if (R8(d, q++) & 0x80) {
                continue;
            }
            break;
        }
        return ReadVlq(d, p);
    };
    while (pos < d.size()) {
        if (!noDelta) {
            tick += vlq(&pos);
        }
        noDelta = false;
        const uint8_t b = R8(d, pos);
        if (b & 0x80) {
            status = b;
            pos++;
        }
        if (status < 0) {
            break;
        }
        if (status == 0xFF) {
            const uint8_t kind = R8(d, pos);
            pos++;
            const uint32_t length = vlq(&pos);
            const size_t payload = pos;
            pos += length;
            if (kind == 0x2F) {
                events->push_back({ tick, Kind::End, 0, 0, 0 });
                break;
            }
            if (kind == 0x51 && length == 3) {
                Need(d, payload, 3);
                events->push_back({ tick, Kind::Tempo, 0, (d[payload] << 16) | (d[payload + 1] << 8) | d[payload + 2], 0 });
            }
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            Fail("sysex in a music sequence at tick " + std::to_string(tick) + ": not handled");
        }
        const int hi = status & 0xF0, ch = status & 0x0F;
        int d1 = R8(d, pos++);
        int d2 = 0;
        if (hi == 0x90 || hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
            d2 = R8(d, pos++);
        }
        noDelta = ((d1 | d2) & 0x80) != 0;
        d1 &= 0x7F;
        d2 &= 0x7F;
        if (hi == 0x90) {
            events->push_back({ tick, d2 ? Kind::On : Kind::Off, ch, d1, d2 });
        } else if (hi == 0x80) {
            events->push_back({ tick, Kind::Off, ch, d1, 0 });
        } else if (hi == 0xC0) {
            events->push_back({ tick, Kind::Prog, ch, d1, 0 });
        } else if (hi == 0xB0) {
            events->push_back({ tick, Kind::Cc, ch, d1, d2 });
        } else if (hi == 0xE0) {
            events->push_back({ tick, Kind::Bend, ch, d1 | (d2 << 7), 0 });
        }
    }
}

struct Loop {
    size_t start, end;
    int count;
};

// find_loop: NRPN 0 + data entry n = loop start n; NRPN 1 + data entry n + LSB count = jump back to loop n.
std::optional<Loop> FindLoop(const std::vector<Event>& events) {
    std::map<int, std::pair<std::optional<int>, std::optional<int>>> nrpn; // channel -> (msb, data)
    std::map<int, size_t> starts;
    for (size_t i = 0; i < events.size(); i++) {
        const Event& e = events[i];
        if (e.kind != Kind::Cc) {
            continue;
        }
        auto& st = nrpn[e.ch];
        if (e.a == 99) {
            st = { e.b, std::nullopt };
        } else if (e.a == 6 && st.first == 0) {
            starts[e.b] = i;
            st.first.reset();
        } else if (e.a == 6 && st.first == 1) {
            st.second = e.b;
        } else if (e.a == 38 && st.first == 1 && st.second) {
            auto it = starts.find(*st.second);
            if (it != starts.end()) {
                return Loop{ it->second, i, e.b };
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- SPU envelope

struct Adsr {
    bool aExp;
    int aShift, aStep, dShift;
    int64_t sLevel;
    bool sExp, sDec;
    int sShift, sStep;
    bool rExp;
    int rShift;
};

Adsr AdsrParams(int adsr1, int adsr2) {
    Adsr p;
    p.aExp = (adsr1 & 0x8000) != 0;
    p.aShift = (adsr1 >> 10) & 0x1F;
    p.aStep = 7 - ((adsr1 >> 8) & 3);
    p.dShift = (adsr1 >> 4) & 0x0F;
    p.sLevel = ((adsr1 & 0x0F) + 1) * 0x800;
    p.sExp = (adsr2 & 0x8000) != 0;
    p.sDec = (adsr2 & 0x4000) != 0;
    p.sShift = (adsr2 >> 8) & 0x1F;
    p.sStep = (adsr2 & 0x4000) ? (-8 + ((adsr2 >> 6) & 3)) : (7 - ((adsr2 >> 6) & 3));
    p.rExp = (adsr2 & 0x20) != 0;
    p.rShift = adsr2 & 0x1F;
    return p;
}

// adsr_step: one envelope step by the SPU's rule: (ticks to wait, level change).
void AdsrStep(int64_t level, bool exp, int shift, int step, int64_t* cycles, int64_t* st) {
    *cycles = int64_t(1) << std::max(0, shift - 11);
    *st = step > 0 ? int64_t(step) << std::max(0, 11 - shift) : -(int64_t(-step) << std::max(0, 11 - shift));
    if (exp && *st > 0 && level > 0x6000) {
        *cycles *= 4;
    }
    if (exp && *st < 0) {
        const int64_t prod = *st * level; // Python's >> floors
        *st = prod >= 0 ? prod >> 15 : -((-prod + 32767) >> 15);
    }
}

class Envelopes {
  public:
    struct Held {
        std::vector<double> out;
        double tail = 0;
        bool final = false;
    };

    // held_curve: key-on to key-off, at least `length` samples unless the curve settles.
    const Held& HeldCurve(int adsr1, int adsr2, int64_t length) {
        const auto key = std::make_pair(adsr1, adsr2);
        auto it = mHeld.find(key);
        if (it != mHeld.end() && (static_cast<int64_t>(it->second.out.size()) >= length || it->second.final)) {
            return it->second;
        }
        const Adsr p = AdsrParams(adsr1, adsr2);
        Held h;
        int64_t n = 0, tick = 0, level = 0, cycles, st;
        char phase = 'a';
        while (n < length) {
            if (phase == 'a') {
                AdsrStep(level, p.aExp, p.aShift, p.aStep, &cycles, &st);
                n = Fill(&h.out, tick, cycles, level, n);
                tick += cycles;
                level = std::min<int64_t>(0x7FFF, level + st);
                if (level >= 0x7FFF) {
                    phase = 'd';
                }
            } else if (phase == 'd') {
                if (level <= p.sLevel) {
                    phase = 's';
                    continue;
                }
                AdsrStep(level, true, p.dShift, -8, &cycles, &st);
                n = Fill(&h.out, tick, cycles, level, n);
                tick += cycles;
                level = std::max<int64_t>(0, level + st);
            } else {
                if ((p.sDec && level <= 0) || (!p.sDec && level >= 0x7FFF)) {
                    h.final = true;
                    break;
                }
                AdsrStep(level, p.sExp, p.sShift, p.sStep, &cycles, &st);
                n = Fill(&h.out, tick, cycles, level, n);
                tick += cycles;
                level = std::max<int64_t>(0, std::min<int64_t>(0x7FFF, level + st));
            }
        }
        h.tail = level / 32768.0;
        mHeld[key] = std::move(h);
        return mHeld[key];
    }

    // silent_after: samples after key-on when a held note's sustain has faded to 0 (nullopt if not within 600 s).
    std::optional<int64_t> SilentAfter(int adsr1, int adsr2) {
        const auto key = std::make_pair(adsr1, adsr2);
        auto it = mSilent.find(key);
        if (it != mSilent.end()) {
            return it->second;
        }
        const Adsr p = AdsrParams(adsr1, adsr2);
        std::optional<int64_t> result;
        if (p.sDec) {
            int64_t tick = 0, level = 0, cycles, st;
            char phase = 'a';
            while (tick < 600 * kSpuTick) {
                if (phase == 'a') {
                    AdsrStep(level, p.aExp, p.aShift, p.aStep, &cycles, &st);
                    tick += cycles;
                    level = std::min<int64_t>(0x7FFF, level + st);
                    if (level >= 0x7FFF) {
                        phase = 'd';
                    }
                } else if (phase == 'd') {
                    if (level <= p.sLevel) {
                        phase = 's';
                        continue;
                    }
                    AdsrStep(level, true, p.dShift, -8, &cycles, &st);
                    tick += cycles;
                    level = std::max<int64_t>(0, level + st);
                } else {
                    if (level <= 0) {
                        result = (tick * kRate + kSpuTick - 1) / kSpuTick;
                        break;
                    }
                    AdsrStep(level, p.sExp, p.sShift, p.sStep, &cycles, &st);
                    tick += cycles;
                    level = std::max<int64_t>(0, std::min<int64_t>(0x7FFF, level + st));
                }
            }
        }
        mSilent[key] = result;
        return result;
    }

    // release_curve: from full level to silence (cut after TAIL_LIMIT_S), and the curve negated (for bisect).
    const std::pair<std::vector<double>, std::vector<double>>& ReleaseCurve(int adsr2) {
        auto it = mRelease.find(adsr2);
        if (it != mRelease.end()) {
            return it->second;
        }
        const Adsr p = AdsrParams(0, adsr2);
        std::vector<double> out;
        int64_t n = 0, tick = 0, level = 0x7FFF, cycles, st;
        const int64_t limit = static_cast<int64_t>(kTailLimitS * kRate);
        while (level > 0 && n < limit) {
            AdsrStep(level, p.rExp, p.rShift, -8, &cycles, &st);
            n = Fill(&out, tick, cycles, level, n);
            tick += cycles;
            level = std::max<int64_t>(0, level + st);
        }
        out.push_back(0.0);
        std::vector<double> neg;
        neg.reserve(out.size());
        for (double v : out) {
            neg.push_back(-v);
        }
        return mRelease[adsr2] = { std::move(out), std::move(neg) };
    }

    // voice: the envelope of a note held `gate` samples, running to silence.
    std::vector<double> Voice(int adsr1, int adsr2, int64_t gate) {
        const Held& held = HeldCurve(adsr1, adsr2, gate);
        const auto silent = SilentAfter(adsr1, adsr2);
        if (silent && gate >= *silent) {
            return Prefix(held.out, *silent); // the sustain faded to nothing before the note-off
        }
        std::vector<double> env = Prefix(held.out, gate);
        if (static_cast<int64_t>(env.size()) < gate) {
            env.insert(env.end(), static_cast<size_t>(gate - env.size()), held.tail);
        }
        const double level = env.empty() ? 0.0 : env.back();
        const auto& rel = ReleaseCurve(adsr2);
        size_t lo = 0, hi = rel.second.size(); // bisect_left(neg, -level)
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (rel.second[mid] < -level) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        env.insert(env.end(), rel.first.begin() + lo, rel.first.end());
        return env;
    }

    static std::vector<double> Prefix(const std::vector<double>& v, int64_t n) {
        const size_t k = n <= 0 ? 0 : std::min(v.size(), static_cast<size_t>(n));
        return std::vector<double>(v.begin(), v.begin() + k);
    }

  private:
    static int64_t Fill(std::vector<double>* out, int64_t tick, int64_t cycles, int64_t level, int64_t nDone) {
        const int64_t nEnd = ((tick + cycles) * kRate + kSpuTick - 1) / kSpuTick;
        if (nEnd > nDone) {
            out->insert(out->end(), static_cast<size_t>(nEnd - nDone), level / 32768.0);
        }
        return nEnd;
    }

    std::map<std::pair<int, int>, Held> mHeld;
    std::map<std::pair<int, int>, std::optional<int64_t>> mSilent;
    std::map<int, std::pair<std::vector<double>, std::vector<double>>> mRelease;
};

// pan_gains (MODHSYN 0x4B34): p = bank pan + CC 10 - 64; (left, right) in 0..1.
std::pair<double, double> PanGains(int p) {
    int left = 128, right = 128;
    const int d = p - 64;
    if (d > 0) {
        left = d >= 63 ? 0 : (63 - d) * 128 / 63;
    } else if (d < 0) {
        right = d < -62 ? 0 : (p - 1) * 128 / 63;
    }
    return { left / 128.0, right / 128.0 };
}

double CcScale(int v) {
    return (v * 128 / 127) / 128.0;
}

// ---------------------------------------------------------------- the renderer

struct Sample {
    std::vector<int> samples;
    std::optional<size_t> loopStart;
    int rate;
};

struct Track {
    int n;
    Bank bank;
    Bytes bd;
    int resolution = 0;
    std::vector<Event> events;
    std::optional<Loop> loop;
    std::map<int, Sample> decoded;

    const Sample& Get(int vag) {
        auto it = decoded.find(vag);
        if (it != decoded.end()) {
            return it->second;
        }
        const VagE& v = *bank.vags.at(vag);
        Decoded dec = DecodeAdpcm(bd, v.offset);
        Sample s{ std::move(dec.samples), (v.loop && dec.loopStart) ? dec.loopStart : std::nullopt, v.rate };
        return decoded[vag] = std::move(s);
    }

    // timeline: pass 1 (to the loop jump) then pass 2 (the body again, ticks running on).
    std::vector<Event> Timeline(std::optional<int64_t>* tStart, int64_t* tEnd) const {
        if (!loop) {
            *tStart = std::nullopt;
            *tEnd = events.back().tick;
            return events;
        }
        *tStart = events[loop->start].tick;
        *tEnd = events[loop->end].tick;
        std::vector<Event> evs(events.begin(), events.begin() + loop->end);
        const int64_t shift = *tEnd - **tStart;
        for (size_t i = loop->start + 1; i < loop->end; i++) {
            Event e = events[i];
            e.tick += shift;
            e.pass = 2;
            evs.push_back(e);
        }
        return evs;
    }
};

struct Voice {
    int64_t start;
    std::optional<int64_t> gate;
    int ch, key, vag;
    double semis;
    int bendLo, bendHi;
    double level;
    int bankpan, adsr1, adsr2, core;
    std::optional<int64_t> end;
    int pass;
};

struct AutoPoint {
    int64_t pos;
    int cc7, cc11, cc10, bend;
};

struct Sim {
    std::vector<Voice> voices;
    std::vector<std::vector<AutoPoint>> autos;
    std::optional<int64_t> sStart, sEnd;
};

int64_t Pos(double seconds) {
    return PyRoundInt(seconds * kRate);
}

Sim Simulate(Track& track, Envelopes& env) {
    std::optional<int64_t> tStart;
    int64_t tEnd = 0;
    const std::vector<Event> evs = track.Timeline(&tStart, &tEnd);
    int64_t tempo = 500000;
    const int res = track.resolution;
    double seconds = 0.0;
    int64_t lastTick = 0;
    struct Chan {
        int prog = 0, cc7 = 127, cc11 = 127, cc10 = 64, bend = 0;
    };
    std::vector<Chan> chans(16);
    Sim sim;
    sim.autos.assign(16, { AutoPoint{ 0, 127, 127, 64, 0 } });
    std::map<std::pair<int, int>, std::vector<size_t>> active;
    int64_t endSample = 0;
    for (const Event& e : evs) {
        seconds += static_cast<double>((e.tick - lastTick) * tempo) / 1e6 / res;
        lastTick = e.tick;
        const int64_t pos = Pos(seconds);
        endSample = pos;
        if (tStart && !sim.sStart && e.tick >= *tStart) {
            sim.sStart = pos;
        }
        if (e.pass == 2 && !sim.sEnd) {
            sim.sEnd = Pos(seconds - static_cast<double>((e.tick - tEnd) * tempo) / 1e6 / res);
        }
        Chan& c = chans[e.ch];
        if (e.kind == Kind::Tempo) {
            tempo = e.a;
        } else if (e.kind == Kind::Prog) {
            c.prog = static_cast<int>(e.a);
        } else if (e.kind == Kind::Cc && (e.a == 7 || e.a == 10 || e.a == 11)) {
            (e.a == 7 ? c.cc7 : e.a == 10 ? c.cc10 : c.cc11) = e.b;
            sim.autos[e.ch].push_back({ pos, c.cc7, c.cc11, c.cc10, c.bend });
        } else if (e.kind == Kind::Bend) {
            c.bend = static_cast<int>(e.a) - 0x2000;
            sim.autos[e.ch].push_back({ pos, c.cc7, c.cc11, c.cc10, c.bend });
        } else if (e.kind == Kind::On) {
            const int key = static_cast<int>(e.a);
            for (const Layer& l : Layers(track.bank, c.prog, key, e.b)) {
                Voice v;
                v.start = pos;
                v.ch = e.ch;
                v.key = key;
                v.pass = e.pass;
                v.vag = l.sm->vag;
                v.semis = (key - l.sm->base + l.p->transpose + l.sp->transpose) +
                          (l.sm->detune + l.p->detune + l.sp->detune) / 128.0;
                v.bendLo = l.sp->bendLo;
                v.bendHi = l.sp->bendHi;
                v.level = static_cast<double>(static_cast<int64_t>(l.p->volume) * l.sp->volume * l.sm->volume * e.b) /
                          static_cast<double>(1 << 28);
                v.bankpan = std::max(0, std::min(127, l.p->pan + l.sp->pan + l.sm->pan - 128));
                v.adsr1 = l.sm->adsr1;
                v.adsr2 = l.sm->adsr2;
                const int core = l.sm->attr & 0x30;
                v.core = core == 0x10 ? 0 : core == 0x20 ? 1 : -1;
                if (v.core == 0) {
                    v.level *= kCore0Gain;
                }
                sim.voices.push_back(v);
                active[{ e.ch, key }].push_back(sim.voices.size() - 1);
            }
        } else if (e.kind == Kind::Off) {
            auto it = active.find({ e.ch, static_cast<int>(e.a) });
            if (it != active.end()) {
                for (size_t vi : it->second) {
                    sim.voices[vi].gate = pos - sim.voices[vi].start;
                }
                active.erase(it);
            }
        }
    }
    if (tStart && !sim.sEnd) {
        sim.sEnd = endSample;
    }
    // Voice ends: the note-off plus the envelope's release, or the sample's end for one-shots.
    for (Voice& v : sim.voices) {
        const Sample& s = track.Get(v.vag);
        if (!v.gate) {
            const auto silent = env.SilentAfter(v.adsr1, v.adsr2);
            v.end = silent ? std::optional<int64_t>(v.start + *silent) : std::nullopt;
        } else {
            v.end = v.start + static_cast<int64_t>(env.Voice(v.adsr1, v.adsr2, *v.gate).size());
        }
        if (!s.loopStart) {
            const double ratio = s.rate * std::pow(2.0, v.semis / 12.0) / kRate;
            const int64_t oneShotEnd = v.start + static_cast<int64_t>(s.samples.size() / ratio);
            v.end = v.end ? std::min(*v.end, oneShotEnd) : oneShotEnd;
        }
    }
    return sim;
}

// convergence_sample: the body offset from which pass 1's channel state equals pass 2's.
int64_t ConvergenceSample(const Track& track) {
    const size_t iStart = track.loop->start, iEnd = track.loop->end;
    // (channel, variable) with variable -1 = program, -2 = bend, else the CC number
    auto stateAt = [&](size_t iStop) {
        std::map<std::pair<int, int>, int64_t> st;
        for (size_t i = 0; i <= iStop && i < track.events.size(); i++) {
            const Event& e = track.events[i];
            if (e.kind == Kind::Prog) {
                st[{ e.ch, -1 }] = e.a;
            } else if (e.kind == Kind::Cc && (e.a == 7 || e.a == 10 || e.a == 11)) {
                st[{ e.ch, static_cast<int>(e.a) }] = e.b;
            } else if (e.kind == Kind::Bend) {
                st[{ e.ch, -2 }] = e.a;
            }
        }
        return st;
    };
    auto def = [](int var) -> int64_t { return var == -1 ? 0 : var == 7 ? 127 : var == 10 ? 64 : var == 11 ? 127 : 0x2000; };
    const auto s1 = stateAt(iStart), s2 = stateAt(iEnd);
    std::set<std::pair<int, int>> keys;
    for (const auto& kv : s1) {
        keys.insert(kv.first);
    }
    for (const auto& kv : s2) {
        keys.insert(kv.first);
    }
    int64_t convTick = track.events[iStart].tick;
    for (const auto& k : keys) {
        const int64_t a = s1.count(k) ? s1.at(k) : def(k.second);
        const int64_t b = s2.count(k) ? s2.at(k) : def(k.second);
        if (a == b) {
            continue;
        }
        for (size_t i = iStart + 1; i < iEnd; i++) {
            const Event& e = track.events[i];
            if (e.ch != k.first) {
                continue;
            }
            if ((k.second == -1 && e.kind == Kind::Prog) || (k.second == -2 && e.kind == Kind::Bend) ||
                (e.kind == Kind::Cc && e.a == k.second)) {
                convTick = std::max(convTick, e.tick);
                break;
            }
        }
    }
    int64_t tempo = 500000;
    double seconds = 0.0;
    int64_t last = 0;
    for (const Event& e : track.events) {
        if (e.tick > convTick) {
            break;
        }
        seconds += static_cast<double>((e.tick - last) * tempo) / 1e6 / track.resolution;
        last = e.tick;
        if (e.kind == Kind::Tempo) {
            tempo = e.a;
        }
    }
    seconds += static_cast<double>((convTick - last) * tempo) / 1e6 / track.resolution;
    return Pos(seconds);
}

// SourceCache: each (sample, pitch) resampled once, keyed by round(semis, 6) (the first pitch with a key makes it).
class SourceCache {
  public:
    SourceCache(Track& track, Resampler& rs) : mTrack(track), mRs(rs) {
    }

    std::vector<double> Get(int vag, double semis, int64_t length) {
        const auto key = std::make_pair(vag, PyRound(semis, 6));
        const Sample& s = mTrack.Get(vag);
        auto it = mCache.find(key);
        if (it == mCache.end()) {
            const double srcRate = s.rate * std::pow(2.0, semis / 12.0);
            Entry e;
            if (s.loopStart) {
                int64_t startOut = 0;
                e.head = mRs.ResampleLoop(s.samples, *s.loopStart, srcRate, kRate, &startOut);
                e.startOut = startOut;
            } else {
                e.head = mRs.Resample(s.samples, srcRate, kRate);
            }
            it = mCache.emplace(key, std::move(e)).first;
        }
        const std::vector<double>& head = it->second.head;
        const auto& startOut = it->second.startOut;
        if (!startOut || static_cast<int64_t>(head.size()) >= length) {
            return Envelopes::Prefix(head, length);
        }
        if (static_cast<size_t>(*startOut) >= head.size()) {
            return Envelopes::Prefix(head, length);
        }
        // the loop unrolled for this note only
        std::vector<double> out = head;
        while (static_cast<int64_t>(out.size()) < length) {
            out.insert(out.end(), head.begin() + *startOut, head.end());
        }
        out.resize(static_cast<size_t>(length));
        return out;
    }

  private:
    struct Entry {
        std::vector<double> head;
        std::optional<int64_t> startOut;
    };
    Track& mTrack;
    Resampler& mRs;
    std::map<std::pair<int, double>, Entry> mCache;
};

// bent_source: `length` samples with a pitch that changes per segment, by linear interpolation.
std::vector<double> BentSource(Track& track, int vag, const std::vector<std::pair<int64_t, double>>& plan,
                               int64_t length) {
    const Sample& s = track.Get(vag);
    const int64_t nSrc = static_cast<int64_t>(s.samples.size());
    std::vector<double> out;
    double pos = 0.0;
    int64_t i = 0;
    auto at = [&](int64_t j) {
        if (j < 0 || j >= nSrc) {
            Fail("bent source index out of range");
        }
        return s.samples[static_cast<size_t>(j)];
    };
    for (const auto& seg : plan) {
        const double step = s.rate * std::pow(2.0, seg.second / 12.0) / kRate;
        for (int64_t k = 0; k < seg.first; k++) {
            if (i >= length) {
                return out;
            }
            int64_t j = static_cast<int64_t>(pos);
            if (j + 1 >= nSrc) {
                if (!s.loopStart) {
                    return out;
                }
                pos -= static_cast<double>(nSrc - static_cast<int64_t>(*s.loopStart));
                j = static_cast<int64_t>(pos);
            }
            const double frac = pos - static_cast<double>(j);
            const int s0 = at(j);
            const int s1 = j + 1 < nSrc ? at(j + 1) : (s.loopStart ? at(static_cast<int64_t>(*s.loopStart)) : 0);
            out.push_back(s0 + (s1 - s0) * frac);
            pos += step;
            i++;
        }
    }
    return out;
}

struct Rendered {
    std::vector<double> left, right;
    std::optional<std::pair<int64_t, int64_t>> loop;
};

Rendered Render(Track& track, Resampler& rs) {
    Envelopes env;
    Sim sim = Simulate(track, env);
    Rendered r;
    int64_t fileEnd = 0;
    if (!sim.sStart) {
        for (const Voice& v : sim.voices) {
            fileEnd = std::max(fileEnd, (v.end && *v.end) ? *v.end : v.start);
        }
    } else {
        const int64_t sLs = *sim.sStart, sLe = *sim.sEnd;
        const int64_t conv = ConvergenceSample(track);
        // Pass-1 voices that differ from pass 2's, and the pass-1 voices still sounding when pass 2 starts.
        int64_t x = conv - sLs;
        for (const Voice& v : sim.voices) {
            if (v.pass != 1) {
                continue;
            }
            const int64_t end = v.end ? *v.end : sLe + static_cast<int64_t>(kTailLimitS * kRate);
            if (v.start < conv) {
                x = std::max(x, end - sLs);
            }
            if (end > sLe) {
                x = std::max(x, end - sLe);
            }
        }
        x = std::max<int64_t>(0, x) + 64;
        if (x >= sLe - sLs) {
            x = sLe - sLs - 1; // the loop needs more than one body pass to settle: loop start kept at the body's end
        }
        fileEnd = sLe + x;
        r.loop = std::make_pair(sLs + x, sLe + x);
    }
    r.left.assign(static_cast<size_t>(fileEnd), 0.0);
    r.right.assign(static_cast<size_t>(fileEnd), 0.0);
    SourceCache sources(track, rs);
    for (const Voice& v : sim.voices) {
        if (v.start >= fileEnd) {
            continue;
        }
        const int64_t stop = v.end ? std::min(*v.end, fileEnd) : fileEnd;
        int64_t length = stop - v.start;
        if (length <= 0) {
            continue;
        }
        std::vector<double> envv;
        if (v.gate && *v.gate < length) {
            envv = env.Voice(v.adsr1, v.adsr2, *v.gate);
        } else {
            const Envelopes::Held& held = env.HeldCurve(v.adsr1, v.adsr2, length);
            envv = Envelopes::Prefix(held.out, length);
            const auto silent = env.SilentAfter(v.adsr1, v.adsr2);
            if (silent) {
                length = std::min(length, *silent);
            }
            if (static_cast<int64_t>(envv.size()) < length) {
                envv.insert(envv.end(), static_cast<size_t>(length - envv.size()), held.tail);
            }
        }
        length = std::min<int64_t>(length, static_cast<int64_t>(envv.size()));
        // The channel's automation over the voice: segments (offset, cc7, cc11, cc10, bend).
        const std::vector<AutoPoint>& chAuto = sim.autos[v.ch];
        size_t lo = 0, hi = chAuto.size(); // bisect_right(positions, start) - 1
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (v.start < chAuto[mid].pos) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (lo == 0) {
            Fail("a voice before the channel's first automation point");
        }
        size_t k = lo - 1;
        std::vector<AutoPoint> segs{ chAuto[k] };
        segs[0].pos = 0;
        k++;
        while (k < chAuto.size() && chAuto[k].pos < v.start + length) {
            AutoPoint p = chAuto[k];
            p.pos -= v.start;
            segs.push_back(p);
            k++;
        }
        bool oneBend = true;
        for (const AutoPoint& s : segs) {
            oneBend = oneBend && s.bend == segs[0].bend;
        }
        auto bendSemis = [&v](int b) {
            const int rng = b < 0 ? v.bendLo : v.bendHi;
            return static_cast<double>(static_cast<int64_t>(rng) * b) / 8192.0 / 128.0;
        };
        std::vector<double> src;
        if (oneBend) {
            src = sources.Get(v.vag, v.semis + bendSemis(segs[0].bend), length);
        } else {
            std::vector<std::pair<int64_t, double>> plan;
            for (size_t i = 0; i < segs.size(); i++) {
                const int64_t nxt = i + 1 < segs.size() ? segs[i + 1].pos : length;
                plan.push_back({ nxt - segs[i].pos, v.semis + bendSemis(segs[i].bend) });
            }
            src = BentSource(track, v.vag, plan, length);
        }
        length = std::min<int64_t>(length, static_cast<int64_t>(src.size()));
        if (length <= 0) {
            continue;
        }
        std::vector<double> wave(static_cast<size_t>(length));
        for (int64_t i = 0; i < length; i++) {
            wave[i] = src[i] * envv[i];
        }
        for (size_t i = 0; i < segs.size(); i++) {
            const int64_t nxt = i + 1 < segs.size() ? segs[i + 1].pos : length;
            const int64_t a = std::max<int64_t>(0, segs[i].pos), b = std::min(nxt, length);
            if (b <= a) {
                continue;
            }
            const double g = v.level * CcScale(segs[i].cc7) * CcScale(segs[i].cc11);
            const auto gains = PanGains(v.bankpan + std::max(-63, std::min(63, segs[i].cc10 - 64)));
            const int64_t j0 = v.start + a;
            for (auto& [buf, gain] : { std::make_pair(&r.left, g * gains.first), std::make_pair(&r.right, g * gains.second) }) {
                if (gain == 0) {
                    continue;
                }
                double* out = buf->data() + j0;
                for (int64_t t = a; t < b; t++) {
                    out[t - a] = out[t - a] + wave[t] * gain;
                }
            }
        }
    }
    return r;
}

// to_int16: scaled down only if it would clip; the peak in dBFS.
std::vector<int16_t> ToInt16(const std::vector<double>& v, double used) {
    std::vector<int16_t> out;
    out.reserve(v.size());
    for (double x : v) {
        const int64_t s = PyRoundInt(x * used);
        if (s < -32768 || s > 32767) {
            Fail("a sample out of 16-bit range");
        }
        out.push_back(static_cast<int16_t>(s));
    }
    return out;
}

Bytes WavMono(const std::vector<int16_t>& pcm) {
    Bytes out;
    auto put16 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto put32 = [&out](uint32_t v) {
        for (int k = 0; k < 4; k++) {
            out.push_back(static_cast<uint8_t>(v >> (8 * k)));
        }
    };
    auto tag = [&out](const char* t) { out.insert(out.end(), t, t + 4); };
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    tag("RIFF");
    put32(4 + 8 + 16 + 8 + bytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(kRate);
    put32(kRate * 2);
    put16(2);
    put16(16);
    tag("data");
    put32(bytes);
    for (int16_t s : pcm) {
        put16(static_cast<uint16_t>(s));
    }
    return out;
}

// libultraship's CRC64 (StrHash64.cpp): ECMA-182, MSB first, init all ones, no final xor.
uint64_t Crc64(const std::string& text) {
    uint64_t crc = ~0ull;
    for (unsigned char byte : text) {
        crc ^= static_cast<uint64_t>(byte) << 56;
        for (int k = 0; k < 8; k++) {
            crc = (crc & (1ull << 63)) ? ((crc << 1) ^ 0x42F0E1EBA9EA3693ull) : (crc << 1);
        }
    }
    return crc;
}

std::string XmlAttr(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '"') {
            out += "&quot;";
        } else if (c == '<') {
            out += "&lt;";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string SequenceXml(const std::string& fontPath, double seconds, bool looped) {
    const uint64_t crc = Crc64(fontPath);
    std::string fonts;
    for (int i = 0; i < 8; i++) {
        fonts += (i ? "\n" : "") + std::string("        <FontIndex FontIdx=\"") +
                 std::to_string((crc >> (8 * i)) & 0xFF) + "\"/>";
    }
    return "<Sequence Version=\"0\" Index=\"0\" Medium=\"Cart\" CachePolicy=\"Either\" Size=\"0\" Streamed=\"true\" "
           "Stereo=\"true\" Looped=\"" +
           std::string(looped ? "true" : "false") + "\" Length=\"" +
           std::to_string(static_cast<int64_t>(std::ceil(seconds)) + 2) + "\">\n    <FontIndicies>\n" + fonts +
           "\n    </FontIndicies>\n</Sequence>\n";
}

std::string FontXml(const std::string& sampleL, const std::string& sampleR) {
    std::string inst;
    for (const std::string* ref : { &sampleL, &sampleR }) {
        if (!inst.empty()) {
            inst += "\n";
        }
        inst += "        <Instrument IsValid=\"true\" Loaded=\"0\" NormalRangeLo=\"0\" NormalRangeHi=\"127\" "
                "ReleaseRate=\"240\">\n"
                "            <Envelopes>\n"
                "                <Envelope Delay=\"1\" Arg=\"32700\"/>\n"
                "                <Envelope Delay=\"-1\" Arg=\"0\"/>\n"
                "            </Envelopes>\n"
                "            <NormalNotesSound SampleRef=\"" +
                XmlAttr(*ref) +
                "\" Tuning=\"1.0\"/>\n"
                "        </Instrument>";
    }
    return "<SoundFont Version=\"0\" Num=\"0\" Medium=\"Cart\" CachePolicy=\"Either\" Data1=\"65535\" Data2=\"0\" "
           "Data3=\"0\">\n    <Drums/>\n    <Instruments>\n" +
           inst + "\n    </Instruments>\n</SoundFont>\n";
}

std::string SampleXml(const std::string& wavPath, int64_t frames,
                      const std::optional<std::pair<int64_t, int64_t>>& loop) {
    const int64_t start = loop ? loop->first : 0, end = loop ? loop->second : frames;
    return "<Sample Version=\"0\" Codec=\"S16\" Medium=\"Ram\" bit26=\"0\" Relocated=\"0\" Size=\"" +
           std::to_string(frames * 2) + "\" Path=\"" + XmlAttr(wavPath) + "\" CustomFormat=\"wav\">\n" +
           "    <ADPCMLoop Start=\"" + std::to_string(start) + "\" End=\"" + std::to_string(end) + "\" Count=\"" +
           (loop ? "4294967295" : "0") + "\"/>\n</Sample>\n";
}

Bytes Text(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

Track ReadTrack(rf3disc::Pack& pack, int n) {
    char base[32];
    std::snprintf(base, sizeof(base), "sound/music/m%02d.", n);
    auto read = [&](const char* ext) {
        Bytes d;
        if (!pack.Read(base + std::string(ext), &d)) {
            Fail(std::string(base) + ext + " is not in this disc's PACKFILE.FPK");
        }
        return d;
    };
    Track tr;
    tr.n = n;
    const Bytes sq = read("sq");
    tr.bank = ReadBank(read("hd"));
    tr.bd = read("bd");
    ParseSq(sq, &tr.resolution, &tr.events);
    if (tr.events.empty()) {
        Fail(std::string(base) + "sq: no events");
    }
    if (tr.resolution == 0) {
        tr.resolution = 480; // the driver would divide by zero; 480 assumed (m18)
    }
    tr.loop = FindLoop(tr.events);
    return tr;
}

struct TrackOut {
    std::vector<int16_t> left, right;
    std::optional<std::pair<int64_t, int64_t>> loop;
    double peakDb = 0;
};

// render_all's per-track part: render, then to_int16 (gain 0 dB, scaled down only if it would clip).
TrackOut RenderTrack(Track tr, Resampler& rs) {
    Rendered r = Render(tr, rs);
    tr.decoded.clear();
    double peak = 0.0;
    if (!r.left.empty()) {
        peak = std::max({ *std::max_element(r.left.begin(), r.left.end()),
                          -*std::min_element(r.left.begin(), r.left.end()),
                          *std::max_element(r.right.begin(), r.right.end()),
                          -*std::min_element(r.right.begin(), r.right.end()) });
    }
    const double gain = 1.0; // 10 ** (0 / 20)
    const double used = peak * gain > 32767 ? 32767 / peak : gain;
    TrackOut o;
    o.left = ToInt16(r.left, used);
    r.left = {};
    o.right = ToInt16(r.right, used);
    r.right = {};
    int p = 0;
    for (const auto* ch : { &o.left, &o.right }) {
        for (int16_t s : *ch) {
            p = std::max(p, std::abs(static_cast<int>(s)));
        }
    }
    o.peakDb = p ? 20 * std::log10(p / 32768.0) : -999.0;
    o.loop = r.loop;
    return o;
}

// Runs job(i) for i in `items` on up to 4 threads (fewer on small CPUs); the first exception stops the rest.
template <typename Job> void Parallel(const std::vector<size_t>& items, Job job) {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned threads = std::max(1u, std::min(4u, hw > 1 ? hw - 1 : 1u));
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> stop{ false };
    std::mutex errMutex;
    std::string error;
    auto worker = [&]() {
        for (size_t k; !stop && (k = next++) < items.size();) {
            try {
                job(items[k]);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(errMutex);
                if (error.empty()) {
                    error = e.what();
                }
                stop = true;
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) {
        pool.emplace_back(worker);
    }
    for (std::thread& t : pool) {
        t.join();
    }
    if (!error.empty()) {
        Fail(error);
    }
}

// Step 77a: the tracks on several threads, each with its own kernel cache. Python's one run shares one cache in which
// the first ratio asked for a key (round(ratio, 9)) makes that key's kernel; a few keys are asked for with ratios a
// rounding apart, and some of those kernels differ. So after the first pass: the kernel Python would use for each key
// is the one from the lowest-numbered track that asked for it, and every track that used a different kernel is
// rendered again with those kernels. *redone = how many.
std::vector<TrackOut> RenderAll(const std::vector<Track>& tracks, const ProgressFn& progress, size_t* redone) {
    const size_t n = tracks.size();
    std::vector<TrackOut> outs(n);
    std::vector<Resampler> caches(n);
    std::atomic<size_t> done{ 0 };
    std::atomic<size_t> total{ n };
    auto report = [&](size_t i) {
        const size_t d = ++done;
        if (progress) {
            progress("m" + std::to_string(tracks[i].n) + " rendered",
                     static_cast<float>(d) / static_cast<float>(total.load()));
        }
    };
    std::vector<size_t> all;
    for (size_t i = 0; i < n; i++) {
        all.push_back(i);
    }
    Parallel(all, [&](size_t i) {
        outs[i] = RenderTrack(tracks[i], caches[i]);
        report(i);
    });
    // the kernels of Python's run: per key, the first track's
    std::map<double, const Kernel*> python;
    for (size_t i = 0; i < n; i++) {
        for (const auto& kv : caches[i].Cache()) {
            python.emplace(kv.first, &kv.second);
        }
    }
    std::map<double, double> seed;
    for (const auto& kv : python) {
        seed[kv.first] = kv.second->ratio;
    }
    std::vector<size_t> redo;
    for (size_t i = 0; i < n; i++) {
        for (const auto& kv : caches[i].Cache()) {
            const Kernel* p = python.at(kv.first);
            if (p != &kv.second && (p->half != kv.second.half || p->table != kv.second.table)) {
                redo.push_back(i);
                break;
            }
        }
    }
    *redone = redo.size();
    total += redo.size();
    std::vector<Resampler> seeded(n);
    for (size_t i : redo) {
        seeded[i].Seed(seed);
    }
    Parallel(redo, [&](size_t i) {
        outs[i] = RenderTrack(tracks[i], seeded[i]);
        report(i);
    });
    return outs;
}

void Build(rf3disc::Pack& pack, std::vector<Entry>* out, const ProgressFn& progress) {
    std::vector<Track> tracks;
    for (int n = kFirstTrack; n <= kLastTrack; n++) {
        tracks.push_back(ReadTrack(pack, n)); // the disc reader is not thread-safe: everything is read first
    }
    size_t redone = 0;
    std::vector<TrackOut> outs = RenderAll(tracks, progress, &redone);
    tracks.clear();
    if (progress) {
        progress("all tracks rendered (" + std::to_string(redone) + " again with Python's kernels)", 1.0f);
    }
    std::vector<std::string> index;
    for (size_t i = 0; i < outs.size(); i++) {
        const int n = kFirstTrack + static_cast<int>(i);
        TrackOut& o = outs[i];
        const int64_t frames = static_cast<int64_t>(o.left.size());
        const std::string name = kTrackNames[n - 4];
        char m[8];
        std::snprintf(m, sizeof(m), "m%02d", n);
        const std::string seq = std::string("custom/music/") + kArchiveFolder + "/" + name + "_bgm";
        const std::string font = std::string("custom/fonts/") + kArchiveFolder + "/" + m;
        const std::string sampleL = std::string("custom/samples/") + kArchiveFolder + "/" + m + "_L";
        const std::string sampleR = std::string("custom/samples/") + kArchiveFolder + "/" + m + "_R";
        out->push_back({ seq, Text(SequenceXml(font, static_cast<double>(frames) / kRate, o.loop.has_value())) });
        out->push_back({ font, Text(FontXml(sampleL, sampleR)) });
        out->push_back({ sampleL, Text(SampleXml(sampleL + ".wav", frames, o.loop)) });
        out->push_back({ sampleR, Text(SampleXml(sampleR + ".wav", frames, o.loop)) });
        out->push_back({ sampleL + ".wav", WavMono(o.left) });
        out->push_back({ sampleR + ".wav", WavMono(o.right) });
        index.push_back(std::string(m) + "\t" + seq + "\tframes=" + std::to_string(frames) + "\tloop=" +
                        (o.loop ? std::to_string(o.loop->first) + "-" + std::to_string(o.loop->second) : "none") +
                        "\tpeak=" + PyFixed(o.peakDb, 2) + "dBFS");
        o = TrackOut{};
    }
    std::string text;
    for (const std::string& line : index) {
        text += line + "\n";
    }
    // outside custom/music/ and custom/fonts/: audio_load.c loads every file there as a sequence or a font
    out->push_back({ "rf3music/index.txt", Text(text) });
}

} // namespace

bool BuildMusic(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress) {
    try {
        Build(pack, out, progress);
    } catch (const std::exception& e) {
        *error = e.what();
        out->clear();
        return false;
    }
    return true;
}

} // namespace rf3extract
