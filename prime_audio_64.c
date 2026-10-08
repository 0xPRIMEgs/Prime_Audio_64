#ifndef PRIME_AUDIO_64_INCLUDED
#define PRIME_AUDIO_64_INCLUDED
/*
 * Include in ONE translation unit (main.c); do not compile separately.
 *
 *   #include "prime_audio_64.c"
 *   static wav64_t sfx_bang, sfx_slash, sfx_lv_up;
 *
 * Startup, after dfs_init(DFS_DEFAULT_LOCATION):
 *   prime_audio_init();
 *   load_snd(&sfx_bang, "rom:/soulless/bang.wav64");
 *   load_snd(&sfx_slash, "rom:/soulless/slash.wav64");
 *   load_snd(&sfx_lv_up, "rom:/soulless/lv_up.wav64");
 *   changeBGM("rom:/bgm_title.wav64");
 *   setBGMVolume(0.4f);
 *   setSFXVolume(0.7f);
 *
 * On game events:
 *   play_snd(&sfx_bang);
 *   play_snd(&sfx_lv_up); // can overlap bang without cutting it off
 *
 * EVERY FRAME (including menus/pause): prime_audio_update();
 * Replace your old audio_can_write/mixer_poll block with this call.
 * Do not also call audio_init/mixer_init or use an audio callback.
 *
 * Sound-set switching: call load_snd(&same_object, new_path) again.
 * It cancels that object's queued requests, stops its active copies and closes
 * its old file before reopening. unload_snd(&object) only unloads it.
 * prime_audio_close() closes all sounds loaded through load_snd automatically.
 *
 * Automatic playback opens a separate WAV64 stream per voice; overlapping
 * copies never share a compressed decoder or file cursor. Finished streams are
 * closed automatically. load_snd copies its path so temporary path buffers work.
 * Externally opened sounds must keep their filename strings alive too.
 *
 * 16 automatic voices + 32 FIFO waiting requests by default. play_snd returns
 * true if started/queued; false if full, uninitialized, NULL, stereo or looping.
 * A full queue rejects the NEW request; it never steals an active voice.
 * A request can be delayed when busy. stop_snd/sfx_stop_all/unload/reload/close
 * are explicit cancellations, not promises to let every accepted sound finish.
 *
 * Keep wav64_t objects alive at stable addresses until unloaded (globals or
 * static objects are easiest). Do not copy them. Use load_snd from the FIRST
 * open, not wav64_open followed by load_snd. Do not directly close/reopen a
 * managed sound. play_snd also accepts valid externally opened mono, non-looping
 * WAV64s; call stop_snd before closing those, before prime_audio_close.
 * Never pass an unopened/closed object to play_snd.
 *
 * Existing CH_SFX_* channels 2..8 and sfx_load/sfx_play slot helpers still work.
 * Automatic voices are separate channels 9 onward; don't use those directly.
 * BGM reserves 0..1 (mono/stereo). SFX must be mono. All SFX share SFX volume.
 * Source rates should be <= PRIME_AUDIO_RATE. Missing/corrupt files assert in
 * libdragon; bool load returns do NOT provide file-I/O recovery. Main thread only.
 *
 * Optional defines BEFORE including:
 * PRIME_AUDIO_RATE=11025, PRIME_AUDIO_BUFFERS=4,
 * PRIME_AUDIO_VOICES=16 (1..23), PRIME_AUDIO_QUEUE=32,
 * PRIME_AUDIO_SOUNDS=64 (maximum managed sound objects, independent of voices).
 */
#include <libdragon.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef PRIME_AUDIO_RATE
#define PRIME_AUDIO_RATE 11025
#endif
#ifndef PRIME_AUDIO_BUFFERS
#define PRIME_AUDIO_BUFFERS 4
#endif
#if PRIME_AUDIO_RATE <= 0 || PRIME_AUDIO_BUFFERS < 2
#error "Use a positive PRIME_AUDIO_RATE and at least two audio buffers"
#endif

/* Channel 1 is reserved as the second half of stereo BGM. */
#define CHANNEL_BGM    0
#define CH_SFX_MELEE   2
#define CH_SFX_HEALING 3
#define CH_SFX_SHOT    4
#define CH_SFX_MISC    5
#define CH_SFX_ENEMY   6
#define CH_SFX_SLASH   7
#define CH_SFX_THUD    8
#ifndef PRIME_AUDIO_VOICES
#define PRIME_AUDIO_VOICES 16
#endif
#ifndef PRIME_AUDIO_QUEUE
#define PRIME_AUDIO_QUEUE 32
#endif
#ifndef PRIME_AUDIO_SOUNDS
#define PRIME_AUDIO_SOUNDS 64
#endif
#if PRIME_AUDIO_VOICES < 1 || PRIME_AUDIO_VOICES > 23 || PRIME_AUDIO_QUEUE < 1 || PRIME_AUDIO_SOUNDS < 1
#error "Invalid audio pool, queue, or sound capacity"
#endif
#define PRIME_AUDIO_AUTO_FIRST 9
#define PRIME_AUDIO_CHANNELS (PRIME_AUDIO_AUTO_FIRST + PRIME_AUDIO_VOICES)

static struct {
    bool ready;
    bool bgm_loaded;
    wav64_t bgm;
    wav64_t sfx[PRIME_AUDIO_CHANNELS];
    bool sfx_loaded[PRIME_AUDIO_CHANNELS];
    wav64_t *queue[PRIME_AUDIO_QUEUE];
    int queue_count;
    wav64_t *voices[PRIME_AUDIO_VOICES];
    wav64_t voice_stream[PRIME_AUDIO_VOICES];
    bool voice_open[PRIME_AUDIO_VOICES];
    wav64_t *managed[PRIME_AUDIO_SOUNDS];
    char *managed_path[PRIME_AUDIO_SOUNDS];
    float bgm_volume;
    float sfx_volume;
    float master_volume;
} prime_audio_state = {
    .bgm_volume = 0.5f,
    .sfx_volume = 0.7f,
    .master_volume = 1.0f
};

static inline float prime_audio_clamp(float v) {
    if (!(v >= 0.0f)) return 0.0f; /* also handles NaN */
    return v > 1.0f ? 1.0f : v;
}
static inline bool prime_audio_sfx_channel(int ch) {
    return ch >= CH_SFX_MELEE && ch < PRIME_AUDIO_AUTO_FIRST;
}
static inline void setBGMVolume(float volume) {
    prime_audio_state.bgm_volume = prime_audio_clamp(volume);
    if (prime_audio_state.ready)
        mixer_ch_set_vol(CHANNEL_BGM, prime_audio_state.bgm_volume,
                         prime_audio_state.bgm_volume);
}
static inline void setSFXVolume(float volume) {
    prime_audio_state.sfx_volume = prime_audio_clamp(volume);
    if (!prime_audio_state.ready) return;
    for (int ch = CH_SFX_MELEE; ch < PRIME_AUDIO_CHANNELS; ++ch)
        mixer_ch_set_vol(ch, prime_audio_state.sfx_volume,
                         prime_audio_state.sfx_volume);
}
static inline void setMasterVolume(float volume) {
    prime_audio_state.master_volume = prime_audio_clamp(volume);
    if (prime_audio_state.ready) mixer_set_vol(prime_audio_state.master_volume);
}
static inline float getBGMVolume(void) { return prime_audio_state.bgm_volume; }
static inline float getSFXVolume(void) { return prime_audio_state.sfx_volume; }
static inline float getMasterVolume(void) { return prime_audio_state.master_volume; }

/* Idempotent. Volume preferences set before init are retained. */
static inline void prime_audio_init(void) {
    if (prime_audio_state.ready) return;
    audio_init(PRIME_AUDIO_RATE, PRIME_AUDIO_BUFFERS);
    mixer_init(PRIME_AUDIO_CHANNELS);
    prime_audio_state.ready = true;
    setMasterVolume(getMasterVolume());
    setBGMVolume(getBGMVolume());
    setSFXVolume(getSFXVolume());
}

/* Each compressed playback needs its OWN file cursor and decoder state.
 * Never copy wav64_t: open an independent stream in a stable voice object. */
static inline void prime_audio_release_voice(int i) {
    if (prime_audio_state.voice_open[i]) {
        mixer_ch_stop(PRIME_AUDIO_AUTO_FIRST + i);
        wav64_close(&prime_audio_state.voice_stream[i]);
        prime_audio_state.voice_open[i] = false;
    }
    prime_audio_state.voices[i] = NULL;
}

/* Called between output buffers so newly free channels can take queued sounds. */
static inline void prime_audio_dispatch(void) {
    for (int i = 0; i < PRIME_AUDIO_VOICES; ++i) {
        int ch = PRIME_AUDIO_AUTO_FIRST + i;
        if (mixer_ch_playing(ch)) continue;
        prime_audio_release_voice(i);
        if (!prime_audio_state.queue_count) continue;
        wav64_t *snd = prime_audio_state.queue[0];
        --prime_audio_state.queue_count;
        for (int j = 0; j < prime_audio_state.queue_count; ++j)
            prime_audio_state.queue[j] = prime_audio_state.queue[j + 1];
        prime_audio_state.voices[i] = snd;
        wav64_t *stream = &prime_audio_state.voice_stream[i];
        wav64_open(stream, snd->wave.name);
        wav64_set_loop(stream, false);
        prime_audio_state.voice_open[i] = true;
        wav64_play(stream, ch);
        mixer_ch_set_vol(ch, getSFXVolume(), getSFXVolume());
    }
}
static inline bool play_snd(wav64_t *snd) {
    if (!prime_audio_state.ready || !snd || snd->wave.channels != 1
        || snd->wave.loop_len != 0 || !snd->wave.name) return false;
    prime_audio_dispatch(); /* older requests get first choice */
    if (prime_audio_state.queue_count == PRIME_AUDIO_QUEUE) return false;
    prime_audio_state.queue[prime_audio_state.queue_count++] = snd;
    prime_audio_dispatch();
    return true;
}
static inline int snd_queue_count(void) { return prime_audio_state.queue_count; }
/* Cancel this sound's pending requests and stop its automatic voices. */
static inline void stop_snd(wav64_t *snd) {
    if (!snd) return;
    int kept = 0;
    for (int i = 0; i < prime_audio_state.queue_count; ++i)
        if (prime_audio_state.queue[i] != snd)
            prime_audio_state.queue[kept++] = prime_audio_state.queue[i];
    prime_audio_state.queue_count = kept;
    for (int i = 0; i < PRIME_AUDIO_VOICES; ++i) {
        if (prime_audio_state.voices[i] != snd) continue;
        prime_audio_release_voice(i);
    }
}
static inline void unload_snd(wav64_t *snd) {
    if (!snd) return;
    stop_snd(snd);
    for (int i = 0; i < PRIME_AUDIO_SOUNDS; ++i) {
        if (prime_audio_state.managed[i] != snd) continue;
        wav64_close(snd); /* SDK also stops any legacy-channel copies */
        prime_audio_state.managed[i] = NULL;
        free(prime_audio_state.managed_path[i]);
        prime_audio_state.managed_path[i] = NULL;
        return;
    }
}
/* Load/reload independently of playback channels. Keep the same object address. */
static inline bool load_snd(wav64_t *snd, const char *path) {
    if (!prime_audio_state.ready || !snd || !path || !*path) return false;
    int slot = -1;
    for (int i = 0; i < PRIME_AUDIO_SOUNDS; ++i) {
        if (prime_audio_state.managed[i] == snd) { slot = i; break; }
        if (!prime_audio_state.managed[i] && slot < 0) slot = i;
    }
    if (slot < 0) return false;
    /* Copy BEFORE unloading: path may point at this sound's current filename. */
    char *saved_path = malloc(strlen(path) + 1);
    if (!saved_path) return false;
    strcpy(saved_path, path);
    unload_snd(snd);
    wav64_open(snd, saved_path);
    if (snd->wave.channels != 1) {
        wav64_close(snd);
        free(saved_path);
        return false;
    }
    wav64_set_loop(snd, false);
    prime_audio_state.managed[slot] = snd;
    prime_audio_state.managed_path[slot] = saved_path;
    return true;
}
static inline void prime_audio_update(void) {
    if (!prime_audio_state.ready) return;
    prime_audio_dispatch();
    while (audio_can_write()) {
        short *buffer = audio_write_begin();
        mixer_poll(buffer, audio_get_buffer_length());
        audio_write_end();
        prime_audio_dispatch();
    }
}
static inline void bgm_stop(void) {
    if (prime_audio_state.ready) mixer_ch_stop(CHANNEL_BGM);
}
static inline bool bgm_is_playing(void) {
    return prime_audio_state.ready && prime_audio_state.bgm_loaded
        && mixer_ch_playing(CHANNEL_BGM);
}
static inline void bgm_unload(void) {
    if (!prime_audio_state.bgm_loaded) return;
    bgm_stop();
    wav64_close(&prime_audio_state.bgm);
    prime_audio_state.bgm_loaded = false;
}
/* Load without starting. A NULL/empty path leaves current music untouched. */
static inline bool bgm_load(const char *path) {
    if (!prime_audio_state.ready || !path || !*path) return false;
    bgm_unload();
    wav64_open(&prime_audio_state.bgm, path);
    prime_audio_state.bgm_loaded = true;
    wav64_set_loop(&prime_audio_state.bgm, true);
    return true;
}
/* Starts/restarts from the beginning. A stopped track remains loaded. */
static inline bool bgm_play(void) {
    if (!prime_audio_state.ready || !prime_audio_state.bgm_loaded) return false;
    wav64_play(&prime_audio_state.bgm, CHANNEL_BGM);
    setBGMVolume(getBGMVolume());
    return true;
}
static inline void bgm_set_loop(bool loop) {
    if (prime_audio_state.bgm_loaded) wav64_set_loop(&prime_audio_state.bgm, loop);
}
static inline bool changeBGM(const char *path) {
    return bgm_load(path) && bgm_play();
}
static inline void sfx_stop(int channel) {
    if (prime_audio_state.ready && prime_audio_sfx_channel(channel))
        mixer_ch_stop(channel);
}
static inline void sfx_stop_all(void) {
    prime_audio_state.queue_count = 0;
    if (prime_audio_state.ready)
        for (int ch = CH_SFX_MELEE; ch < PRIME_AUDIO_CHANNELS; ++ch) mixer_ch_stop(ch);
    for (int i = 0; i < PRIME_AUDIO_VOICES; ++i) prime_audio_release_voice(i);
}
static inline void sfx_unload(int channel) {
    if (!prime_audio_sfx_channel(channel) || !prime_audio_state.sfx_loaded[channel])
        return;
    sfx_stop(channel);
    stop_snd(&prime_audio_state.sfx[channel]);
    wav64_close(&prime_audio_state.sfx[channel]);
    prime_audio_state.sfx_loaded[channel] = false;
}
/* Returns false for invalid state/channel/path or a non-mono asset.
 * A rejected stereo asset leaves this slot empty; missing files assert in SDK. */
static inline bool sfx_load(int channel, const char *path) {
    if (!prime_audio_state.ready || !prime_audio_sfx_channel(channel)
        || !path || !*path) return false;
    sfx_unload(channel);
    wav64_open(&prime_audio_state.sfx[channel], path);
    if (prime_audio_state.sfx[channel].wave.channels != 1) {
        wav64_close(&prime_audio_state.sfx[channel]);
        return false;
    }
    wav64_set_loop(&prime_audio_state.sfx[channel], false);
    prime_audio_state.sfx_loaded[channel] = true;
    return true;
}
static inline bool sfx_play(int channel) {
    if (!prime_audio_state.ready || !prime_audio_sfx_channel(channel)
        || !prime_audio_state.sfx_loaded[channel]) return false;
    wav64_play(&prime_audio_state.sfx[channel], channel);
    mixer_ch_set_vol(channel, getSFXVolume(), getSFXVolume());
    return true;
}
static inline bool sfx_is_playing(int channel) {
    return prime_audio_state.ready && prime_audio_sfx_channel(channel)
        && mixer_ch_playing(channel);
}
static inline void prime_audio_close(void) {
    if (!prime_audio_state.ready) return;
    bgm_unload();
    sfx_stop_all();
    for (int ch = CH_SFX_MELEE; ch < PRIME_AUDIO_CHANNELS; ++ch) sfx_unload(ch);
    for (int i = 0; i < PRIME_AUDIO_SOUNDS; ++i)
        if (prime_audio_state.managed[i]) unload_snd(prime_audio_state.managed[i]);
    mixer_close();
    audio_close();
    prime_audio_state.ready = false;
}
#endif /* PRIME_AUDIO_64_INCLUDED */
