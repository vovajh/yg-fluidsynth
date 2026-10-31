// Движок рендера: MIDI + SoundFont(ы) -> WAV на libfluidsynth. Без STL, без зависимости от Yougram.
#pragma once
#include <stddef.h>

namespace fsr {

enum { kMaxSf = 8, kMaxOvr = 32, kPath = 512 };

struct ProgOverride {
    int chan;  // 0..15
    int bank;  // -1 = не менять банк
    int prog;  // 0..127
};

struct Params {
    char midi_path[kPath];
    char out_path[kPath];
    char sf_paths[kMaxSf][kPath];
    int sf_count;
    int sample_rate;
    double gain;
    double speed;       // множитель темпа (fluid_player_set_tempo INTERNAL)
    bool reverb, chorus, limiter, mono;
    int bits;           // 16, 24, 32 (float)
    int interp;         // 0, 1, 4, 7
    int polyphony;
    int tail_max_s;     // максимум «хвоста» после конца MIDI
    int max_seconds;    // жёсткий лимит длины
    char reverb_engine[16];
    char bank_select[8];
    char extra[2048];   // "ключ=значение\n..." — произвольные synth.*/player.*
    ProgOverride ovr[kMaxOvr];
    int ovr_count;
};

enum JobState { kIdle = 0, kRunning = 1, kDone = 2, kFailed = 3, kCancelled = 4 };

struct Job {
    int state;
    int permille;
    int cancel;
    double seconds;     // длительность результата
    double wall;        // сколько считалось
    double peak;        // пик (0..1+), до лимитера уже учтён синтезатором
    size_t bytes;
    int truncated;      // упёрлись в max_seconds
    char error[320];
    char warnings[320];
    char* text;         // для listPresets (malloc, освобождает владелец)
};

// Блокирующий рендер. Вернёт 0 при успехе. Одновременно — только один вызов render/listPresets.
int render(const Params& p, Job& job);

// Текст со списком пресетов "BBB-PPP  Имя". Блокирующий. job.text = malloc-буфер.
int listPresets(const char* sfPath, Job& job);

// Проверка произвольной настройки FluidSynth (synth.* / player.*) без побочных эффектов.
bool validateSetting(const char* key, const char* value, char* err, size_t cap);

// Логирование fluidsynth (вызвать один раз при загрузке плагина).
void initLogging();

}  // namespace fsr
