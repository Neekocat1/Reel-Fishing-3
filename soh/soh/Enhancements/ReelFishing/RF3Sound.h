#pragma once
// Reel Fishing III mod (step 63b, docs/DESIGN.md "Step 63 design"): RF3's own sounds, played by the mod's mixer on
// SoH's audio thread (choice S1 A). The clips come from mods/rf3_sounds.o2r (tools/rf3_sound.py build, from the
// user's disc): rf3sounds/<bank>/seqNN.rf3c is RF3's sound NN of that bank, rf3sounds/fis/spmNN.rf3c its stream NN.
// Like RF3's sound manager (RF3-MECHANICS 9) there are 16 channels; a sound started on a channel replaces the one
// playing there. Step 63c-2: streams play on a separate pool of 6 voices, as RF3's, so they overlap.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF3_SOUND_CHANNELS 16
#define RF3_STREAM_VOICES 6

// Starts RF3's sound `number` of `bank` ("fis", "sys") on `channel`, at `volume` (1 = RF3's own) and `pitch` (1 = as
// RF3 plays it). Returns 1 if started, 0 if the clip is missing (logged once).
int32_t RF3Sound_Play(int32_t channel, const char* bank, int32_t number, float volume, float pitch);
// The same for RF3's stream `id` (fis.spm), at `pitch` times its own rate. Step 63c-2: on a free stream voice (or the
// one started first when all are busy), never replacing a channel's sound; `bank` is RF3's stream bank (channel
// argument), only logged. `volume` 1 = RF3's full stream level (0x3FFF); more is clamped to 1, as RF3 does.
int32_t RF3Sound_PlayStream(int32_t bank, int32_t id, float volume, float pitch);
// Step 63c: the same at RF3's absolute SPU pitch (0x1000 = 48 kHz), as RF3's stream call 0x0015FDA0 takes it.
int32_t RF3Sound_PlayStreamSpu(int32_t bank, int32_t id, float volume, int32_t spuPitch);
// Channels only; streams play to their end.
void RF3Sound_Stop(int32_t channel);
// Channels and streams.
void RF3Sound_StopAll(void);
int32_t RF3Sound_IsPlaying(int32_t channel);
// SoH's audio thread (OTRAudio_Thread), after OoT's audio is mixed: adds the playing sounds to `frames` interleaved
// stereo frames at `rate` Hz, scaled by SoH's master volume and the mod's own volume (step 63c: not the SFX volume).
void RF3Sound_Mix(int16_t* stereo, uint32_t frames, uint32_t rate);
// The debug window's line.
const char* RF3Sound_Status(void);
// Step 63c-5a: "Record RF3 mix": the mixer's own output at RF3's level to logs/rf3_mix_<date>.wav, and each voice's
// start / stop / cut / end with its sample position to logs/rf3_mix_<date>.csv (tools/rf3_audiocmp.py).
void RF3Sound_SetRecording(int32_t on);
// Step 63e-5: the game thread, about once a second: logs the peak limiter's work since the last call (only if any).
void RF3Sound_LimiterReport(void);
int32_t RF3Sound_IsRecording(void);
const char* RF3Sound_RecordingStatus(void);

#ifdef __cplusplus
}
#endif
