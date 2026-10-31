// Reel Fishing III mod (step 76b, P2): see RF3ExtractSound.h. A port of tools/rf3_sound.py's "build" path (read_hd,
// resolve, parse_sequence, read_sq, read_shd, decode_adpcm, sinc_kernels, resample, to_pcm, resample_loop,
// unroll_loop, sequence_notes, render, sound_clip, clip_bytes, cmd_build); the formats are in the project's
// RF3-MECHANICS.md section 9. Every float operation is in the Python tool's order, and sums use Python 3.13's sum()
// (Neumaier), so the clips come out byte for byte the same (tools/rf3_extract_check.py checks it).

#include "RF3ExtractSound.h"

#include "RF3ExtractAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace rf3extract {

namespace {

constexpr int kClipRate = 32000;
constexpr int kSpuRate = 48000;
constexpr double kMaxRenderSeconds = 6.0;
constexpr double kCore0Gain = 0.5;

// ---------------------------------------------------------------- .hd

struct Vag {
    uint32_t offset;
    uint16_t rate;
    uint8_t loop;
};
struct Smpl {
    int vag, base, detune, pan, volume;
};
struct Sset {
    std::vector<int> smpls;
};
struct Split {
    int sset, low, high, volume, pan, transpose, detune;
};
struct Prog {
    int volume, pan, transpose, detune;
    std::vector<Split> splits;
};
struct Hd {
    std::vector<std::optional<Vag>> vags;
    std::vector<std::optional<Smpl>> smpls;
    std::vector<std::optional<Sset>> ssets;
    std::vector<std::optional<Prog>> progs;
};

bool ReadHd(const std::vector<uint8_t>& d, Hd* hd) {
    const int64_t head = Find(d, "IECSdaeH", 0);
    if (head < 0) {
        return false;
    }
    const size_t h = static_cast<size_t>(head);
    // "<8sIIIIIII": the tag, a word, hd size, bd size, then the four chunk offsets
    const uint32_t prog = U32(d, h + 20), sset = U32(d, h + 24), smpl = U32(d, h + 28), vagi = U32(d, h + 32);
    for (const auto& e : ChunkTable(d, vagi)) {
        hd->vags.push_back(e ? std::optional<Vag>(Vag{ U32(d, *e), U16(d, *e + 4), d[*e + 6] }) : std::nullopt);
    }
    for (const auto& e : ChunkTable(d, smpl)) {
        if (!e) {
            hd->smpls.push_back(std::nullopt);
            continue;
        }
        hd->smpls.push_back(Smpl{ U16(d, *e), d[*e + 11], static_cast<int8_t>(d[*e + 12]), d[*e + 13], d[*e + 16] });
    }
    for (const auto& e : ChunkTable(d, sset)) {
        if (!e) {
            hd->ssets.push_back(std::nullopt);
            continue;
        }
        Sset s;
        const int n = d[*e + 3];
        for (int k = 0; k < n; k++) {
            s.smpls.push_back(U16(d, *e + 4 + 2 * k));
        }
        hd->ssets.push_back(s);
    }
    for (const auto& e : ChunkTable(d, prog)) {
        if (!e) {
            hd->progs.push_back(std::nullopt);
            continue;
        }
        Prog p;
        const uint32_t splitRel = U32(d, *e);
        const int n = d[*e + 4], size = d[*e + 5];
        p.volume = d[*e + 6];
        p.pan = d[*e + 7];
        p.transpose = static_cast<int8_t>(d[*e + 8]);
        p.detune = static_cast<int8_t>(d[*e + 9]);
        for (int k = 0; k < n; k++) {
            const size_t s = *e + splitRel + static_cast<size_t>(k) * size;
            p.splits.push_back(Split{ U16(d, s), d[s + 2], d[s + 4], d[s + 16], d[s + 17], static_cast<int8_t>(d[s + 18]),
                                      static_cast<int8_t>(d[s + 19]) });
        }
        hd->progs.push_back(p);
    }
    return true;
}

struct Resolved {
    int vag;
    double rate;
    double vol;
    bool loop;
};

std::optional<Resolved> Resolve(const Hd& hd, int program, int key, int velocity) {
    if (program >= static_cast<int>(hd.progs.size()) || !hd.progs[program]) {
        return std::nullopt;
    }
    const Prog& p = *hd.progs[program];
    for (const Split& sp : p.splits) {
        if (!(sp.low <= key && key <= sp.high)) {
            continue;
        }
        if (sp.sset >= static_cast<int>(hd.ssets.size()) || !hd.ssets[sp.sset]) {
            continue;
        }
        for (int si : hd.ssets[sp.sset]->smpls) {
            if (si >= static_cast<int>(hd.smpls.size()) || !hd.smpls[si]) {
                continue;
            }
            const Smpl& sm = *hd.smpls[si];
            if (sm.vag >= static_cast<int>(hd.vags.size()) || !hd.vags[sm.vag]) {
                continue;
            }
            const Vag& v = *hd.vags[sm.vag];
            const double semis = static_cast<double>(key - sm.base + p.transpose + sp.transpose) +
                                 static_cast<double>(sm.detune + p.detune + sp.detune) / 128.0;
            const double vol =
                (p.volume / 127.0) * (sp.volume / 127.0) * (sm.volume / 127.0) * (velocity / 127.0);
            return Resolved{ sm.vag, v.rate * std::pow(2.0, semis / 12.0), vol, v.loop != 0 };
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- .sq

enum class Ev { NoteOn, NoteOff, Program, Cc, Tempo, End };
struct Event {
    int64_t tick;
    Ev kind;
    int a0 = 0, a1 = 0;
};
struct Sequence {
    int resolution = 0;
    std::vector<Event> events;
};

Sequence ParseSequence(const std::vector<uint8_t>& d, size_t start, size_t end) {
    Sequence seq;
    const uint32_t dataRel = U32(d, start);
    seq.resolution = U16(d, start + 4);
    size_t pos = start + dataRel;
    int64_t tick = 0;
    int status = -1;
    bool needDelta = true;
    while (pos < end) {
        if (needDelta) {
            tick += ReadVlq(d, &pos);
        }
        needDelta = true;
        const uint8_t b = d[pos];
        if (b >= 0x80) {
            status = b;
            pos++;
        }
        if (status < 0) {
            break;
        }
        const int hi = status & 0xF0;
        if (status == 0xFF) {
            const int kind = d[pos];
            pos++;
            const uint32_t length = ReadVlq(d, &pos);
            const size_t payload = pos;
            pos += length;
            if (kind == 0x2F) {
                seq.events.push_back({ tick, Ev::End });
                break;
            }
            if (kind == 0x51 && length == 3) {
                seq.events.push_back({ tick, Ev::Tempo, (d[payload] << 16) | (d[payload + 1] << 8) | d[payload + 2] });
            }
            continue;
        }
        const int nargs = (hi == 0x80 || hi == 0xC0 || hi == 0xD0) ? 1 : 2;
        int args[2] = { 0, 0 };
        int got = 0;
        for (int k = 0; k < nargs && pos + k < d.size(); k++) {
            args[k] = d[pos + k];
            got++;
        }
        pos += nargs;
        if (got > 0 && (args[got - 1] & 0x80)) { // Sony: no delta time after this event
            args[got - 1] &= 0x7F;
            needDelta = false;
        }
        if (hi == 0x90) {
            seq.events.push_back({ tick, args[1] ? Ev::NoteOn : Ev::NoteOff, args[0], args[1] });
        } else if (hi == 0x80) {
            seq.events.push_back({ tick, Ev::NoteOff, args[0] });
        } else if (hi == 0xC0) {
            seq.events.push_back({ tick, Ev::Program, args[0] });
        } else if (hi == 0xB0) {
            seq.events.push_back({ tick, Ev::Cc, args[0], args[1] });
        }
    }
    return seq;
}

std::vector<std::optional<Sequence>> ReadSq(const std::vector<uint8_t>& d) {
    std::vector<std::optional<Sequence>> out;
    const int64_t midi = Find(d, "IECSidiM", 0);
    if (midi < 0) {
        return out;
    }
    const auto entries = ChunkTable(d, static_cast<size_t>(midi));
    std::vector<size_t> starts;
    for (const auto& e : entries) {
        if (e) {
            starts.push_back(*e);
        }
    }
    std::sort(starts.begin(), starts.end());
    starts.push_back(d.size());
    for (const auto& e : entries) {
        if (!e) {
            out.push_back(std::nullopt);
            continue;
        }
        const size_t end = starts[std::find(starts.begin(), starts.end(), *e) - starts.begin() + 1];
        out.push_back(ParseSequence(d, *e, end));
    }
    return out;
}

// ---------------------------------------------------------------- .shd / ADPCM

struct ShdEntry {
    int id, pitch;
    uint32_t offset, size;
};

std::vector<ShdEntry> ReadShd(const std::vector<uint8_t>& d) {
    std::vector<ShdEntry> out;
    if (d.size() < 16 || std::memcmp(d.data(), "SPMH", 4) != 0) {
        return out;
    }
    const uint32_t count = U32(d, 8);
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = 16 + i * 16;
        if (e + 16 > d.size()) {
            break;
        }
        out.push_back({ U16(d, e), U16(d, e + 2), U32(d, e + 4), U32(d, e + 8) & 0xFFFFFF });
    }
    return out;
}

// to_pcm: floats as s16, scaled down by the gain (>= 1) needed so nothing clips.
std::vector<int> ToPcm(const std::vector<double>& values, double* gain) {
    double peak = 0.0;
    for (double v : values) {
        peak = std::max(peak, std::fabs(v));
    }
    *gain = std::max(1.0, peak / 32767.0);
    std::vector<int> out;
    out.reserve(values.size());
    for (double v : values) {
        out.push_back(static_cast<int>(PyRoundInt(v / *gain)));
    }
    return out;
}

struct Note {
    double t0, t1;
    int prog, key, vel, vol;
};

std::vector<Note> SequenceNotes(const Sequence& seq) {
    int64_t tempo = 500000;
    int program = 0, ccVolume = 127;
    std::vector<Note> notes;
    std::vector<std::pair<int, Note>> active; // Python dict order: insertion, an update keeps the place
    double seconds = 0.0;
    int64_t lastTick = 0;
    for (const Event& e : seq.events) {
        seconds += static_cast<double>((e.tick - lastTick) * tempo) / 1e6 / seq.resolution;
        lastTick = e.tick;
        if (e.kind == Ev::Tempo) {
            tempo = e.a0;
        } else if (e.kind == Ev::Program) {
            program = e.a0;
        } else if (e.kind == Ev::Cc && e.a0 == 7) {
            ccVolume = e.a1;
        } else if (e.kind == Ev::NoteOn) {
            Note n{ seconds, 0, program, e.a0, e.a1, ccVolume };
            auto it = std::find_if(active.begin(), active.end(), [&](const auto& p) { return p.first == e.a0; });
            if (it != active.end()) {
                it->second = n;
            } else {
                active.emplace_back(e.a0, n);
            }
        } else if (e.kind == Ev::NoteOff || e.kind == Ev::End) {
            std::vector<int> keys;
            if (e.kind == Ev::NoteOff) {
                keys.push_back(e.a0);
            } else {
                for (const auto& p : active) {
                    keys.push_back(p.first);
                }
            }
            for (int k : keys) {
                auto it = std::find_if(active.begin(), active.end(), [&](const auto& p) { return p.first == k; });
                if (it != active.end()) {
                    Note n = it->second;
                    active.erase(it);
                    n.t1 = seconds;
                    notes.push_back(n);
                }
            }
        }
    }
    return notes;
}

struct Clip {
    double rate;
    std::optional<size_t> loopStart;
    double volume;
    std::vector<int> pcm;
    std::string how;
};

class Bank {
  public:
    Bank(const Hd& hd, const std::vector<uint8_t>& bd, Resampler& rs) : mHd(hd), mBd(bd), mRs(rs) {
    }

    const Decoded& Sample(int vag) {
        auto it = mDecoded.find(vag);
        if (it == mDecoded.end()) {
            it = mDecoded.emplace(vag, DecodeAdpcm(mBd, mHd.vags[vag]->offset)).first;
        }
        return it->second;
    }

    // render: a sound mixed to rateOut mono [assumed: no envelopes].
    std::vector<int> Render(const Sequence& seq, int rateOut, double* gain) {
        std::vector<double> out(static_cast<size_t>(rateOut * kMaxRenderSeconds), 0.0);
        for (const Note& n : SequenceNotes(seq)) {
            const auto r = Resolve(mHd, n.prog, n.key, n.vel);
            if (!r) {
                continue;
            }
            const Decoded& dec = Sample(r->vag);
            const double amp = r->vol * (n.vol / 127.0);
            const double step = r->rate / rateOut;
            const int64_t i0 = static_cast<int64_t>(n.t0 * rateOut);
            int64_t length;
            std::vector<int> unrolled;
            const std::vector<int>* source = &dec.samples;
            if (r->loop && dec.loopStart) {
                length = static_cast<int64_t>(std::min(n.t1 - n.t0, kMaxRenderSeconds) * rateOut);
                unrolled = UnrollLoop(dec.samples, dec.loopStart, static_cast<size_t>(static_cast<int64_t>(length * step) + 2));
                source = &unrolled;
            } else {
                length = static_cast<int64_t>(dec.samples.size() / step);
            }
            length = std::min<int64_t>(length, static_cast<int64_t>(out.size()) - i0);
            if (length > 0) {
                const std::vector<double> v = mRs.Resample(*source, r->rate, rateOut, length);
                for (int64_t k = 0; k < length; k++) {
                    out[i0 + k] += v[k] * amp;
                }
            }
        }
        size_t last = 0;
        for (size_t i = 0; i < out.size(); i++) {
            if (std::fabs(out[i]) > 1) {
                last = i;
            }
        }
        out.resize(last + 1);
        return ToPcm(out, gain);
    }

    // sound_clip: one sound number as a clip.
    std::optional<Clip> SoundClip(const Sequence& seq) {
        const std::vector<Note> notes = SequenceNotes(seq);
        char how[200];
        if (notes.size() == 1) {
            const Note& n = notes[0];
            const auto r = Resolve(mHd, n.prog, n.key, n.vel);
            if (!r) {
                return std::nullopt;
            }
            const Decoded& dec = Sample(r->vag);
            double gain = 1.0;
            if (r->loop && dec.loopStart) {
                // resample_loop (step 63e-2): baked at 32 kHz, still looping seamlessly
                const size_t body = dec.samples.size() - *dec.loopStart;
                const int64_t lOut = std::max<int64_t>(1, PyRoundInt(static_cast<double>(body) * kClipRate / r->rate));
                const double ratio = static_cast<double>(body) / static_cast<double>(lOut);
                const int half = mRs.Kernels(ratio).half;
                const int64_t startOut = static_cast<int64_t>(std::ceil((*dec.loopStart + half) / ratio));
                const int64_t total = startOut + lOut;
                const std::vector<int> source = UnrollLoop(
                    dec.samples, dec.loopStart, static_cast<size_t>(static_cast<int64_t>(total * ratio) + 2 * half + 2));
                const std::vector<int> pcm = ToPcm(mRs.Resample(source, ratio * kClipRate, kClipRate, total), &gain);
                std::snprintf(how, sizeof(how), "vag%02d resampled from %s Hz, loop %zu -> %lld", r->vag,
                              PyFixed(r->rate, 0).c_str(), body, static_cast<long long>(pcm.size() - startOut));
                return Clip{ static_cast<double>(kClipRate), static_cast<size_t>(startOut), r->vol * n.vol / 127 * gain,
                             pcm, how };
            }
            const std::vector<int> pcm = ToPcm(mRs.Resample(dec.samples, r->rate, kClipRate), &gain);
            std::snprintf(how, sizeof(how), "vag%02d resampled from %s Hz", r->vag, PyFixed(r->rate, 0).c_str());
            return Clip{ static_cast<double>(kClipRate), std::nullopt, r->vol * n.vol / 127 * gain, pcm, how };
        }
        if (!notes.empty()) {
            double gain = 1.0;
            std::vector<int> pcm = Render(seq, kClipRate, &gain);
            std::snprintf(how, sizeof(how), "%zu notes mixed", notes.size());
            return Clip{ static_cast<double>(kClipRate), std::nullopt, gain, pcm, how };
        }
        return std::nullopt;
    }

  private:
    const Hd& mHd;
    const std::vector<uint8_t>& mBd;
    Resampler& mRs;
    std::map<int, Decoded> mDecoded;
};

// clip_bytes: "RF3C", version 1, rate, loop start (-1 none), f32 volume, count, s16 samples.
std::vector<uint8_t> ClipBytes(double rate, std::optional<size_t> loopStart, double volume, const std::vector<int>& samples) {
    std::vector<uint8_t> out = { 'R', 'F', '3', 'C' };
    auto put32 = [&out](uint32_t v) {
        for (int k = 0; k < 4; k++) {
            out.push_back(static_cast<uint8_t>(v >> (8 * k)));
        }
    };
    put32(1);
    put32(static_cast<uint32_t>(PyRoundInt(rate)));
    put32(loopStart ? static_cast<uint32_t>(*loopStart) : 0xFFFFFFFFu);
    const float vf = static_cast<float>(volume);
    uint32_t vu;
    std::memcpy(&vu, &vf, 4);
    put32(vu);
    put32(static_cast<uint32_t>(samples.size()));
    for (int s : samples) {
        const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(s));
        out.push_back(static_cast<uint8_t>(u & 0xFF));
        out.push_back(static_cast<uint8_t>(u >> 8));
    }
    return out;
}

} // namespace

bool BuildSound(rf3disc::Pack& pack, std::vector<Entry>* out, std::string* error, const ProgressFn& progress) {
    out->clear();
    Resampler rs;
    std::vector<std::string> index;
    const char* const banks[2] = { "fis", "sys" };
    for (int b = 0; b < 2; b++) {
        const std::string name = banks[b];
        const std::string prefix = "sound/se/" + name + ".";
        std::vector<uint8_t> hdData, bd, sq;
        if (!pack.Read(prefix + "hd", &hdData) || !pack.Read(prefix + "bd", &bd) || !pack.Read(prefix + "sq", &sq)) {
            *error = "sound bank " + name + " is missing on the disc";
            return false;
        }
        Hd hd;
        if (!ReadHd(hdData, &hd)) {
            *error = "sound bank " + name + ": no IECS head";
            return false;
        }
        Bank bank(hd, bd, rs);
        const auto seqs = ReadSq(sq);
        for (size_t n = 0; n < seqs.size(); n++) {
            progress("Sounds: " + name + " " + std::to_string(n),
                     (b + 0.9f * static_cast<float>(n) / std::max<size_t>(1, seqs.size())) / 2.0f);
            if (!seqs[n]) {
                continue;
            }
            std::optional<Clip> clip = bank.SoundClip(*seqs[n]);
            if (!clip) {
                continue;
            }
            if (name == "fis" && n >= 28 && n < 38) { // step 63e-3: RF3 plays the reel family on SPU2 core 0
                clip->volume *= kCore0Gain;
                clip->how += ", core 0 (x0.5)";
            }
            char path[64];
            std::snprintf(path, sizeof(path), "rf3sounds/%s/seq%02zu.rf3c", name.c_str(), n);
            out->push_back({ path, ClipBytes(clip->rate, clip->loopStart, clip->volume, clip->pcm) });
            index.push_back(std::string(path) + " rate=" + std::to_string(PyRoundInt(clip->rate)) +
                            " frames=" + std::to_string(clip->pcm.size()) +
                            " loop=" + (clip->loopStart ? std::to_string(*clip->loopStart) : std::string("None")) +
                            " vol=" + PyFixed(clip->volume, 2) + " " + clip->how);
        }
        std::vector<uint8_t> shd, spm;
        if (pack.Read(prefix + "shd", &shd) && pack.Read(prefix + "spm", &spm)) {
            for (const ShdEntry& e : ReadShd(shd)) {
                const Decoded dec = DecodeAdpcm(spm, e.offset, static_cast<size_t>(e.offset) + e.size);
                const double rate = static_cast<double>(static_cast<int64_t>(e.pitch) * kSpuRate) / 4096;
                char path[64];
                std::snprintf(path, sizeof(path), "rf3sounds/%s/spm%02d.rf3c", name.c_str(), e.id);
                out->push_back({ path, ClipBytes(rate, std::nullopt, 1.0, dec.samples) });
                index.push_back(std::string(path) + " rate=" + std::to_string(PyRoundInt(rate)) +
                                " frames=" + std::to_string(dec.samples.size()) + " stream");
            }
        }
    }
    std::string text;
    for (const std::string& line : index) {
        text += line + "\n";
    }
    out->push_back({ "rf3sounds/index.txt", std::vector<uint8_t>(text.begin(), text.end()) });
    return true;
}

} // namespace rf3extract
