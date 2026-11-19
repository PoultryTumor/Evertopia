#ifdef RECOMP_LAUNCHER

#include "spc_jukebox.h"
#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/dsp.h"
#include "desktop/sdl_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum {
    SPC_STATE_STOPPED = 0,
    SPC_STATE_PLAYING = 1,
    SPC_STATE_PAUSED  = 2,
    SPC_STATE_FADING  = 3
};

static SDL_Mutex *s_spc_mutex = NULL;
static Apu *s_spc_apu = NULL;
static int s_spc_state = SPC_STATE_STOPPED;
static float s_spc_volume = 0.85f;
static uint32_t s_fade_total_samples = 0;
static uint32_t s_fade_pos_samples = 0;

#if SNESRECOMP_SDL3
static SDL_AudioStream *s_spc_stream = NULL;
#else
static SDL_AudioDeviceID s_spc_dev = 0;
static int s_spc_dev_freq = 48000;
static double s_spc_resamp_pos = 0.0;
static int16_t s_spc_prev_l = 0;
static int16_t s_spc_prev_r = 0;
static int16_t s_spc_cur_l = 0;
static int16_t s_spc_cur_r = 0;
#endif

static int16_t *s_spc_mix_buf = NULL;
static size_t s_spc_mix_cap = 0;

static void SpcEnsureMutex(void)
{
    if (!s_spc_mutex) {
        s_spc_mutex = SDL_CreateMutex();
    }
}

static void SpcLock(void)
{
    SpcEnsureMutex();
    if (s_spc_mutex) SDL_LockMutex(s_spc_mutex);
}

static void SpcUnlock(void)
{
    if (s_spc_mutex) SDL_UnlockMutex(s_spc_mutex);
}

static void SpcPumpNativeLocked(uint32_t needed_frames)
{
    if (!s_spc_apu || !s_spc_apu->dsp) return;
    while (dsp_available(s_spc_apu->dsp) < needed_frames) {
        uint32_t deficit = needed_frames - dsp_available(s_spc_apu->dsp);
        uint32_t cycles = deficit * 32u + 128u;
        if (cycles > 32768u) cycles = 32768u;
        apu_runCycles(s_spc_apu, cycles);
    }
}

static void SpcGenerateNativeStereoLocked(int16_t *out, uint32_t frames)
{
    if (!s_spc_apu || !s_spc_apu->dsp ||
        s_spc_state == SPC_STATE_STOPPED || s_spc_state == SPC_STATE_PAUSED) {
        memset(out, 0, (size_t)frames * 2u * sizeof(int16_t));
        return;
    }

    SpcPumpNativeLocked(frames);
    Dsp *dsp = s_spc_apu->dsp;
    uint32_t avail = dsp_available(dsp);
    uint32_t take = (avail < frames) ? avail : frames;

    for (uint32_t i = 0; i < take; i++) {
        int16_t l = 0, r = 0;
        dsp_peek(dsp, i, &l, &r);

        float gain = s_spc_volume;
        if (s_spc_state == SPC_STATE_FADING) {
            if (s_fade_total_samples == 0 || s_fade_pos_samples >= s_fade_total_samples) {
                gain = 0.0f;
                s_spc_state = SPC_STATE_STOPPED;
            } else {
                float t = (float)s_fade_pos_samples / (float)s_fade_total_samples;
                float rem = 1.0f - t;
                gain *= rem * rem; /* smooth perceptual fade-out */
                s_fade_pos_samples++;
            }
        }

        int32_t sl = (int32_t)((float)l * gain);
        int32_t sr = (int32_t)((float)r * gain);
        if (sl > 32767) sl = 32767;
        else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767;
        else if (sr < -32768) sr = -32768;
        out[i * 2u]      = (int16_t)sl;
        out[i * 2u + 1u] = (int16_t)sr;
    }
    dsp_advance(dsp, take);

    if (take < frames) {
        memset(out + take * 2u, 0, (size_t)(frames - take) * 2u * sizeof(int16_t));
    }
}

#if SNESRECOMP_SDL3
static void SDLCALL SpcAudioStreamCallback(void *userdata, SDL_AudioStream *stream,
                                           int additional_amount, int total_amount)
{
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0) return;

    uint32_t frames = (uint32_t)(additional_amount / 4);
    if (frames == 0) return;

    if ((size_t)frames > s_spc_mix_cap) {
        int16_t *nb = (int16_t *)realloc(s_spc_mix_buf, (size_t)frames * 2u * sizeof(int16_t));
        if (!nb) return;
        s_spc_mix_buf = nb;
        s_spc_mix_cap = (size_t)frames;
    }

    SpcLock();
    SpcGenerateNativeStereoLocked(s_spc_mix_buf, frames);
    SpcUnlock();

    SDL_PutAudioStreamData(stream, s_spc_mix_buf, (int)(frames * 4u));
}
#else
static void SDLCALL SpcAudioCallback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int out_frames = len / 4;
    if (out_frames <= 0) return;

    SpcLock();
    if (!s_spc_apu || s_spc_state == SPC_STATE_STOPPED || s_spc_state == SPC_STATE_PAUSED) {
        memset(out, 0, (size_t)len);
        SpcUnlock();
        return;
    }

    double step = 32040.0 / (double)(s_spc_dev_freq > 0 ? s_spc_dev_freq : 48000);
    for (int i = 0; i < out_frames; i++) {
        while (s_spc_resamp_pos >= 1.0) {
            s_spc_prev_l = s_spc_cur_l;
            s_spc_prev_r = s_spc_cur_r;
            int16_t tmp[2] = {0, 0};
            SpcGenerateNativeStereoLocked(tmp, 1);
            s_spc_cur_l = tmp[0];
            s_spc_cur_r = tmp[1];
            s_spc_resamp_pos -= 1.0;
        }
        double frac = s_spc_resamp_pos;
        out[i * 2]     = (int16_t)((double)s_spc_prev_l + ((double)s_spc_cur_l - (double)s_spc_prev_l) * frac);
        out[i * 2 + 1] = (int16_t)((double)s_spc_prev_r + ((double)s_spc_cur_r - (double)s_spc_prev_r) * frac);
        s_spc_resamp_pos += step;
    }
    SpcUnlock();
}
#endif

static bool SpcEnsureAudioOpen(void)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!snesrecomp_sdl_init(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[SPC Jukebox] SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
            return false;
        }
    }

#if SNESRECOMP_SDL3
    if (s_spc_stream) return true;
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.freq = 32040;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    s_spc_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, SpcAudioStreamCallback, NULL);
    if (!s_spc_stream) {
        fprintf(stderr, "[SPC Jukebox] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(s_spc_stream);
    return true;
#else
    if (s_spc_dev != 0) return true;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = 48000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 512;
    want.callback = SpcAudioCallback;
    s_spc_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_spc_dev == 0) {
        fprintf(stderr, "[SPC Jukebox] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    s_spc_dev_freq = have.freq > 0 ? have.freq : 48000;
    SDL_PauseAudioDevice(s_spc_dev, 0);
    return true;
#endif
}

static bool SpcLoadFileIntoApu(Apu *apu, const uint8_t *data, size_t size)
{
    if (!apu || !apu->spc || !apu->dsp || size < 0x10180u) return false;
    if (memcmp(data, "SNES-SPC700 Sound File Data", 27) != 0) return false;

    apu_reset(apu);

    /* 64 KB ARAM */
    memcpy(apu->ram, data + 0x0100, 0x10000u);

    /* SPC700 registers */
    Spc *spc = apu->spc;
    spc->pc = (uint16_t)(data[0x25] | ((uint16_t)data[0x26] << 8));
    spc->a = data[0x27];
    spc->x = data[0x28];
    spc->y = data[0x29];
    uint8_t psw = data[0x2A];
    spc->sp = data[0x2B];
    spc->c = (psw & 0x01u) != 0;
    spc->z = (psw & 0x02u) != 0;
    spc->i = (psw & 0x04u) != 0;
    spc->h = (psw & 0x08u) != 0;
    spc->b = (psw & 0x10u) != 0;
    spc->p = (psw & 0x20u) != 0;
    spc->v = (psw & 0x40u) != 0;
    spc->n = (psw & 0x80u) != 0;
    spc->stopped = false;
    spc->cyclesUsed = 0;

    /* APU I/O registers */
    uint8_t f1 = apu->ram[0xF1];
    for (int i = 0; i < 3; i++) {
        apu->timer[i].enabled = (f1 & (1 << i)) != 0;
        apu->timer[i].divider = 0;
        apu->timer[i].counter = 0;
        apu->timer[i].target = apu->ram[0xFA + i];
    }
    apu->romReadable = (f1 & 0x80u) != 0;
    apu->dspAdr = apu->ram[0xF2];
    for (int i = 0; i < 4; i++) {
        apu->inPorts[i] = apu->ram[0xF4 + i];
    }
    apu->inPorts[4] = apu->ram[0xF8];
    apu->inPorts[5] = apu->ram[0xF9];

    /* 128 DSP registers */
    const uint8_t *dsp_regs = data + 0x10100;
    for (int i = 0; i < 0x80; i++) {
        if (i == 0x4C || i == 0x7C) continue;
        dsp_write(apu->dsp, (uint8_t)i, dsp_regs[i]);
    }
    dsp_write(apu->dsp, 0x4C, dsp_regs[0x4C]);
    apu->dsp->ram[0x7C] = dsp_regs[0x7C];

    apu->dsp->sampleRead = apu->dsp->sampleWrite;
    return true;
}

static int JbPlay(const char *spc_path)
{
    if (!spc_path || !spc_path[0]) return 0;

    FILE *f = fopen(spc_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x10180L || sz > 0x20000L) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return 0;
    }

    if (!SpcEnsureAudioOpen()) {
        free(buf);
        return 0;
    }

    SpcLock();
    if (!s_spc_apu) {
        s_spc_apu = apu_init();
    }
    bool ok = s_spc_apu && SpcLoadFileIntoApu(s_spc_apu, buf, (size_t)sz);
    if (ok) {
        s_spc_state = SPC_STATE_PLAYING;
        s_fade_total_samples = 0;
        s_fade_pos_samples = 0;
    } else {
        s_spc_state = SPC_STATE_STOPPED;
    }
    SpcUnlock();

    free(buf);
    return ok ? 1 : 0;
}

static void JbStop(void)
{
    SpcLock();
    s_spc_state = SPC_STATE_STOPPED;
    s_fade_total_samples = 0;
    s_fade_pos_samples = 0;
    if (s_spc_apu) {
        apu_reset(s_spc_apu);
    }
    SpcUnlock();
}

static void JbSetPaused(int paused)
{
    SpcLock();
    if (paused && s_spc_state == SPC_STATE_PLAYING) {
        s_spc_state = SPC_STATE_PAUSED;
    } else if (!paused && s_spc_state == SPC_STATE_PAUSED) {
        s_spc_state = SPC_STATE_PLAYING;
    }
    SpcUnlock();
}

static void JbSetVolume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    SpcLock();
    s_spc_volume = volume;
    SpcUnlock();
}

static int JbGetState(void)
{
    SpcLock();
    int st = s_spc_state;
    SpcUnlock();
    return st;
}

static void JbBeginFadeout(int duration_ms)
{
    if (duration_ms <= 0) duration_ms = 500;
    SpcLock();
    if (s_spc_state == SPC_STATE_PLAYING) {
        s_fade_total_samples = (uint32_t)((uint64_t)duration_ms * 32040u / 1000u);
        if (s_fade_total_samples == 0) s_fade_total_samples = 1;
        s_fade_pos_samples = 0;
        s_spc_state = SPC_STATE_FADING;
    } else if (s_spc_state != SPC_STATE_FADING) {
        s_spc_state = SPC_STATE_STOPPED;
    }
    SpcUnlock();
}

static int JbIsFading(void)
{
    SpcLock();
    int fading = (s_spc_state == SPC_STATE_FADING) ? 1 : 0;
    SpcUnlock();
    return fading;
}

static void JbShutdown(void)
{
    SpcLock();
    s_spc_state = SPC_STATE_STOPPED;
    s_fade_total_samples = 0;
    s_fade_pos_samples = 0;
    SpcUnlock();

#if SNESRECOMP_SDL3
    if (s_spc_stream) {
        SDL_DestroyAudioStream(s_spc_stream);
        s_spc_stream = NULL;
    }
#else
    if (s_spc_dev != 0) {
        SDL_CloseAudioDevice(s_spc_dev);
        s_spc_dev = 0;
    }
#endif

    SpcLock();
    if (s_spc_apu) {
        apu_free(s_spc_apu);
        s_spc_apu = NULL;
    }
    free(s_spc_mix_buf);
    s_spc_mix_buf = NULL;
    s_spc_mix_cap = 0;
    SpcUnlock();

    if (s_spc_mutex) {
        SDL_DestroyMutex(s_spc_mutex);
        s_spc_mutex = NULL;
    }
}

void SoeRegisterLauncherSpcJukebox(void)
{
    RecompLauncherSpcPlayer player;
    memset(&player, 0, sizeof(player));
    player.play = JbPlay;
    player.stop = JbStop;
    player.set_paused = JbSetPaused;
    player.set_volume = JbSetVolume;
    player.get_state = JbGetState;
    player.begin_fadeout = JbBeginFadeout;
    player.is_fading = JbIsFading;
    player.shutdown = JbShutdown;
    recomp_launcher_set_spc_player(&player);
}

#endif /* RECOMP_LAUNCHER */
