// Reel Fishing III mod (step 63b, docs/DESIGN.md "Step 63 design", RF3-MECHANICS 9): the mod's sound mixer.
// - Clips (.rf3c, tools/rf3_sound.py) are loaded from the archives on the game thread the first time they're played and
//   kept: mono s16, a playback rate (RF3's note pitch included), an optional loop start (the clip then loops to its
//   end until stopped), and RF3's own volume.
// - 16 channels, as RF3's sound manager: a sound started on a channel replaces the one playing there.
// - Step 63c-2: streams get one of 6 stream voices instead, as RF3's stream pool (0x0015D350): the first free one, or
//   when all play the one started first. So repeated streams overlap. Their volume is clamped to 1 (0x3FFF, RF3
//   0x0015F620).
// - SoH's audio thread calls RF3Sound_Mix after OoT's audio is mixed (OTRGlobals.cpp, OTRAudio_Thread): each voice is
//   played at SoH's 32 kHz (copied when the build baked it at 32 kHz; through a 24-tap sinc otherwise, step 63e-2)
//   and added to both sides, scaled by SoH's master volume (not its sound-effect
//   volume, step 63c) and Settings > Audio's "RF3 Sound Volume" (ReelFishing3.SoundVolumePct, step 63b-2), then clamped.
// The voices are shared between the threads under one mutex, held only while the audio thread mixes a buffer.
// Log, don't look: logs/rf3_records.txt gets "rf3-sound play|stream|stop|missing ...".

#include "RF3Sound.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/audio/AudioSettings.h"
#include "RF3DevTools.h"

namespace {

struct Clip {
    uint32_t rate = 0;
    int32_t loopStart = -1;
    float volume = 1.0f;
    std::vector<int16_t> samples;
};

struct Voice {
    std::shared_ptr<const Clip> clip;
    double pos = 0.0;
    float volume = 1.0f;
    float pitch = 1.0f;
    std::string name;
    uint64_t started = 0; // start order, for taking the oldest stream voice
    bool stream = false;
    int slot = 0;         // channel, or stream voice
    bool logged = false;  // step 63c-5a: its start is in the recording's log
};

// Step 63c-5a (DESIGN "63c-5 plan"): "Record RF3 mix" writes the mixer's own output (RF3's sounds only, at RF3's
// level: no master volume, no "RF3 Sound Volume") to logs/rf3_mix_<date>.wav, and each voice's start, stop, cut
// (replaced) and end with its sample position in that file to logs/rf3_mix_<date>.csv. tools/rf3_audiocmp.py reads
// them. Written on the audio thread, under sMutex.
struct MixRecording {
    std::FILE* wav = nullptr;
    std::FILE* csv = nullptr;
    uint64_t frames = 0; // stereo frames written
    uint32_t rate = 0;   // set by the first mix after the start (0: header not written yet)
    std::string path;
};

std::mutex sMutex; // guards sVoices, sStreams and sRec
Voice sVoices[RF3_SOUND_CHANNELS];
Voice sStreams[RF3_STREAM_VOICES];
uint64_t sStarts = 0;
MixRecording sRec;

// Step 63e-5 (DESIGN "63e-5"): a peak limiter on RF3's part of the output. Overlapping struggle splashes summed past
// 16 bits and hard-clipped. Where OoT's audio + RF3's would pass the ceiling, RF3's part alone is turned down just
// enough: the gain ramps down over the lookahead before the peak (within the buffer) and recovers over about 50 ms.
// Below the ceiling nothing changes, so the balance stays; OoT's audio is never changed. The "Record RF3 mix" dump
// stays at RF3's raw level.
constexpr float kLimitCeiling = 32767.0f * 0.97f;
constexpr int kLimitLookahead = 32;          // frames: 1 ms at 32 kHz
constexpr float kLimitRelease = 0.999375f;   // per frame: exp(-1 / (0.05 s x 32000))
float sLimitGain = 1.0f;                     // audio thread only, carried across buffers
std::atomic<uint32_t> sLimitReduced{ 0 };    // frames with the gain under 1 since the last report
std::atomic<uint32_t> sLimitClipped{ 0 };    // samples still clamped (OoT's audio alone past the ceiling)
std::atomic<int32_t> sLimitDeepest{ 1000 };  // lowest gain since the last report, x 1000
std::map<std::string, std::shared_ptr<const Clip>> sClips; // game thread only
std::set<std::string> sMissing;                            // logged once each
std::atomic<float> sGain{ 1.0f };                          // SFX volume x the mod's volume, set on the game thread
std::string sStatus = "idle";

void Log(const std::string& line) {
    std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out = RF3OpenLog(dir + "/rf3_records.txt"); // step 78b: nothing while the logs are off
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    out << stamp << ' ' << line << '\n';
}

std::shared_ptr<const Clip> LoadClip(const std::string& path) {
    auto it = sClips.find(path);
    if (it != sClips.end()) {
        return it->second;
    }
    std::shared_ptr<const Clip> result;
    auto ctx = Ship::Context::GetRawInstance();
    auto archives = (ctx && ctx->GetResourceManager()) ? ctx->GetResourceManager()->GetArchiveManager() : nullptr;
    auto file = archives ? archives->LoadFile(path) : nullptr;
    if (file != nullptr && file->Buffer != nullptr) {
        const char* data = file->Buffer->data() + file->BufferOffset;
        const size_t size = file->Buffer->size() - file->BufferOffset;
        uint32_t version = 0, rate = 0, count = 0;
        int32_t loop = -1;
        float volume = 1.0f;
        if (size >= 24 && std::memcmp(data, "RF3C", 4) == 0) {
            std::memcpy(&version, data + 4, 4);
            std::memcpy(&rate, data + 8, 4);
            std::memcpy(&loop, data + 12, 4);
            std::memcpy(&volume, data + 16, 4);
            std::memcpy(&count, data + 20, 4);
            if (version == 1 && rate > 0 && size >= 24 + static_cast<size_t>(count) * 2) {
                auto clip = std::make_shared<Clip>();
                clip->rate = rate;
                clip->loopStart = (loop >= 0 && static_cast<uint32_t>(loop) < count) ? loop : -1;
                clip->volume = volume;
                clip->samples.resize(count);
                std::memcpy(clip->samples.data(), data + 24, static_cast<size_t>(count) * 2);
                result = clip;
            }
        }
    }
    sClips[path] = result; // a missing clip isn't looked for again
    return result;
}

// One line of the recording's log; `at` is the stereo frame in the WAV. Called with sMutex held.
void RecordEvent(const Voice& v, const char* event, uint64_t at) {
    if (sRec.csv == nullptr || v.clip == nullptr) {
        return;
    }
    const Clip& c = *v.clip;
    const double rate = sRec.rate > 0 ? sRec.rate : 32000.0;
    const double duration = c.loopStart >= 0 ? -1.0 : c.samples.size() / (c.rate * static_cast<double>(v.pitch));
    std::fprintf(sRec.csv, "%llu,%.5f,%s,%s,%d,%s,%.4f,%.4f,%u,%zu,%d,%.4f,%.4f\n",
                 static_cast<unsigned long long>(at), at / rate, event, v.stream ? "stream" : "play", v.slot,
                 v.name.c_str(), v.volume, v.pitch, c.rate, c.samples.size(), c.loopStart, c.volume, duration);
}

void WriteWavHeader(std::FILE* f, uint32_t rate, uint32_t dataBytes) {
    const uint16_t channels = 2, bits = 16, align = channels * bits / 8, format = 1;
    const uint32_t byteRate = rate * align, riffSize = 36 + dataBytes, fmtSize = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&format, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
}

void UpdateGain() {
    // Step 63b-2: Settings > Audio's "RF3 Sound Volume" (percent; 100 = RF3's own levels). Step 63c (user: "decouple
    // the rf3 sounds from the sfx slider"): only the master volume and this one apply, not the sound effects volume.
    const float mod = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.SoundVolumePct"), 100) / 100.0f;
    sGain.store(mod, std::memory_order_relaxed);
}

// A stream (`stream`) takes a stream voice; anything else replaces `channel`'s sound. For a stream, `channel` is
// RF3's stream bank, only logged.
int32_t Start(int32_t channel, const std::string& path, const std::string& name, float volume, float pitch,
              bool stream) {
    if (!stream && (channel < 0 || channel >= RF3_SOUND_CHANNELS)) {
        return 0;
    }
    UpdateGain();
    auto clip = LoadClip(path);
    if (clip == nullptr) {
        if (sMissing.insert(path).second) {
            Log("rf3-sound missing " + path + " (run tools/rf3_sound.py build)");
        }
        return 0;
    }
    if (stream) {
        volume = std::clamp(volume, 0.0f, 1.0f);
    }
    int slot = channel;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        Voice* v = &sVoices[channel < 0 || channel >= RF3_SOUND_CHANNELS ? 0 : channel];
        if (stream) {
            slot = -1;
            for (int i = 0; i < RF3_STREAM_VOICES && slot < 0; i++) {
                if (sStreams[i].clip == nullptr) {
                    slot = i;
                }
            }
            if (slot < 0) { // all busy: the one started first
                slot = 0;
                for (int i = 1; i < RF3_STREAM_VOICES; i++) {
                    if (sStreams[i].started < sStreams[slot].started) {
                        slot = i;
                    }
                }
            }
            v = &sStreams[slot];
        }
        if (v->clip != nullptr) {
            RecordEvent(*v, "cut", sRec.frames); // step 63c-5a: replaced while playing
        }
        v->clip = clip;
        v->pos = 0.0;
        v->volume = volume;
        v->pitch = pitch;
        v->name = name;
        v->started = ++sStarts;
        v->stream = stream;
        v->slot = stream ? slot : channel;
        v->logged = false; // logged when first mixed
    }
    // Step 63c: the fight repeats some sounds several times a second; one line per sound per second is enough.
    static std::map<std::string, std::time_t> lastLogged;
    const std::time_t now = std::time(nullptr);
    if (lastLogged[name] != now) {
        lastLogged[name] = now;
        char line[200];
        std::snprintf(line, sizeof(line),
                      "rf3-sound %s %s %s=%d%s volume=%.2f pitch=%.2f rate=%u frames=%zu loop=%d",
                      stream ? "stream" : "play", name.c_str(), stream ? "bank" : "channel", channel,
                      stream ? (" voice=" + std::to_string(slot)).c_str() : "", volume, pitch, clip->rate,
                      clip->samples.size(), clip->loopStart);
        Log(line);
    }
    sStatus = stream ? "stream " + name + " on stream voice " + std::to_string(slot)
                     : "play " + name + " on channel " + std::to_string(channel);
    return 1;
}

// Step 63e-2: a windowed-sinc interpolator for clips not at the output rate (the streams, whose pitch RF3 sets at
// run time; tools/rf3_sound.py bakes every other clip at 32 kHz). 24 taps, 256 phases, Blackman window, cutoff 95 %
// of the source's Nyquist, each phase normalized to unity gain. It replaces linear interpolation, which smeared
// the strain stream's treble (DESIGN "63e-2").
constexpr int kSincHalf = 12;
constexpr int kSincPhases = 256;

const std::vector<float>& SincTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(static_cast<size_t>(kSincPhases) * 2 * kSincHalf);
        const double fc = 0.5 * 0.95; // cycles per source sample
        const double pi = 3.14159265358979323846;
        for (int p = 0; p < kSincPhases; p++) {
            const double frac = static_cast<double>(p) / kSincPhases;
            double sum = 0.0;
            float* row = &t[static_cast<size_t>(p) * 2 * kSincHalf];
            for (int k = 0; k < 2 * kSincHalf; k++) {
                const double x = (k - kSincHalf + 1) - frac; // source offset from the output point
                const double sinc = x == 0.0 ? 2.0 * fc : std::sin(2.0 * pi * fc * x) / (pi * x);
                const double w = 0.42 + 0.5 * std::cos(pi * x / kSincHalf) + 0.08 * std::cos(2.0 * pi * x / kSincHalf);
                row[k] = static_cast<float>(std::abs(x) < kSincHalf ? sinc * w : 0.0);
                sum += row[k];
            }
            for (int k = 0; k < 2 * kSincHalf; k++) {
                row[k] = static_cast<float>(row[k] / sum);
            }
        }
        return t;
    }();
    return table;
}

// Source sample `i` of a clip: silence before its start and after its end, its loop repeated past the end.
float At(const Clip& c, int64_t i) {
    const int64_t n = static_cast<int64_t>(c.samples.size());
    if (i < 0) {
        return 0.0f;
    }
    if (i >= n) {
        if (c.loopStart < 0) {
            return 0.0f;
        }
        i = c.loopStart + (i - n) % (n - c.loopStart);
    }
    return c.samples[static_cast<size_t>(i)];
}

// Adds one voice, at RF3's level, to `acc` (interleaved stereo); clears it when a clip without a loop ends.
void MixVoice(Voice& v, int32_t* acc, uint32_t frames, uint32_t rate) {
    if (!v.logged) {
        RecordEvent(v, v.pos > 0.0 ? "playing" : "start", sRec.frames);
        v.logged = sRec.csv != nullptr;
    }
    const Clip& c = *v.clip;
    const double step = static_cast<double>(c.rate) * v.pitch / rate;
    const bool direct = step == 1.0; // a clip baked at the output rate, at its own pitch: copied as is
    const std::vector<float>& table = SincTable();
    const float amp = v.volume * c.volume;
    const size_t n = c.samples.size();
    for (uint32_t f = 0; f < frames; f++) {
        if (v.pos >= static_cast<double>(n)) {
            if (c.loopStart < 0) {
                RecordEvent(v, "end", sRec.frames + f);
                v = Voice();
                return;
            }
            v.pos = c.loopStart + std::fmod(v.pos - n, static_cast<double>(n - c.loopStart));
        }
        const int64_t i = static_cast<int64_t>(v.pos);
        float value;
        if (direct) {
            value = c.samples[static_cast<size_t>(i)];
        } else {
            const int phase = static_cast<int>((v.pos - static_cast<double>(i)) * kSincPhases);
            const float* row = &table[static_cast<size_t>(phase) * 2 * kSincHalf];
            value = 0.0f;
            for (int k = 0; k < 2 * kSincHalf; k++) {
                value += At(c, i + k - kSincHalf + 1) * row[k];
            }
        }
        const int32_t s = static_cast<int32_t>(value * amp);
        acc[f * 2] += s;
        acc[f * 2 + 1] += s;
        v.pos += step;
    }
}

} // namespace

extern "C" int32_t RF3Sound_Play(int32_t channel, const char* bank, int32_t number, float volume, float pitch) {
    char path[64], name[32];
    std::snprintf(path, sizeof(path), "rf3sounds/%s/seq%02d.rf3c", bank, number);
    std::snprintf(name, sizeof(name), "%s/%d", bank, number);
    return Start(channel, path, name, volume, pitch, false);
}

extern "C" int32_t RF3Sound_PlayStream(int32_t bank, int32_t id, float volume, float pitch) {
    char path[64], name[32];
    std::snprintf(path, sizeof(path), "rf3sounds/fis/spm%02d.rf3c", id);
    std::snprintf(name, sizeof(name), "stream/%d", id);
    return Start(bank, path, name, volume, pitch, true);
}

extern "C" int32_t RF3Sound_PlayStreamSpu(int32_t bank, int32_t id, float volume, int32_t spuPitch) {
    char path[64], name[32];
    std::snprintf(path, sizeof(path), "rf3sounds/fis/spm%02d.rf3c", id);
    std::snprintf(name, sizeof(name), "stream/%d", id);
    auto clip = LoadClip(path);
    const float pitch = (clip != nullptr && clip->rate > 0) ? (spuPitch * 48000.0f / 4096.0f) / clip->rate : 1.0f;
    return Start(bank, path, name, volume, pitch, true);
}

extern "C" void RF3Sound_Stop(int32_t channel) {
    if (channel < 0 || channel >= RF3_SOUND_CHANNELS) {
        return;
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sVoices[channel].clip == nullptr) {
            return;
        }
        name = sVoices[channel].name;
        RecordEvent(sVoices[channel], sVoices[channel].logged ? "stop" : "dropped", sRec.frames);
        sVoices[channel] = Voice();
    }
    Log("rf3-sound stop " + name + " channel=" + std::to_string(channel));
}

extern "C" void RF3Sound_StopAll(void) {
    std::lock_guard<std::mutex> lock(sMutex);
    for (Voice& v : sVoices) {
        RecordEvent(v, v.logged ? "stop" : "dropped", sRec.frames);
        v = Voice();
    }
    for (Voice& v : sStreams) {
        RecordEvent(v, v.logged ? "stop" : "dropped", sRec.frames);
        v = Voice();
    }
}

extern "C" int32_t RF3Sound_IsPlaying(int32_t channel) {
    if (channel < 0 || channel >= RF3_SOUND_CHANNELS) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    return sVoices[channel].clip != nullptr ? 1 : 0;
}

extern "C" void RF3Sound_Mix(int16_t* stereo, uint32_t frames, uint32_t rate) {
    const float gain = AudioSettings_GetMasterVolume() * sGain.load(std::memory_order_relaxed);
    static std::vector<int32_t> acc; // audio thread only
    acc.assign(static_cast<size_t>(frames) * 2, 0);
    std::lock_guard<std::mutex> lock(sMutex);
    if (sRec.wav != nullptr && sRec.rate == 0) {
        sRec.rate = rate;
        WriteWavHeader(sRec.wav, rate, 0); // sizes are filled in when the recording stops
    }
    for (Voice& v : sVoices) {
        if (v.clip != nullptr) {
            MixVoice(v, acc.data(), frames, rate);
        }
    }
    for (Voice& v : sStreams) {
        if (v.clip != nullptr) {
            MixVoice(v, acc.data(), frames, rate);
        }
    }
    // Step 63e-5: the peak limiter. need[f]: the most of RF3's part frame f can take without passing the ceiling.
    static std::vector<float> need;
    need.assign(frames, 1.0f);
    for (uint32_t f = 0; f < frames; f++) {
        for (int side = 0; side < 2; side++) {
            const float o = stereo[f * 2 + side];
            const float r = acc[f * 2 + side] * gain;
            const float s = o + r;
            if (std::fabs(s) > kLimitCeiling && r != 0.0f) {
                const float g = ((s > 0.0f ? kLimitCeiling : -kLimitCeiling) - o) / r;
                need[f] = std::min(need[f], std::clamp(g, 0.0f, 1.0f));
            }
        }
    }
    uint32_t reduced = 0, clipped = 0;
    float deepest = 1.0f;
    for (uint32_t f = 0; f < frames; f++) {
        // A peak at f + d (d <= lookahead) allows at most need + (1 - need) x d / lookahead now: a 1 ms ramp down.
        float target = 1.0f;
        for (uint32_t k = f; k < frames && k <= f + kLimitLookahead; k++) {
            if (need[k] < 1.0f) {
                const float d = static_cast<float>(k - f) / kLimitLookahead;
                target = std::min(target, need[k] + (1.0f - need[k]) * d);
            }
        }
        sLimitGain = target < sLimitGain ? target : target - (target - sLimitGain) * kLimitRelease;
        if (sLimitGain < 0.999f) {
            reduced++;
            deepest = std::min(deepest, sLimitGain);
        }
        for (int side = 0; side < 2; side++) {
            const int32_t s = static_cast<int32_t>(stereo[f * 2 + side]) +
                              static_cast<int32_t>(acc[f * 2 + side] * gain * sLimitGain);
            if (s > 32767 || s < -32768) {
                clipped++;
            }
            stereo[f * 2 + side] = static_cast<int16_t>(std::clamp(s, -32768, 32767));
        }
    }
    if (reduced > 0) {
        sLimitReduced.fetch_add(reduced, std::memory_order_relaxed);
        int32_t was = sLimitDeepest.load(std::memory_order_relaxed);
        const int32_t now = static_cast<int32_t>(deepest * 1000.0f);
        while (now < was && !sLimitDeepest.compare_exchange_weak(was, now, std::memory_order_relaxed)) {
        }
    }
    if (clipped > 0) {
        sLimitClipped.fetch_add(clipped, std::memory_order_relaxed);
    }
    if (sRec.wav != nullptr) {
        static std::vector<int16_t> out;
        out.resize(acc.size());
        for (size_t i = 0; i < acc.size(); i++) {
            out[i] = static_cast<int16_t>(std::clamp(acc[i], -32768, 32767));
        }
        std::fwrite(out.data(), sizeof(int16_t), out.size(), sRec.wav);
        sRec.frames += frames;
    }
}

extern "C" void RF3Sound_LimiterReport(void) {
    const uint32_t reduced = sLimitReduced.exchange(0, std::memory_order_relaxed);
    const uint32_t clipped = sLimitClipped.exchange(0, std::memory_order_relaxed);
    const int32_t deepest = sLimitDeepest.exchange(1000, std::memory_order_relaxed);
    if (reduced == 0 && clipped == 0) {
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "rf3-sound limiter: %u frames turned down, deepest %.1f dB, %u samples still clipped",
                  reduced, deepest > 0 ? 20.0 * std::log10(deepest / 1000.0) : -99.0, clipped);
    Log(line);
}

extern "C" void RF3Sound_SetRecording(int32_t on) {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (on && sRec.wav == nullptr) {
            const std::string dir = Ship::Context::GetPathRelativeToAppDirectory("logs");
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            char stamp[32];
            std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
            sRec = MixRecording();
            sRec.path = dir + "/rf3_mix_" + stamp;
            sRec.wav = std::fopen((sRec.path + ".wav").c_str(), "wb");
            sRec.csv = std::fopen((sRec.path + ".csv").c_str(), "w");
            if (sRec.wav == nullptr || sRec.csv == nullptr) {
                if (sRec.wav != nullptr) {
                    std::fclose(sRec.wav);
                }
                if (sRec.csv != nullptr) {
                    std::fclose(sRec.csv);
                }
                line = "rf3-sound recording failed to open " + sRec.path;
                sRec = MixRecording();
            } else {
                std::fprintf(sRec.csv, "sample,time,event,kind,slot,name,volume,pitch,clipRate,clipFrames,loopStart,"
                                       "clipVolume,duration\n");
                for (Voice& v : sVoices) {
                    v.logged = false; // already-playing voices are logged as "playing" on the next mix
                }
                for (Voice& v : sStreams) {
                    v.logged = false;
                }
                line = "rf3-sound recording started " + sRec.path + ".wav";
            }
        } else if (!on && sRec.wav != nullptr) {
            const uint32_t bytes = static_cast<uint32_t>(sRec.frames * 4);
            std::fseek(sRec.wav, 0, SEEK_SET);
            WriteWavHeader(sRec.wav, sRec.rate > 0 ? sRec.rate : 32000, bytes);
            std::fclose(sRec.wav);
            std::fclose(sRec.csv);
            char text[64];
            std::snprintf(text, sizeof(text), " (%llu frames at %u Hz)",
                          static_cast<unsigned long long>(sRec.frames), sRec.rate);
            line = "rf3-sound recording stopped " + sRec.path + ".wav" + text;
            sRec = MixRecording();
        }
    }
    if (!line.empty()) {
        Log(line);
        sStatus = line.substr(10); // without "rf3-sound "
    }
}

extern "C" int32_t RF3Sound_IsRecording(void) {
    std::lock_guard<std::mutex> lock(sMutex);
    return sRec.wav != nullptr ? 1 : 0;
}

extern "C" const char* RF3Sound_RecordingStatus(void) {
    static std::string text;
    std::lock_guard<std::mutex> lock(sMutex);
    if (sRec.wav == nullptr) {
        text = "not recording";
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f s", sRec.rate > 0 ? sRec.frames / static_cast<double>(sRec.rate) : 0.0);
        text = "recording " + sRec.path + ".wav, " + buf;
    }
    return text.c_str();
}

extern "C" const char* RF3Sound_Status(void) {
    return sStatus.c_str();
}
