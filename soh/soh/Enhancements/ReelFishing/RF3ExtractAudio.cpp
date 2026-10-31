// Reel Fishing III mod (step 76b): see RF3ExtractAudio.h.

#include "RF3ExtractAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "RF3ExtractCommon.h"

namespace rf3extract {

namespace {

constexpr int kSincHalf = 16;
constexpr int kSincPhases = 256;
const int kAdpcmCoef[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };
constexpr double kPi = 3.141592653589793; // math.pi

} // namespace

// Python's format(x, ".Nf"): the exact value rounded half to even.
std::string PyFixed(double x, int digits) {
    char buf[64];
    const double r = PyRound(x, digits);
    std::snprintf(buf, sizeof(buf), "%.*f", digits, r);
    std::string s(buf);
    if (s == "-0" || s.rfind("-0.", 0) == 0) {
        bool zero = s.find_first_not_of("-0.") == std::string::npos;
        if (zero && !std::signbit(x)) {
            s.erase(0, 1);
        }
    }
    return s;
}

std::vector<std::optional<size_t>> ChunkTable(const std::vector<uint8_t>& d, size_t off) {
    const uint32_t maxIndex = U32(d, off + 12);
    std::vector<std::optional<size_t>> out;
    for (uint32_t i = 0; i <= maxIndex; i++) {
        const uint32_t rel = U32(d, off + 16 + i * 4);
        out.push_back(rel == 0xFFFFFFFFu ? std::nullopt : std::optional<size_t>(off + rel));
    }
    return out;
}

uint32_t ReadVlq(const std::vector<uint8_t>& d, size_t* pos) {
    uint32_t v = 0;
    while (true) {
        const uint8_t b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            return v;
        }
    }
}

Decoded DecodeAdpcm(const std::vector<uint8_t>& d, size_t start, std::optional<size_t> end) {
    Decoded out;
    int64_t h1 = 0, h2 = 0;
    size_t pos = start;
    const size_t limit = end ? std::min(*end, d.size()) : d.size();
    while (pos + 16 <= limit) {
        const uint8_t shiftFilter = d[pos], flags = d[pos + 1];
        if ((flags & 0x04) && !out.loopStart) {
            out.loopStart = out.samples.size();
        }
        const int shift = shiftFilter & 0x0F;
        const int* c = kAdpcmCoef[std::min((shiftFilter >> 4) & 0x0F, 4)];
        for (size_t k = pos + 2; k < pos + 16; k++) {
            const uint8_t b = d[k];
            for (int nib : { b & 0x0F, b >> 4 }) {
                int64_t s = (static_cast<int64_t>(nib) << 12) & 0xFFFF;
                if (s & 0x8000) {
                    s -= 0x10000;
                }
                s = std::max<int64_t>(-32768, std::min<int64_t>(32767, (s >> shift) + ((h1 * c[0] + h2 * c[1] + 32) >> 6)));
                out.samples.push_back(static_cast<int>(s));
                h2 = h1;
                h1 = s;
            }
        }
        pos += 16;
        if (flags & 0x01) {
            break;
        }
    }
    return out;
}

std::vector<int> UnrollLoop(const std::vector<int>& samples, std::optional<size_t> loopStart, size_t needed) {
    std::vector<int> out = samples;
    if (!loopStart || *loopStart >= samples.size()) {
        return out;
    }
    while (out.size() < needed) {
        out.insert(out.end(), samples.begin() + *loopStart, samples.end());
    }
    return out;
}

const Kernel& Resampler::Kernels(double ratio) {
    const double key = PyRound(ratio, 9);
    auto it = mCache.find(key);
    if (it != mCache.end()) {
        return it->second;
    }
    auto seed = mSeed.find(key);
    return mCache.emplace(key, MakeKernel(seed != mSeed.end() ? seed->second : ratio)).first->second;
}

Kernel Resampler::MakeKernel(double ratio) {
    Kernel k;
    k.ratio = ratio;
    const double fc = 0.5 * 0.95 * std::min(1.0, 1.0 / ratio);
    k.half = static_cast<int>(std::ceil(kSincHalf * std::max(1.0, ratio)));
    const double half = k.half;
    for (int ph = 0; ph < kSincPhases; ph++) {
        const double frac = static_cast<double>(ph) / kSincPhases;
        std::vector<double> row;
        for (int kk = -k.half + 1; kk < k.half + 1; kk++) {
            const double x = kk - frac;
            const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double w = 0.42 + 0.5 * std::cos(kPi * x / half) + 0.08 * std::cos(2 * kPi * x / half);
            row.push_back(std::fabs(x) < half ? sinc * w : 0.0);
        }
        const double total = PySum(row.size(), [&row](size_t i) { return row[i]; });
        for (double& v : row) {
            v = v / total;
        }
        k.table.push_back(row);
    }
    return k;
}

std::vector<double> Resampler::Resample(const std::vector<int>& samples, double srcRate, double dstRate,
                                        std::optional<int64_t> length) {
    const double ratio = srcRate / dstRate;
    const Kernel& k = Kernels(ratio);
    const int64_t nOut = length ? *length : static_cast<int64_t>(samples.size() / ratio);
    std::vector<double> padded(k.half, 0.0);
    padded.reserve(samples.size() + 2 * k.half + 2);
    for (int v : samples) {
        padded.push_back(static_cast<double>(v));
    }
    padded.insert(padded.end(), k.half + 2, 0.0);
    std::vector<double> out(nOut > 0 ? static_cast<size_t>(nOut) : 0, 0.0);
    for (int64_t n = 0; n < nOut; n++) {
        const double t = n * ratio;
        const int64_t i = static_cast<int64_t>(t);
        const std::vector<double>& row = k.table[static_cast<size_t>((t - i) * kSincPhases)];
        const size_t first = static_cast<size_t>(i + 1);
        const size_t count = first >= padded.size()
                                 ? 0
                                 : std::min<size_t>(2 * static_cast<size_t>(k.half), padded.size() - first);
        out[n] = PySum(count, [&](size_t j) { return padded[first + j] * row[j]; });
    }
    return out;
}

std::vector<double> Resampler::ResampleLoop(const std::vector<int>& samples, size_t loopStart, double srcRate,
                                            int dstRate, int64_t* startOut) {
    const size_t body = samples.size() - loopStart;
    const int64_t lOut = std::max<int64_t>(1, PyRoundInt(static_cast<double>(body * static_cast<size_t>(dstRate)) / srcRate));
    const double ratio = static_cast<double>(body) / static_cast<double>(lOut);
    const int half = Kernels(ratio).half;
    *startOut = static_cast<int64_t>(std::ceil((loopStart + half) / ratio));
    const int64_t total = *startOut + lOut;
    const std::vector<int> source =
        UnrollLoop(samples, loopStart, static_cast<size_t>(static_cast<int64_t>(total * ratio) + 2 * half + 2));
    return Resample(source, ratio * dstRate, dstRate, total);
}

} // namespace rf3extract
