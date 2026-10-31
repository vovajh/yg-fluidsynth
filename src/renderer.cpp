// Рендер MIDI -> WAV через libfluidsynth (player + synth, тайминг по сэмплам = быстрее реального времени).
#include "renderer.h"

#include <fluidsynth.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fsr {

namespace {

Job* g_logJob = nullptr;
char g_lastErr[256];

double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

void appendStr(char* dst, size_t cap, const char* s) {
    size_t n = strlen(dst);
    if (n + 1 >= cap) return;
    snprintf(dst + n, cap - n, "%s%s", n ? " | " : "", s);
}

void logFn(int level, const char* msg, void*) {
    if (!msg) return;
    if (level <= FLUID_ERR) {
        snprintf(g_lastErr, sizeof(g_lastErr), "%s", msg);
    }
    if (strstr(msg, "ROM sample ignored")) return;  // безвредно для GM-баз вроде Vintage Dreams
    if (g_logJob && level <= FLUID_WARN) appendStr(g_logJob->warnings, sizeof(g_logJob->warnings), msg);
}

void setError(Job& job, const char* what) {
    if (g_lastErr[0]) snprintf(job.error, sizeof(job.error), "%s (%s)", what, g_lastErr);
    else snprintf(job.error, sizeof(job.error), "%s", what);
}

const char* kDeniedKeys[] = {"synth.lock-memory", "synth.cpu-cores", "synth.threadsafe-api",
                             "synth.verbose",     "synth.default-soundfont", "synth.sample-rate",
                             "synth.gain",        "synth.polyphony", nullptr};

bool isDenied(const char* key) {
    for (int i = 0; kDeniedKeys[i]; i++)
        if (strcmp(kDeniedKeys[i], key) == 0) return true;
    return false;
}

bool parseBool(const char* v, int* out) {
    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes")) { *out = 1; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no")) { *out = 0; return true; }
    return false;
}

bool applySetting(fluid_settings_t* st, const char* key, const char* val, char* err, size_t cap) {
    if (strncmp(key, "synth.", 6) != 0 && strncmp(key, "player.", 7) != 0) {
        snprintf(err, cap, "разрешены только synth.* и player.*");
        return false;
    }
    if (isDenied(key)) {
        snprintf(err, cap, "%s управляется отдельной настройкой плагина (gain, rate, poly) или закрыта", key);
        return false;
    }
    int type = fluid_settings_get_type(st, key);
    int rc = FLUID_FAILED;
    switch (type) {
        case FLUID_NUM_TYPE: {
            char* end = nullptr;
            double d = strtod(val, &end);
            if (end == val || *end) { snprintf(err, cap, "ожидается число"); return false; }
            rc = fluid_settings_setnum(st, key, d);
            break;
        }
        case FLUID_INT_TYPE: {
            int b;
            int n;
            if (parseBool(val, &b)) n = b;
            else {
                char* end = nullptr;
                long l = strtol(val, &end, 10);
                if (end == val || *end) { snprintf(err, cap, "ожидается целое число или on/off"); return false; }
                n = (int) l;
            }
            rc = fluid_settings_setint(st, key, n);
            break;
        }
        case FLUID_STR_TYPE:
        case FLUID_SET_TYPE:
            rc = fluid_settings_setstr(st, key, val);
            break;
        default:
            snprintf(err, cap, "нет такой настройки");
            return false;
    }
    if (rc != FLUID_OK) {
        snprintf(err, cap, "значение не принято (вне диапазона или нет в списке допустимых)");
        return false;
    }
    return true;
}

// ---- WAV ----
void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
void put32(uint8_t* p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

size_t wavHeader(uint8_t* h, int ch, int rate, int bits, size_t dataBytes) {
    bool isFloat = bits == 32;
    size_t n = 0;
    unsigned blockAlign = (unsigned) (ch * bits / 8);
    size_t fmtSize = isFloat ? 18 : 16;
    size_t factSize = isFloat ? 12 : 0;
    memcpy(h + n, "RIFF", 4); n += 4;
    put32(h + n, (unsigned) (4 + 8 + fmtSize + factSize + 8 + dataBytes)); n += 4;
    memcpy(h + n, "WAVE", 4); n += 4;
    memcpy(h + n, "fmt ", 4); n += 4;
    put32(h + n, (unsigned) fmtSize); n += 4;
    put16(h + n, isFloat ? 3 : 1); n += 2;
    put16(h + n, (unsigned) ch); n += 2;
    put32(h + n, (unsigned) rate); n += 4;
    put32(h + n, (unsigned) rate * blockAlign); n += 4;
    put16(h + n, blockAlign); n += 2;
    put16(h + n, (unsigned) bits); n += 2;
    if (isFloat) {
        put16(h + n, 0); n += 2;
        memcpy(h + n, "fact", 4); n += 4;
        put32(h + n, 4); n += 4;
        put32(h + n, (unsigned) (dataBytes / blockAlign)); n += 4;
    }
    memcpy(h + n, "data", 4); n += 4;
    put32(h + n, (unsigned) dataBytes); n += 4;
    return n;
}

struct Guard {
    fluid_settings_t* st = nullptr;
    fluid_synth_t* synth = nullptr;
    fluid_player_t* player = nullptr;
    FILE* out = nullptr;
    char* midi = nullptr;
    ~Guard() {
        if (player) delete_fluid_player(player);
        if (synth) delete_fluid_synth(synth);
        if (st) delete_fluid_settings(st);
        if (out) fclose(out);
        free(midi);
        g_logJob = nullptr;
    }
};

struct PlayCtx {
    fluid_synth_t* synth;
    const ProgOverride* ovr;
    int count;
    bool applied;
};

const ProgOverride* findOvr(const PlayCtx* c, int chan) {
    for (int i = 0; i < c->count; i++)
        if (c->ovr[i].chan == chan) return &c->ovr[i];
    return nullptr;
}

// Перехват событий плеера: принудительные инструменты на каналах (аналог shell-команды `prog`/`select`).
int playbackHandler(void* data, fluid_midi_event_t* ev) {
    PlayCtx* c = static_cast<PlayCtx*>(data);
    if (!c->applied) {
        c->applied = true;  // первое событие приходит уже после player.reset-synth
        for (int i = 0; i < c->count; i++) {
            if (c->ovr[i].bank >= 0) fluid_synth_bank_select(c->synth, c->ovr[i].chan, c->ovr[i].bank);
            fluid_synth_program_change(c->synth, c->ovr[i].chan, c->ovr[i].prog);
        }
    }
    int type = fluid_midi_event_get_type(ev);
    if (type == 0xC0 || type == 0xB0) {
        const ProgOverride* o = findOvr(c, fluid_midi_event_get_channel(ev));
        if (o) {
            if (type == 0xC0) return FLUID_OK;
            int cc = fluid_midi_event_get_control(ev);
            if (o->bank >= 0 && (cc == 0 || cc == 32)) return FLUID_OK;
        }
    }
    return fluid_synth_handle_midi_event(c->synth, ev);
}

uint32_t g_rng = 0x1234567u;
float rnd01() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float) (g_rng & 0xFFFFFF) * (1.0f / 16777216.0f);
}

}  // namespace

void initLogging() {
    fluid_set_log_function(FLUID_PANIC, logFn, nullptr);
    fluid_set_log_function(FLUID_ERR, logFn, nullptr);
    fluid_set_log_function(FLUID_WARN, logFn, nullptr);
    fluid_set_log_function(FLUID_INFO, nullptr, nullptr);
    fluid_set_log_function(FLUID_DBG, nullptr, nullptr);
}

bool validateSetting(const char* key, const char* value, char* err, size_t cap) {
    fluid_settings_t* st = new_fluid_settings();
    if (!st) { snprintf(err, cap, "нет памяти"); return false; }
    bool ok = applySetting(st, key, value, err, cap);
    delete_fluid_settings(st);
    return ok;
}

int render(const Params& p, Job& job) {
    Guard g;
    job.state = kRunning;
    job.permille = 0;
    job.error[0] = 0;
    job.warnings[0] = 0;
    job.seconds = job.wall = job.peak = 0;
    job.bytes = 0;
    job.truncated = 0;
    g_lastErr[0] = 0;
    g_logJob = &job;
    double t0 = nowSec();

    // --- MIDI в память ---
    FILE* mf = fopen(p.midi_path, "rb");
    if (!mf) { snprintf(job.error, sizeof(job.error), "не могу открыть MIDI-файл"); job.state = kFailed; return -1; }
    fseek(mf, 0, SEEK_END);
    long msz = ftell(mf);
    fseek(mf, 0, SEEK_SET);
    if (msz <= 14 || msz > 32L * 1024 * 1024) {
        fclose(mf);
        snprintf(job.error, sizeof(job.error), "MIDI-файл пустой или слишком большой (%ld байт)", msz);
        job.state = kFailed;
        return -1;
    }
    g.midi = static_cast<char*>(malloc((size_t) msz));
    if (!g.midi || fread(g.midi, 1, (size_t) msz, mf) != (size_t) msz) {
        fclose(mf);
        snprintf(job.error, sizeof(job.error), "ошибка чтения MIDI");
        job.state = kFailed;
        return -1;
    }
    fclose(mf);
    {
        bool smf = memcmp(g.midi, "MThd", 4) == 0;
        bool rmid = msz > 20 && memcmp(g.midi, "RIFF", 4) == 0 && memcmp(g.midi + 8, "RMID", 4) == 0;
        if (!smf && !rmid) {
            snprintf(job.error, sizeof(job.error), "это не MIDI-файл (нет заголовка MThd/RMID)");
            job.state = kFailed;
            return -1;
        }
    }

    // --- настройки ---
    g.st = new_fluid_settings();
    if (!g.st) { snprintf(job.error, sizeof(job.error), "нет памяти"); job.state = kFailed; return -1; }
    long long biggest = 0;
    for (int i = 0; i < p.sf_count; i++) {
        FILE* f = fopen(p.sf_paths[i], "rb");
        if (f) { fseek(f, 0, SEEK_END); long long s = ftell(f); fclose(f); if (s > biggest) biggest = s; }
    }
    fluid_settings_setnum(g.st, "synth.sample-rate", p.sample_rate);
    fluid_settings_setnum(g.st, "synth.gain", p.gain);
    fluid_settings_setint(g.st, "synth.polyphony", p.polyphony);
    fluid_settings_setint(g.st, "synth.reverb.active", p.reverb ? 1 : 0);
    fluid_settings_setint(g.st, "synth.chorus.active", p.chorus ? 1 : 0);
    if (fluid_settings_setint(g.st, "synth.limiter.active", p.limiter ? 1 : 0) != FLUID_OK && p.limiter)
        appendStr(job.warnings, sizeof(job.warnings), "лимитер недоступен в этой сборке");
    fluid_settings_setint(g.st, "synth.lock-memory", 0);
    fluid_settings_setint(g.st, "synth.verbose", 0);
    fluid_settings_setint(g.st, "synth.dynamic-sample-loading", biggest > 64LL * 1024 * 1024 ? 1 : 0);
    fluid_settings_setstr(g.st, "synth.reverb.engine", p.reverb_engine);
    fluid_settings_setstr(g.st, "synth.midi-bank-select", p.bank_select);
    fluid_settings_setstr(g.st, "player.timing-source", "sample");

    char line[256], err[200];
    const char* s = p.extra;
    while (*s) {
        const char* e = strchr(s, '\n');
        size_t n = e ? (size_t) (e - s) : strlen(s);
        if (n > 0 && n < sizeof(line)) {
            memcpy(line, s, n);
            line[n] = 0;
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                if (!applySetting(g.st, line, eq + 1, err, sizeof(err)))
                    appendStr(job.warnings, sizeof(job.warnings), err);
            }
        }
        if (!e) break;
        s = e + 1;
    }

    g.synth = new_fluid_synth(g.st);
    if (!g.synth) { setError(job, "не удалось создать синтезатор"); job.state = kFailed; return -1; }
    fluid_synth_set_interp_method(g.synth, -1, p.interp);

    for (int i = 0; i < p.sf_count; i++) {
        if (fluid_synth_sfload(g.synth, p.sf_paths[i], 1) < 0) {
            const char* base = strrchr(p.sf_paths[i], '/');
            char what[200];
            snprintf(what, sizeof(what), "не удалось загрузить SoundFont %s", base ? base + 1 : p.sf_paths[i]);
            setError(job, what);
            job.state = kFailed;
            return -1;
        }
    }
    if (fluid_synth_sfcount(g.synth) == 0) {
        snprintf(job.error, sizeof(job.error), "не выбрано ни одного SoundFont");
        job.state = kFailed;
        return -1;
    }

    g.player = new_fluid_player(g.synth);
    if (!g.player || fluid_player_add_mem(g.player, g.midi, (size_t) msz) != FLUID_OK) {
        setError(job, "это не похоже на MIDI-файл");
        job.state = kFailed;
        return -1;
    }
    free(g.midi);
    g.midi = nullptr;
    if (p.speed > 0.0 && fabs(p.speed - 1.0) > 1e-6)
        fluid_player_set_tempo(g.player, FLUID_PLAYER_TEMPO_INTERNAL, p.speed);

    PlayCtx pc = {g.synth, p.ovr, p.ovr_count, false};
    fluid_player_set_playback_callback(g.player, playbackHandler, &pc);

    // --- вывод ---
    g.out = fopen(p.out_path, "wb");
    if (!g.out) { snprintf(job.error, sizeof(job.error), "не могу создать выходной файл"); job.state = kFailed; return -1; }
    const int ch = p.mono ? 1 : 2;
    const int bps = p.bits == 24 ? 3 : (p.bits == 32 ? 4 : 2);
    uint8_t hdr[64];
    size_t hlen = wavHeader(hdr, ch, p.sample_rate, p.bits, 0);
    fwrite(hdr, 1, hlen, g.out);

    enum { BLK = 512 };
    float L[BLK], R[BLK];
    uint8_t outbuf[BLK * 2 * 4];
    size_t dataBytes = 0;
    size_t frames = 0;
    const size_t maxFrames = (size_t) p.max_seconds * (size_t) p.sample_rate;
    const size_t maxBytes = 400u * 1024u * 1024u;  // страховка от гигантских файлов
    double peak = 0;

    auto emit = [&](int n) -> bool {
        uint8_t* o = outbuf;
        for (int i = 0; i < n; i++) {
            float l = L[i], r = R[i];
            double a = fabs((double) l), b = fabs((double) r);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
            float v[2];
            if (p.mono) { v[0] = 0.5f * (l + r); }
            else { v[0] = l; v[1] = r; }
            for (int c = 0; c < ch; c++) {
                float x = v[c];
                if (p.bits == 32) {
                    memcpy(o, &x, 4);  // little-endian хост (ARM/x86)
                    o += 4;
                } else if (p.bits == 24) {
                    if (x > 1.0f) x = 1.0f;
                    if (x < -1.0f) x = -1.0f;
                    int32_t q = (int32_t) lrintf(x * 8388607.0f);
                    o[0] = (uint8_t) q; o[1] = (uint8_t) (q >> 8); o[2] = (uint8_t) (q >> 16);
                    o += 3;
                } else {
                    float d = rnd01() + rnd01() - 1.0f;  // TPDF-дизеринг ±1 LSB
                    float y = x * 32767.0f + d;
                    if (y > 32767.0f) y = 32767.0f;
                    if (y < -32768.0f) y = -32768.0f;
                    int16_t q = (int16_t) lrintf(y);
                    o[0] = (uint8_t) q; o[1] = (uint8_t) ((uint16_t) q >> 8);
                    o += 2;
                }
            }
        }
        size_t bytes = (size_t) (o - outbuf);
        if (fwrite(outbuf, 1, bytes, g.out) != bytes) return false;
        dataBytes += bytes;
        return true;
    };
    (void) bps;

    fluid_player_play(g.player);
    bool diskOk = true;
    int blocks = 0;
    while (fluid_player_get_status(g.player) == FLUID_PLAYER_PLAYING) {
        if (__atomic_load_n(&job.cancel, __ATOMIC_ACQUIRE)) { job.state = kCancelled; return -2; }
        if (fluid_synth_write_float(g.synth, BLK, L, 0, 1, R, 0, 1) != FLUID_OK) {
            setError(job, "ошибка синтеза");
            job.state = kFailed;
            return -1;
        }
        if (!emit(BLK)) { diskOk = false; break; }
        frames += BLK;
        if ((++blocks & 15) == 0) {
            int total = fluid_player_get_total_ticks(g.player);
            if (total > 0) {
                int pm = (int) (1000.0 * fluid_player_get_current_tick(g.player) / total);
                if (pm > 990) pm = 990;
                if (pm < 0) pm = 0;
                __atomic_store_n(&job.permille, pm, __ATOMIC_RELEASE);
            }
        }
        if (frames >= maxFrames || dataBytes >= maxBytes) { job.truncated = 1; break; }
    }

    // --- хвост: ждём затухания ревербератора/релизов ---
    if (diskOk && !job.truncated) {
        const size_t tailMax = (size_t) p.tail_max_s * (size_t) p.sample_rate;
        const size_t quiet = (size_t) (0.3 * p.sample_rate);
        size_t tail = 0, silentRun = 0;
        while (tail < tailMax && silentRun < quiet) {
            if (__atomic_load_n(&job.cancel, __ATOMIC_ACQUIRE)) { job.state = kCancelled; return -2; }
            fluid_synth_write_float(g.synth, BLK, L, 0, 1, R, 0, 1);
            float m = 0;
            for (int i = 0; i < BLK; i++) {
                float a = fabsf(L[i]), b = fabsf(R[i]);
                if (a > m) m = a;
                if (b > m) m = b;
            }
            if (!emit(BLK)) { diskOk = false; break; }
            frames += BLK;
            tail += BLK;
            if (m < 1e-4f) silentRun += BLK; else silentRun = 0;
        }
    }

    // --- финализация заголовка ---
    hlen = wavHeader(hdr, ch, p.sample_rate, p.bits, dataBytes);
    fseek(g.out, 0, SEEK_SET);
    fwrite(hdr, 1, hlen, g.out);
    int closeRc = fclose(g.out);
    g.out = nullptr;
    if (!diskOk || closeRc != 0) {
        snprintf(job.error, sizeof(job.error), "ошибка записи на диск (нет места?)");
        job.state = kFailed;
        return -1;
    }

    job.seconds = (double) frames / (double) p.sample_rate;
    job.bytes = dataBytes + hlen;
    job.peak = peak;
    job.wall = nowSec() - t0;
    __atomic_store_n(&job.permille, 1000, __ATOMIC_RELEASE);
    if (frames <= (size_t) BLK * 2) appendStr(job.warnings, sizeof(job.warnings), "результат почти пустой — в MIDI нет нот?");
    job.state = kDone;
    return 0;
}

int listPresets(const char* sfPath, Job& job) {
    Guard g;
    job.state = kRunning;
    job.error[0] = 0;
    job.warnings[0] = 0;
    job.text = nullptr;
    g_lastErr[0] = 0;
    g_logJob = &job;

    g.st = new_fluid_settings();
    if (!g.st) { snprintf(job.error, sizeof(job.error), "нет памяти"); job.state = kFailed; return -1; }
    fluid_settings_setint(g.st, "synth.lock-memory", 0);
    fluid_settings_setint(g.st, "synth.dynamic-sample-loading", 1);
    fluid_settings_setint(g.st, "synth.reverb.active", 0);
    fluid_settings_setint(g.st, "synth.chorus.active", 0);
    g.synth = new_fluid_synth(g.st);
    if (!g.synth || fluid_synth_sfload(g.synth, sfPath, 0) < 0) {
        setError(job, "не удалось открыть SoundFont");
        job.state = kFailed;
        return -1;
    }
    fluid_sfont_t* sf = fluid_synth_get_sfont(g.synth, 0);
    size_t cap = 4096, len = 0;
    char* buf = static_cast<char*>(malloc(cap));
    if (!sf || !buf) { free(buf); snprintf(job.error, sizeof(job.error), "нет памяти"); job.state = kFailed; return -1; }
    buf[0] = 0;
    fluid_sfont_iteration_start(sf);
    while (fluid_preset_t* pr = fluid_sfont_iteration_next(sf)) {
        char row[200];
        int n = snprintf(row, sizeof(row), "%03d-%03d  %s\n", fluid_preset_get_banknum(pr), fluid_preset_get_num(pr),
                         fluid_preset_get_name(pr));
        if (n < 0) continue;
        if (len + (size_t) n + 1 > cap) {
            if (cap >= (1u << 20)) break;
            cap *= 2;
            char* nb = static_cast<char*>(realloc(buf, cap));
            if (!nb) break;
            buf = nb;
        }
        memcpy(buf + len, row, (size_t) n);
        len += (size_t) n;
        buf[len] = 0;
    }
    job.text = buf;
    job.state = kDone;
    return 0;
}

}  // namespace fsr
