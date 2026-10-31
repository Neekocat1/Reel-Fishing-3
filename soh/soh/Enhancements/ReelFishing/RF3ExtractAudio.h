#pragma once

// Reel Fishing III mod (step 76b): the audio pieces shared by the sound (P2) and music (P4) converters, ports of
// tools/rf3_sound.py's chunk_table, read_vlq, decode_adpcm, sinc_kernels, resample, resample_loop, unroll_loop and
// Python's format(x, ".Nf"). Every float operation is in the Python tool's order (sums with Python 3.13's sum()).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rf3extract {

// format(x, ".Nf"): the exact value rounded half to even.
std::string PyFixed(double x, int digits);

// chunk_table: an IECS index chunk's entries as absolute offsets (nullopt for unused).
std::vector<std::optional<size_t>> ChunkTable(const std::vector<uint8_t>& d, size_t off);

// read_vlq: a MIDI variable-length number at *pos.
uint32_t ReadVlq(const std::vector<uint8_t>& d, size_t* pos);

struct Decoded {
    std::vector<int> samples;
    std::optional<size_t> loopStart;
};

// decode_adpcm: PS2 ADPCM from start to the end-flag block (or end).
Decoded DecodeAdpcm(const std::vector<uint8_t>& d, size_t start, std::optional<size_t> end = std::nullopt);

std::vector<int> UnrollLoop(const std::vector<int>& samples, std::optional<size_t> loopStart, size_t needed);

struct Kernel {
    double ratio = 0; // the exact ratio that made it
    int half = 0;
    std::vector<std::vector<double>> table;
};

// The windowed-sinc resampler (step 63e). Its kernels are cached by round(ratio, 9) for its lifetime, as the Python
// module's _kernels for one run: the first ratio with a key makes the kernel. Not thread-safe: one per thread.
class Resampler {
  public:
    const Kernel& Kernels(double ratio);
    // The kernels made so far, by key (round(ratio, 9)); each remembers the ratio that made it.
    const std::map<double, Kernel>& Cache() const {
        return mCache;
    }
    // Step 77a: make the kernel of each of these keys from the given ratio, whoever asks first (to reproduce the
    // kernels of a run in another order).
    void Seed(std::map<double, double> keyRatios) {
        mSeed = std::move(keyRatios);
    }
    static Kernel MakeKernel(double ratio);
    // resample: `samples` played at src_rate, resampled to dst_rate; `length` output samples (default: all).
    std::vector<double> Resample(const std::vector<int>& samples, double srcRate, double dstRate,
                                 std::optional<int64_t> length = std::nullopt);
    // resample_loop (step 63e-2): a looping sample at dst_rate, still looping seamlessly; *startOut = its loop start.
    std::vector<double> ResampleLoop(const std::vector<int>& samples, size_t loopStart, double srcRate, int dstRate,
                                     int64_t* startOut);

  private:
    std::map<double, Kernel> mCache;
    std::map<double, double> mSeed;
};

} // namespace rf3extract
