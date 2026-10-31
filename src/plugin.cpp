// FluidSynth для Yougram: рендер MIDI -> WAV прямо в чате.
// Команда /fs (алиас /fluid). Сам рендер — в renderer.cpp (libfluidsynth), здесь только «клей» с хостом.
//
// Потоки: все вызовы host->call делаются ТОЛЬКО из служебного потока Yougram (on_event / таймер).
// Рендер идёт в отдельном pthread, прогресс и результат подбирает таймер fs_poll на служебном потоке.
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "renderer.h"
#include "yougram_plugin.h"

extern "C" {
extern const unsigned char yg_default_sf2[];
extern const size_t yg_default_sf2_size;
}

namespace {

// =============================================================== базовое
const YgHost* g_host = nullptr;
char g_data[400];
int g_seq = 0;

void logI(const char* m) { yg_log(g_host, YG_LOG_INFO, m); }
void logW(const char* m) { yg_log(g_host, YG_LOG_WARN, m); }

char* hcall(const char* method, const char* args) { return yg_call(g_host, method, args); }

// Вызов с проверкой ok; при ошибке кладёт текст в err.
bool hok(const char* method, const char* args, char* err = nullptr, size_t cap = 0) {
    char* r = hcall(method, args);
    bool ok = yg_ok(r);
    if (!ok) {
        if (err && cap) {
            if (!r || yg_json_get_string(r, "error", err, cap) < 0) snprintf(err, cap, "нет ответа от хоста");
        }
        logW(r ? r : "host->call: нет ответа");
    }
    yg_free(g_host, r);
    return ok;
}

struct Out {
    char b[3900];
    size_t n = 0;
    Out() { b[0] = 0; }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (n + 1 >= sizeof(b)) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(b + n, sizeof(b) - n, fmt, ap);
        va_end(ap);
        if (w > 0) n = (n + (size_t) w < sizeof(b)) ? n + (size_t) w : sizeof(b) - 1;
    }
};

char* escAlloc(const char* s) {
    size_t n = strlen(s);
    char* o = static_cast<char*>(malloc(n * 6 + 1));
    if (!o) return nullptr;
    if (yg_json_escape(s, o, n * 6 + 1) < 0) { free(o); return nullptr; }
    return o;
}

long long idFromResponse(const char* r) {
    if (!r) return -1;
    long long v = yg_json_get_int64(r, "message_id", -1);
    if (v < 0) v = yg_json_get_int64(r, "id", -1);
    return v;
}

bool sendMsg(long long chat, const char* text, long long* idOut = nullptr) {
    char* e = escAlloc(text);
    if (!e) return false;
    size_t cap = strlen(e) + 96;
    char* a = static_cast<char*>(malloc(cap));
    if (!a) { free(e); return false; }
    snprintf(a, cap, "{\"chat_id\":%lld,\"text\":\"%s\"}", chat, e);
    char* r = hcall("send_message", a);
    bool ok = yg_ok(r);
    if (idOut) *idOut = ok ? idFromResponse(r) : -1;
    if (!ok) logW(r ? r : "send_message: нет ответа");
    yg_free(g_host, r);
    free(a);
    free(e);
    return ok;
}

bool editMsg(long long chat, long long id, const char* text) {
    char* e = escAlloc(text);
    if (!e) return false;
    size_t cap = strlen(e) + 128;
    char* a = static_cast<char*>(malloc(cap));
    if (!a) { free(e); return false; }
    snprintf(a, cap, "{\"chat_id\":%lld,\"message_id\":%lld,\"text\":\"%s\"}", chat, id, e);
    bool ok = hok("edit_message", a);
    free(a);
    free(e);
    return ok;
}

bool sendDoc(long long chat, const char* path, const char* caption, char* err, size_t ecap) {
    char* ep = escAlloc(path);
    char* ec = escAlloc(caption);
    if (!ep || !ec) { free(ep); free(ec); return false; }
    size_t cap = strlen(ep) + strlen(ec) + 128;
    char* a = static_cast<char*>(malloc(cap));
    if (!a) { free(ep); free(ec); return false; }
    snprintf(a, cap, "{\"chat_id\":%lld,\"path\":\"%s\",\"caption\":\"%s\"}", chat, ep, ec);
    bool ok = hok("send_document", a, err, ecap);
    free(a);
    free(ep);
    free(ec);
    return ok;
}

void toast(const char* title, const char* msg) {
    char* et = escAlloc(title);
    char* em = escAlloc(msg);
    if (et && em) {
        size_t cap = strlen(et) + strlen(em) + 64;
        char* a = static_cast<char*>(malloc(cap));
        if (a) {
            snprintf(a, cap, "{\"title\":\"%s\",\"message\":\"%s\"}", et, em);
            hok("toast", a);
            free(a);
        }
    }
    free(et);
    free(em);
}

// =============================================================== JSON-хелперы
// Конец JSON-значения, начинающегося в v.
const char* valueEnd(const char* v) {
    if (!v) return nullptr;
    if (*v == '"') {
        v++;
        while (*v && *v != '"') v += (*v == '\\' && v[1]) ? 2 : 1;
        return *v ? v + 1 : v;
    }
    if (*v == '{' || *v == '[') {
        int depth = 0;
        while (*v) {
            if (*v == '"') {
                v = valueEnd(v);
                continue;
            }
            if (*v == '{' || *v == '[') depth++;
            if (*v == '}' || *v == ']') {
                if (--depth == 0) return v + 1;
            }
            v++;
        }
        return v;
    }
    while (*v && *v != ',' && *v != '}' && *v != ']' && !isspace((unsigned char) *v)) v++;
    return v;
}

// Копия значения ключа (как JSON-текст, объект/массив целиком). malloc, либо nullptr.
char* valueCopy(const char* json, const char* key) {
    const char* v = yg_json_find(json, key);
    if (!v) return nullptr;
    const char* e = valueEnd(v);
    if (!e || e <= v) return nullptr;
    size_t n = (size_t) (e - v);
    char* o = static_cast<char*>(malloc(n + 1));
    if (!o) return nullptr;
    memcpy(o, v, n);
    o[n] = 0;
    return o;
}

bool isEmptyJson(const char* s) {
    if (!s || !*s) return true;
    if (!strcmp(s, "null") || !strcmp(s, "{}") || !strcmp(s, "[]") || !strcmp(s, "\"\"") || !strcmp(s, "false")) return true;
    return false;
}

// Значение ключа любого типа в виде строки (для settings_all: строка/число/bool).
bool tokenValue(const char* json, const char* key, char* out, size_t cap) {
    const char* v = yg_json_find(json, key);
    if (!v) return false;
    if (*v == '"') return yg_json_get_string(json, key, out, cap) >= 0;
    const char* e = valueEnd(v);
    size_t n = (size_t) (e - v);
    if (n == 0 || n >= cap) return false;
    memcpy(out, v, n);
    out[n] = 0;
    return true;
}

// =============================================================== файловая система
void mkdirp(const char* path) { mkdir(path, 0700); }

void pathIn(char* out, size_t cap, const char* sub, const char* name = nullptr) {
    if (name) snprintf(out, cap, "%s/%s/%s", g_data, sub, name);
    else snprintf(out, cap, "%s/%s", g_data, sub);
}

bool readFile(const char* path, char* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { buf[0] = 0; return false; }
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return true;
}

bool writeFile(const char* path, const char* data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(data);
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

long long fileSize(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long) st.st_size;
}

bool copyFile(const char* from, const char* to, long long maxBytes) {
    FILE* in = fopen(from, "rb");
    if (!in) return false;
    FILE* out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    long long total = 0;
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        total += (long long) n;
        if (total > maxBytes || fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) unlink(to);
    return ok;
}

enum Kind { kUnknown = 0, kMidi, kSf, kDls, kWav };

Kind sniff(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return kUnknown;
    unsigned char h[12] = {0};
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n >= 4 && !memcmp(h, "MThd", 4)) return kMidi;
    if (n >= 12 && !memcmp(h, "RIFF", 4)) {
        if (!memcmp(h + 8, "RMID", 4)) return kMidi;
        if (!memcmp(h + 8, "sfbk", 4)) return kSf;
        if (!memcmp(h + 8, "DLS ", 4)) return kDls;
        if (!memcmp(h + 8, "WAVE", 4)) return kWav;
    }
    return kUnknown;
}

const char* kindName(Kind k) {
    switch (k) {
        case kMidi: return "MIDI";
        case kSf: return "SoundFont";
        case kDls: return "DLS";
        case kWav: return "WAV (аудио)";
        default: return "неизвестный формат";
    }
}

// Только [A-Za-z0-9._-], остальное -> '_'.
void sanitize(const char* in, char* out, size_t cap) {
    size_t j = 0;
    bool lastUnd = false;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char) in[i];
        bool good = isalnum(c) && c < 128;
        if (good || c == '.' || c == '-' || c == '_') { out[j++] = (char) c; lastUnd = false; }
        else if (!lastUnd) { out[j++] = '_'; lastUnd = true; }
    }
    out[j] = 0;
    while (out[0] == '.' || out[0] == '_') memmove(out, out + 1, strlen(out));  // без скрытых файлов
    if (!out[0]) snprintf(out, cap, "file");
}

void stripExt(char* s) {
    char* d = strrchr(s, '.');
    if (d && d != s) *d = 0;
}

bool hasExt(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && strcasecmp(name + n - e, ext) == 0;
}

struct DirEnt {
    char name[96];
    long long size;
    time_t mtime;
};

int listDir(const char* dir, DirEnt* out, int max, bool (*filter)(const char*)) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (filter && !filter(e->d_name)) continue;
        if (n >= max) break;
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].size = (long long) st.st_size;
        out[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    return n;
}

bool isSfName(const char* n) { return hasExt(n, ".sf2") || hasExt(n, ".sf3") || hasExt(n, ".dls"); }

void sortByName(DirEnt* a, int n) {
    for (int i = 1; i < n; i++) {
        DirEnt t = a[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(a[j].name, t.name) > 0) { a[j + 1] = a[j]; j--; }
        a[j + 1] = t;
    }
}

// Оставить keep самых новых файлов в каталоге, остальные удалить. olderThan>0 — удалять и всё старше N секунд.
void prune(const char* sub, int keep, long olderThan) {
    char dir[500];
    pathIn(dir, sizeof(dir), sub);
    static DirEnt e[64];
    int n = listDir(dir, e, 64, nullptr);
    for (int i = 1; i < n; i++) {  // новые первыми
        DirEnt t = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].mtime < t.mtime) { e[j + 1] = e[j]; j--; }
        e[j + 1] = t;
    }
    time_t now = time(nullptr);
    for (int i = 0; i < n; i++) {
        bool drop = i >= keep || (olderThan > 0 && now - e[i].mtime > olderThan);
        if (drop) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", dir, e[i].name);
            unlink(p);
        }
    }
}

void fmtSize(long long b, char* out, size_t cap) {
    if (b >= 1024LL * 1024) snprintf(out, cap, "%.1f МБ", (double) b / (1024.0 * 1024.0));
    else snprintf(out, cap, "%lld КБ", (b + 1023) / 1024);
}

void fmtTime(double s, char* out, size_t cap) {
    int t = (int) (s + 0.5);
    snprintf(out, cap, "%d:%02d", t / 60, t % 60);
}

// =============================================================== настройки
struct SDef {
    const char* key;
    const char* aliases;  // через запятую
    char type;            // s=switch, n=number(int), t=text(число строкой), c=choice
    const char* choices;
    double lo, hi;
};

const SDef kDefs[] = {
    {"gain", "g", 't', nullptr, 0.0, 10.0},
    {"rate", "r", 'c', "22050,32000,44100,48000,96000", 0, 0},
    {"bits", "b,O", 'c', "16,24,32", 0, 0},
    {"mono", "", 's', nullptr, 0, 0},
    {"reverb", "R", 's', nullptr, 0, 0},
    {"chorus", "C", 's', nullptr, 0, 0},
    {"limiter", "", 's', nullptr, 0, 0},
    {"interp", "i", 'c', "0,1,4,7", 0, 0},
    {"poly", "p,polyphony", 'n', nullptr, 16, 2048},
    {"tail", "", 'n', nullptr, 0, 30},
    {"maxmin", "", 'n', nullptr, 1, 60},
    {"speed", "tempo", 't', nullptr, 0.1, 4.0},
    {"engine", "reverb-engine", 'c', "dat,fdn,free,lex,smith", 0, 0},
    {"bank", "bank-select", 'c', "gs,gm,xg,mma", 0, 0},
};
const int kNDefs = sizeof(kDefs) / sizeof(kDefs[0]);

// Короткие имена для частых synth.*-настроек (как команды reverb/chorus в shell FluidSynth).
struct Alias { const char* a; const char* full; };
const Alias kAliases[] = {
    {"room", "synth.reverb.room-size"}, {"damp", "synth.reverb.damp"},   {"width", "synth.reverb.width"},
    {"rlevel", "synth.reverb.level"},   {"cn", "synth.chorus.nr"},       {"clevel", "synth.chorus.level"},
    {"cspeed", "synth.chorus.speed"},   {"cdepth", "synth.chorus.depth"}, {"limit", "synth.limiter.output-limit"},
};

struct Cfg {
    double gain = 1.0, speed = 1.0;
    int rate = 44100, bits = 16, interp = 4, poly = 256, tail = 4, maxmin = 15;
    bool mono = false, reverb = true, chorus = true, limiter = true;
    char engine[16] = "dat";
    char bank[8] = "gs";
};

bool csvHas(const char* csv, const char* v) {
    size_t n = strlen(v);
    for (const char* p = csv; *p;) {
        const char* e = strchr(p, ',');
        size_t l = e ? (size_t) (e - p) : strlen(p);
        if (l == n && !strncmp(p, v, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

const SDef* findDef(const char* name) {
    for (int i = 0; i < kNDefs; i++) {
        if (!strcmp(kDefs[i].key, name)) return &kDefs[i];
        if (kDefs[i].aliases[0] && csvHas(kDefs[i].aliases, name)) return &kDefs[i];
    }
    return nullptr;
}

bool parseBoolS(const char* v, bool* out) {
    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "да")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "нет")) { *out = false; return true; }
    return false;
}

// Проверяет значение и пишет в cfg; norm — нормализованная строка для сохранения.
bool cfgApply(Cfg& c, const SDef* d, const char* val, char* norm, size_t ncap, char* err, size_t ecap) {
    const char* k = d->key;
    if (d->type == 's') {
        bool b;
        if (!parseBoolS(val, &b)) { snprintf(err, ecap, "%s: нужно on или off", k); return false; }
        snprintf(norm, ncap, "%s", b ? "true" : "false");
        if (!strcmp(k, "mono")) c.mono = b;
        else if (!strcmp(k, "reverb")) c.reverb = b;
        else if (!strcmp(k, "chorus")) c.chorus = b;
        else if (!strcmp(k, "limiter")) c.limiter = b;
        return true;
    }
    if (d->type == 'c') {
        if (!csvHas(d->choices, val)) { snprintf(err, ecap, "%s: допустимо %s", k, d->choices); return false; }
        snprintf(norm, ncap, "%s", val);
        if (!strcmp(k, "rate")) c.rate = atoi(val);
        else if (!strcmp(k, "bits")) c.bits = atoi(val);
        else if (!strcmp(k, "interp")) c.interp = atoi(val);
        else if (!strcmp(k, "engine")) snprintf(c.engine, sizeof(c.engine), "%s", val);
        else if (!strcmp(k, "bank")) snprintf(c.bank, sizeof(c.bank), "%s", val);
        return true;
    }
    char* end = nullptr;
    double v = strtod(val, &end);
    if (end == val || *end) { snprintf(err, ecap, "%s: нужно число", k); return false; }
    if (v < d->lo || v > d->hi) { snprintf(err, ecap, "%s: допустимо от %g до %g", k, d->lo, d->hi); return false; }
    if (d->type == 'n') snprintf(norm, ncap, "%d", (int) (v + 0.5));
    else snprintf(norm, ncap, "%g", v);
    if (!strcmp(k, "gain")) c.gain = v;
    else if (!strcmp(k, "speed")) c.speed = v;
    else if (!strcmp(k, "poly")) c.poly = (int) (v + 0.5);
    else if (!strcmp(k, "tail")) c.tail = (int) (v + 0.5);
    else if (!strcmp(k, "maxmin")) c.maxmin = (int) (v + 0.5);
    return true;
}

void loadCfg(Cfg& c) {
    c = Cfg();
    char* r = hcall("settings_all", "{}");
    if (r) {
        for (int i = 0; i < kNDefs; i++) {
            char val[64], norm[64], err[100];
            if (tokenValue(r, kDefs[i].key, val, sizeof(val))) cfgApply(c, &kDefs[i], val, norm, sizeof(norm), err, sizeof(err));
        }
    }
    yg_free(g_host, r);
}

bool saveCfgKey(const SDef* d, const char* norm) {
    char args[200];
    if (d->type == 's' || d->type == 'n') snprintf(args, sizeof(args), "{\"key\":\"%s\",\"value\":%s}", d->key, norm);
    else snprintf(args, sizeof(args), "{\"key\":\"%s\",\"value\":\"%s\"}", d->key, norm);
    return hok("settings_set", args);
}

// ---- состояние в файлах data_dir: стек SoundFont, принудительные инструменты, доп. настройки FluidSynth
const char* kDefaultSf = "default.sf2";

void stackPath(char* p, size_t c) { pathIn(p, c, "sf_stack.txt"); }

int readStack(char names[][96], int max) {
    char p[500], buf[1024];
    stackPath(p, sizeof(p));
    int n = 0;
    if (readFile(p, buf, sizeof(buf))) {
        char* save = nullptr;
        for (char* t = strtok_r(buf, "\n", &save); t && n < max; t = strtok_r(nullptr, "\n", &save)) {
            if (!*t) continue;
            char full[600];
            pathIn(full, sizeof(full), "soundfonts", t);
            if (fileSize(full) > 0) snprintf(names[n++], 96, "%s", t);
        }
    }
    if (n == 0) snprintf(names[n++], 96, "%s", kDefaultSf);
    return n;
}

void writeStack(char names[][96], int n) {
    char p[500], buf[1024] = "";
    stackPath(p, sizeof(p));
    for (int i = 0; i < n; i++) {
        strncat(buf, names[i], sizeof(buf) - strlen(buf) - 2);
        strcat(buf, "\n");
    }
    writeFile(p, buf);
}

int readOvr(fsr::ProgOverride* o, int max) {
    char p[500], buf[1024];
    pathIn(p, sizeof(p), "prog.txt");
    int n = 0;
    if (!readFile(p, buf, sizeof(buf))) return 0;
    char* save = nullptr;
    for (char* t = strtok_r(buf, "\n", &save); t && n < max; t = strtok_r(nullptr, "\n", &save)) {
        int c, b, pr;
        if (sscanf(t, "%d %d %d", &c, &b, &pr) == 3 && c >= 0 && c < 16 && pr >= 0 && pr < 128) { o[n++] = {c, b, pr}; }
    }
    return n;
}

void writeOvr(const fsr::ProgOverride* o, int n) {
    char p[500], buf[1024] = "";
    pathIn(p, sizeof(p), "prog.txt");
    for (int i = 0; i < n; i++) {
        char l[40];
        snprintf(l, sizeof(l), "%d %d %d\n", o[i].chan, o[i].bank, o[i].prog);
        strncat(buf, l, sizeof(buf) - strlen(buf) - 1);
    }
    writeFile(p, buf);
}

void extraPath(char* p, size_t c) { pathIn(p, c, "extra.txt"); }

// Установить/заменить key=value в extra (value == nullptr -> удалить).
bool extraSet(const char* key, const char* value) {
    char p[500], buf[2048], out[2048] = "";
    extraPath(p, sizeof(p));
    readFile(p, buf, sizeof(buf));
    char* save = nullptr;
    size_t klen = strlen(key);
    for (char* t = strtok_r(buf, "\n", &save); t; t = strtok_r(nullptr, "\n", &save)) {
        if (!strncmp(t, key, klen) && t[klen] == '=') continue;
        if (strlen(out) + strlen(t) + 2 < sizeof(out)) { strcat(out, t); strcat(out, "\n"); }
    }
    if (value) {
        char l[300];
        snprintf(l, sizeof(l), "%s=%s\n", key, value);
        if (strlen(out) + strlen(l) + 1 >= sizeof(out)) return false;
        strcat(out, l);
    }
    return writeFile(p, out);
}

// =============================================================== библиотека SoundFont
int listLib(DirEnt* e, int max) {
    char dir[500];
    pathIn(dir, sizeof(dir), "soundfonts");
    int n = listDir(dir, e, max, isSfName);
    sortByName(e, n);
    return n;
}

// "3" -> третий по списку, иначе поиск по имени (точное, затем уникальная подстрока).
int resolveSf(const char* ref, DirEnt* lib, int n) {
    char* end = nullptr;
    long v = strtol(ref, &end, 10);
    if (end != ref && !*end) return (v >= 1 && v <= n) ? (int) v - 1 : -1;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(lib[i].name, ref)) return i;
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (strcasestr(lib[i].name, ref)) {
            if (found >= 0) return -1;
            found = i;
        }
    }
    return found;
}

// =============================================================== файлы из чата
struct Media {
    char path[512];
    char url[1024];
    char name[128];
};

bool firstString(const char* json, const char* const* keys, char* out, size_t cap) {
    for (int i = 0; keys[i]; i++) {
        const char* v = yg_json_find(json, keys[i]);
        if (v && *v == '"' && yg_json_get_string(json, keys[i], out, cap) > 0) return true;
    }
    return false;
}

const char* const kPathKeys[] = {"local_path", "localPath", "file_path", "path", "file", nullptr};
const char* const kUrlKeys[] = {"download_url", "url", "link", "remote_url", nullptr};
const char* const kNameKeys[] = {"file_name", "filename", "name", "title", nullptr};

bool extractFrom(const char* json, Media& m, bool allowUrl) {
    char tmp[1100];
    if (firstString(json, kPathKeys, tmp, sizeof(tmp))) {
        const char* p = !strncmp(tmp, "file://", 7) ? tmp + 7 : tmp;
        if (p[0] == '/') snprintf(m.path, sizeof(m.path), "%s", p);
    }
    if (allowUrl && firstString(json, kUrlKeys, tmp, sizeof(tmp)) && !strncmp(tmp, "http", 4))
        snprintf(m.url, sizeof(m.url), "%s", tmp);
    firstString(json, kNameKeys, m.name, sizeof(m.name));
    return m.path[0] || m.url[0];
}


// Последний шанс: перебрать ВСЕ строковые значения JSON сообщения. Берём существующий файл на диске
// (путь вида /…/файл) или http-ссылку; имя — строка с расширением .sf2/.mid и т.п.
bool hasKnownExt(const char* n) {
    return hasExt(n, ".sf2") || hasExt(n, ".sf3") || hasExt(n, ".dls") || hasExt(n, ".mid") || hasExt(n, ".midi") ||
           hasExt(n, ".kar") || hasExt(n, ".rmi");
}

bool scanAnyStrings(const char* json, Media& m) {
    const char* p = json;
    while (*p) {
        if (*p != '"') { p++; continue; }
        const char* st = p + 1;
        const char* e = st;
        while (*e && *e != '"') e += (*e == '\\' && e[1]) ? 2 : 1;
        const char* q = e + 1;
        while (*q == ' ') q++;
        bool isKey = (*q == ':');
        size_t n = (size_t) (e - st);
        if (!isKey && n > 3 && n < 1000) {
            char buf[1024];
            size_t j = 0;
            for (const char* c = st; c < e && j + 1 < sizeof(buf); c++) {
                if (*c == '\\' && c + 1 < e) c++;  // \/ -> /, \\ -> \ (достаточно для путей)
                buf[j++] = *c;
            }
            buf[j] = 0;
            const char* path = !strncmp(buf, "file://", 7) ? buf + 7 : buf;
            if (path[0] == '/' && strchr(path + 1, '/') && !m.path[0]) {
                struct stat sb;
                if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) && access(path, R_OK) == 0) snprintf(m.path, sizeof(m.path), "%s", path);
            } else if (!strncmp(buf, "http", 4) && !m.url[0]) {
                snprintf(m.url, sizeof(m.url), "%s", buf);
            } else if (!strchr(buf, '/') && hasKnownExt(buf) && !m.name[0]) {
                snprintf(m.name, sizeof(m.name), "%.120s", buf);
            }
        }
        p = *e ? e + 1 : e;
    }
    if (m.path[0] && !m.name[0]) {
        const char* b = strrchr(m.path, '/');
        snprintf(m.name, sizeof(m.name), "%.120s", b ? b + 1 : m.path);
    }
    return m.path[0] || m.url[0];
}

// Ищет файл в JSON сообщения: сначала в "media", потом в "content", потом пути по всему сообщению.
bool extractMedia(const char* msg, Media& m, char* dbg, size_t dbgCap) {
    memset(&m, 0, sizeof(m));
    char* media = valueCopy(msg, "media");
    bool found = false;
    if (media && !isEmptyJson(media)) {
        found = extractFrom(media, m, true);
        if (dbg) snprintf(dbg, dbgCap, "%.400s", media);
    }
    free(media);
    if (!found) {
        char* content = valueCopy(msg, "content");
        if (content && !isEmptyJson(content)) {
            found = extractFrom(content, m, true);
            if (dbg && !dbg[0]) snprintf(dbg, dbgCap, "content=%.380s", content);
        }
        free(content);
    }
    if (!found) found = extractFrom(msg, m, false);
    if (!found) found = scanAnyStrings(msg, m);
    return found;
}

// Берёт файл из Media в work/: копирует локальный или качает по ссылке. Возвращает путь.
bool fetchMedia(const Media& m, char* outPath, size_t cap, char* err, size_t ecap) {
    char work[500], dest[600];
    pathIn(work, sizeof(work), "work");
    mkdirp(work);
    snprintf(dest, sizeof(dest), "%s/in_%d.bin", work, ++g_seq);
    if (m.path[0]) {
        if (copyFile(m.path, dest, 400LL * 1024 * 1024)) { snprintf(outPath, cap, "%s", dest); return true; }
        if (!m.url[0]) { snprintf(err, ecap, "не могу прочитать файл по пути из сообщения (или он больше 400 МБ)"); return false; }
    }
    if (m.url[0]) {
        char* eu = escAlloc(m.url);
        if (!eu) { snprintf(err, ecap, "нет памяти"); return false; }
        size_t acap = strlen(eu) + 128;
        char* a = static_cast<char*>(malloc(acap));
        if (!a) { free(eu); snprintf(err, ecap, "нет памяти"); return false; }
        snprintf(a, acap, "{\"url\":\"%s\",\"name\":\"in_%d.bin\",\"timeout_ms\":25000}", eu, g_seq);
        free(eu);
        char* r = hcall("http_download", a);
        free(a);
        bool ok = yg_ok(r);
        char path[600] = "";
        if (ok) ok = yg_json_get_string(r, "path", path, sizeof(path)) > 0;
        if (!ok) {
            char e2[160] = "";
            if (r) yg_json_get_string(r, "error", e2, sizeof(e2));
            snprintf(err, ecap, "не удалось скачать: %s", e2[0] ? e2 : "нет ответа");
            yg_free(g_host, r);
            return false;
        }
        yg_free(g_host, r);
        snprintf(outPath, cap, "%s", path);
        return true;
    }
    snprintf(err, ecap, "в сообщении нет пути к файлу");
    return false;
}

// Кольцо последних сообщений с вложениями: позволяет написать «/fs render» после отправки файла без reply.
struct Seen { long long chat, id; time_t ts; };
Seen g_seen[16];
int g_seenN = 0;

void rememberMedia(const char* eventJson) {
    char* msg = valueCopy(eventJson, "message");
    const char* src = msg ? msg : eventJson;
    char* media = valueCopy(src, "media");
    bool has = media && !isEmptyJson(media);
    free(media);
    if (has) {
        long long chat = yg_json_get_int64(src, "chat_id", 0);
        long long id = yg_json_get_int64(src, "id", 0);
        if (chat && id) {
            if (g_seenN == 16) { memmove(g_seen, g_seen + 1, sizeof(Seen) * 15); g_seenN = 15; }
            g_seen[g_seenN++] = {chat, id, time(nullptr)};
        }
    }
    free(msg);
}

bool loadMessage(long long chat, long long id, Media& m, char* err, size_t ecap, char* dbg = nullptr, size_t dcap = 0) {
    char a[96];
    snprintf(a, sizeof(a), "{\"chat_id\":%lld,\"message_id\":%lld}", chat, id);
    char* r = hcall("get_message", a);
    if (!yg_ok(r)) {
        snprintf(err, ecap, "не могу прочитать сообщение (нужно разрешение chat.read)");
        yg_free(g_host, r);
        return false;
    }
    bool ok = extractMedia(r, m, dbg, dcap);
    if (!ok) {
        char* ai = hcall("get_app_info", "{}");
        char* methods = ai ? valueCopy(ai, "methods") : nullptr;
        if (strstr(r, "\"_type\":\"LocalFile\"")) {
            // TDLib: файл есть на сервере, но на телефон не скачан (local path пустой)
            snprintf(err, ecap,
                     "⬇ Файл ещё не скачан на телефон: Yougram пока хранит только ссылку на сервер.\n"
                     "Нажми на файл в чате, дождись загрузки и повтори команду.\n\n[диагностика] методы хоста: %.700s",
                     methods ? methods : "?");
        } else {
            snprintf(err, ecap, "в сообщении не нашёл файл. Ответ get_message (пришли мне этот текст):\n%.900s\n\nметоды хоста: %.400s", r,
                     methods ? methods : "?");
        }
        free(methods);
        yg_free(g_host, ai);
    }
    yg_free(g_host, r);
    return ok;
}

enum Want { kWantAny = 0, kWantMidi, kWantSf };

bool kindMatches(Kind k, Want w) {
    if (w == kWantMidi) return k == kMidi;
    if (w == kWantSf) return k == kSf || k == kDls;
    return k == kMidi || k == kSf || k == kDls;
}

struct Input {
    char path[600];
    char name[128];
    Kind kind;
};

// Ищет входной файл: ссылка из аргумента -> ответ на сообщение -> недавние вложения в чате.
bool obtainInput(long long chat, long long replyTo, const char* url, Want want, Input& in, char* err, size_t ecap) {
    memset(&in, 0, sizeof(in));
    Media m;
    if (url && url[0]) {
        memset(&m, 0, sizeof(m));
        snprintf(m.url, sizeof(m.url), "%s", url);
        const char* slash = strrchr(url, '/');
        if (slash) snprintf(m.name, sizeof(m.name), "%.100s", slash + 1);
        if (!fetchMedia(m, in.path, sizeof(in.path), err, ecap)) return false;
        snprintf(in.name, sizeof(in.name), "%s", m.name);
        in.kind = sniff(in.path);
        return true;
    }
    if (replyTo > 0) {
        if (!loadMessage(chat, replyTo, m, err, ecap)) return false;
        if (!fetchMedia(m, in.path, sizeof(in.path), err, ecap)) return false;
        snprintf(in.name, sizeof(in.name), "%s", m.name);
        in.kind = sniff(in.path);
        return true;
    }
    int tried = 0;
    time_t now = time(nullptr);
    for (int i = g_seenN - 1; i >= 0 && tried < 4; i--) {
        if (g_seen[i].chat != chat || now - g_seen[i].ts > 1800) continue;
        tried++;
        char e2[200];
        if (!loadMessage(chat, g_seen[i].id, m, e2, sizeof(e2))) continue;
        if (!fetchMedia(m, in.path, sizeof(in.path), e2, sizeof(e2))) continue;
        Kind k = sniff(in.path);
        if (kindMatches(k, want)) {
            snprintf(in.name, sizeof(in.name), "%s", m.name);
            in.kind = k;
            return true;
        }
        unlink(in.path);
    }
    snprintf(err, ecap, "не нашёл файл. Ответьте командой на сообщение с файлом или дайте ссылку: /fs render https://…/song.mid");
    return false;
}

// =============================================================== фоновая задача
enum JobKind { kJobRender = 1, kJobInst = 2 };

struct Pending {
    bool active = false;
    bool threadStarted = false;
    int kind = 0;
    long long chat = 0, progressId = -1;
    time_t lastEdit = 0;
    int lastPm = -1;
    char tmpMidi[600] = "";
    char label[128] = "";
    char sfLabel[200] = "";
    char sfName[96] = "";
    Cfg cfg;
    pthread_t th;
    fsr::Job job;
};

Pending g_p;
fsr::Params g_params;
bool g_timerOn = false;

void* workerMain(void*) {
    if (g_p.kind == kJobRender) fsr::render(g_params, g_p.job);
    else fsr::listPresets(g_params.sf_paths[0], g_p.job);
    return nullptr;
}

void timerStop() {
    if (g_timerOn) hok("timer_stop", "{\"id\":\"fs_poll\"}");
    g_timerOn = false;
}

void startJob() {
    memset(&g_p.job, 0, sizeof(g_p.job));
    g_p.job.state = fsr::kRunning;
    g_p.active = true;
    g_p.lastEdit = time(nullptr);
    g_p.lastPm = -1;
    if (pthread_create(&g_p.th, nullptr, workerMain, nullptr) != 0) {
        g_p.active = false;
        sendMsg(g_p.chat, "❌ Не удалось запустить поток рендера");
        return;
    }
    g_p.threadStarted = true;
    if (!g_timerOn) g_timerOn = hok("timer_start", "{\"id\":\"fs_poll\",\"interval_ms\":1000,\"repeat\":true}");
}

void finishJob() {
    pthread_join(g_p.th, nullptr);
    g_p.threadStarted = false;
    g_p.active = false;
    timerStop();
    fsr::Job& j = g_p.job;
    long long chat = g_p.chat;
    if (g_p.tmpMidi[0]) { unlink(g_p.tmpMidi); g_p.tmpMidi[0] = 0; }

    if (g_p.kind == kJobInst) {
        if (j.state == fsr::kDone && j.text) {
            size_t len = strlen(j.text);
            if (len < 3000) {
                Out o;
                o.add("🎹 Инструменты %s\nФормат: банк-программа  имя\n\n%s", g_p.sfName, j.text);
                sendMsg(chat, o.b);
            } else {
                char fn[200], path[600], err[200];
                snprintf(fn, sizeof(fn), "presets_%s.txt", g_p.sfName);
                pathIn(path, sizeof(path), "out", fn);
                writeFile(path, j.text);
                char cap[160];
                snprintf(cap, sizeof(cap), "🎹 Инструменты %s (банк-программа  имя)", g_p.sfName);
                if (!sendDoc(chat, path, cap, err, sizeof(err))) sendMsg(chat, "❌ Не получилось отправить список инструментов");
            }
        } else {
            Out o;
            o.add("❌ %s", j.error[0] ? j.error : "не удалось прочитать SoundFont");
            sendMsg(chat, o.b);
        }
        free(j.text);
        j.text = nullptr;
        return;
    }

    if (j.state == fsr::kCancelled) { sendMsg(chat, "⛔ Рендер отменён"); return; }
    if (j.state != fsr::kDone) {
        Out o;
        o.add("❌ Рендер не удался: %s", j.error[0] ? j.error : "неизвестная ошибка");
        if (j.warnings[0]) o.add("\n⚠ %s", j.warnings);
        sendMsg(chat, o.b);
        return;
    }

    char tm[16], sz[24], err[240];
    fmtTime(j.seconds, tm, sizeof(tm));
    fmtSize((long long) j.bytes, sz, sizeof(sz));
    const Cfg& c = g_p.cfg;
    double db = j.peak > 1e-6 ? 20.0 * log10(j.peak) : -99.0;
    Out cap;
    cap.add("🎹 %s\n", g_p.label);
    cap.add("⏱ %s · %g кГц · %d-bit%s %s · %s\n", tm, c.rate / 1000.0, c.bits == 32 ? 32 : c.bits, c.bits == 32 ? " float" : "",
            c.mono ? "mono" : "stereo", sz);
    cap.add("🎼 %s\n", g_p.sfLabel);
    cap.add("⚙ gain %g · reverb %s · chorus %s · limiter %s · interp %d", c.gain, c.reverb ? c.engine : "off",
            c.chorus ? "on" : "off", c.limiter ? "on" : "off", c.interp);
    if (fabs(c.speed - 1.0) > 1e-6) cap.add(" · tempo ×%g", c.speed);
    cap.add("\n⚡ %.1f с (×%.0f от реального времени) · пик %.1f dBFS", j.wall, j.wall > 0 ? j.seconds / j.wall : 0.0, db);
    if (j.truncated) cap.add("\n✂ обрезано по лимиту %d мин (настройка maxmin)", c.maxmin);
    if (j.warnings[0]) cap.add("\n⚠ %.200s", j.warnings);

    if (!sendDoc(chat, g_params.out_path, cap.b, err, sizeof(err))) {
        Out o;
        o.add("❌ Рендер готов (%s, %s), но отправить файл не вышло: %s", tm, sz, err);
        o.add("\nПопробуйте уменьшить размер: /fs render -r 22050 -b 16 --mono");
        sendMsg(chat, o.b);
    }
    prune("out", 3, 0);
}

void pollJob() {
    if (!g_p.active) { timerStop(); return; }
    int st = __atomic_load_n(&g_p.job.state, __ATOMIC_ACQUIRE);
    if (st == fsr::kRunning) {
        if (g_p.kind != kJobRender || g_p.progressId < 0) return;
        int pm = __atomic_load_n(&g_p.job.permille, __ATOMIC_ACQUIRE);
        time_t now = time(nullptr);
        if (now - g_p.lastEdit >= 5 && pm / 10 != g_p.lastPm) {
            char t[200];
            snprintf(t, sizeof(t), "⏳ Рендерю %s… %d%%", g_p.label, pm / 10);
            editMsg(g_p.chat, g_p.progressId, t);
            g_p.lastEdit = now;
            g_p.lastPm = pm / 10;
        }
        return;
    }
    finishJob();
}

void cancelAndJoin() {
    if (g_p.active && g_p.threadStarted) {
        __atomic_store_n(&g_p.job.cancel, 1, __ATOMIC_RELEASE);
        pthread_join(g_p.th, nullptr);
        g_p.threadStarted = false;
        g_p.active = false;
        if (g_p.tmpMidi[0]) { unlink(g_p.tmpMidi); g_p.tmpMidi[0] = 0; }
        free(g_p.job.text);
        g_p.job.text = nullptr;
    }
    g_timerOn = false;  // при выключении таймеры снимает сам хост
}

// =============================================================== разбор команды
struct Toks {
    char t[24][320];
    int n = 0;
};

void tokenize(const char* s, Toks& k) {
    k.n = 0;
    while (*s && k.n < 24) {
        while (*s && isspace((unsigned char) *s)) s++;
        if (!*s) break;
        size_t j = 0;
        char q = 0;
        if (*s == '"' || *s == '\'') q = *s++;
        while (*s && ((q && *s != q) || (!q && !isspace((unsigned char) *s)))) {
            if (j + 1 < sizeof(k.t[0])) k.t[k.n][j++] = *s;
            s++;
        }
        if (q && *s == q) s++;
        k.t[k.n][j] = 0;
        k.n++;
    }
}

// "args" может быть строкой или массивом строк.
void getArgs(const char* json, char* out, size_t cap) {
    out[0] = 0;
    const char* v = yg_json_find(json, "args");
    if (!v) return;
    if (*v == '"') { yg_json_get_string(json, "args", out, cap); return; }
    if (*v != '[') return;
    size_t j = 0;
    const char* e = valueEnd(v);
    for (const char* p = v + 1; p < e && j + 2 < cap; p++) {
        if (*p != '"') continue;
        if (j) out[j++] = ' ';
        for (p++; p < e && *p != '"' && j + 1 < cap; p++) {
            if (*p == '\\' && p[1]) p++;
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

long long getReplyTo(const char* json) {
    const char* v = yg_json_find(json, "reply_to");
    if (!v) return 0;
    if ((*v >= '0' && *v <= '9')) return strtoll(v, nullptr, 10);
    if (*v == '{') {
        char* o = valueCopy(json, "reply_to");
        long long id = 0;
        if (o) {
            id = yg_json_get_int64(o, "message_id", 0);
            if (!id) id = yg_json_get_int64(o, "id", 0);
            free(o);
        }
        return id;
    }
    return 0;
}

// =============================================================== сборка параметров рендера
struct RenderOpts {
    char url[1024] = "";
    char sfList[256] = "";
    char outName[64] = "";
    char extra[600] = "";  // -o key=value (на один рендер)
};

bool parseRenderArgs(const Toks& k, int from, Cfg& cfg, RenderOpts& o, char* err, size_t ecap) {
    for (int i = from; i < k.n; i++) {
        const char* a = k.t[i];
        if (!strncmp(a, "http://", 7) || !strncmp(a, "https://", 8)) { snprintf(o.url, sizeof(o.url), "%s", a); continue; }
        if (a[0] != '-') { snprintf(err, ecap, "не понял «%s». Параметры начинаются с «-», см. /fs help", a); return false; }
        const char* name = a + (a[1] == '-' ? 2 : 1);
        char norm[64];
        // булевы флаги
        if (!strcmp(name, "m") || !strcmp(name, "mono")) { cfgApply(cfg, findDef("mono"), "on", norm, sizeof(norm), err, ecap); continue; }
        if (!strcmp(name, "stereo")) { cfgApply(cfg, findDef("mono"), "off", norm, sizeof(norm), err, ecap); continue; }
        if (!strncmp(name, "no-", 3)) {
            const SDef* d = findDef(name + 3);
            if (d && d->type == 's') { cfgApply(cfg, d, "off", norm, sizeof(norm), err, ecap); continue; }
        }
        {
            const SDef* d = findDef(name);
            if (d && d->type == 's') { cfgApply(cfg, d, "on", norm, sizeof(norm), err, ecap); continue; }
        }
        // опции со значением
        if (strcmp(name, "f") && strcmp(name, "sf") && strcmp(name, "n") && strcmp(name, "name") && strcmp(name, "o") && !findDef(name)) {
            snprintf(err, ecap, "неизвестный параметр %s (список: /fs help)", a);
            return false;
        }
        if (i + 1 >= k.n) { snprintf(err, ecap, "у параметра %s нет значения", a); return false; }
        const char* val = k.t[++i];
        if (!strcmp(name, "f") || !strcmp(name, "sf")) { snprintf(o.sfList, sizeof(o.sfList), "%s", val); continue; }
        if (!strcmp(name, "n") || !strcmp(name, "name")) { snprintf(o.outName, sizeof(o.outName), "%s", val); continue; }
        if (!strcmp(name, "o")) {
            if (!strchr(val, '=')) { snprintf(err, ecap, "-o ожидает ключ=значение"); return false; }
            char kk[120];
            snprintf(kk, sizeof(kk), "%s", val);
            *strchr(kk, '=') = 0;
            char e2[160];
            if (!fsr::validateSetting(kk, strchr(val, '=') + 1, e2, sizeof(e2))) { snprintf(err, ecap, "-o %s: %s", kk, e2); return false; }
            strncat(o.extra, val, sizeof(o.extra) - strlen(o.extra) - 2);
            strcat(o.extra, "\n");
            continue;
        }
        const SDef* d = findDef(name);
        if (!d) { snprintf(err, ecap, "неизвестный параметр %s", a); return false; }
        if (!cfgApply(cfg, d, val, norm, sizeof(norm), err, ecap)) return false;
    }
    return true;
}

bool buildParams(const Cfg& cfg, const RenderOpts& o, const char* midiPath, const char* outPath, char* err, size_t ecap,
                 char* sfLabel, size_t slcap, char* sfFirstName, size_t sfnCap) {
    memset(&g_params, 0, sizeof(g_params));
    snprintf(g_params.midi_path, sizeof(g_params.midi_path), "%s", midiPath);
    snprintf(g_params.out_path, sizeof(g_params.out_path), "%s", outPath);
    char names[fsr::kMaxSf][96];
    int n = 0;
    if (o.sfList[0]) {
        static DirEnt lib[64];
        int ln = listLib(lib, 64);
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", o.sfList);
        char* save = nullptr;
        for (char* t = strtok_r(tmp, ",", &save); t && n < fsr::kMaxSf; t = strtok_r(nullptr, ",", &save)) {
            int idx = resolveSf(t, lib, ln);
            if (idx < 0) { snprintf(err, ecap, "SoundFont «%s» не найден или имя неоднозначно (/fs sf list)", t); return false; }
            snprintf(names[n++], 96, "%s", lib[idx].name);
        }
    } else {
        n = readStack(names, fsr::kMaxSf);
    }
    for (int i = 0; i < n; i++) {
        char full[600];
        pathIn(full, sizeof(full), "soundfonts", names[i]);
        snprintf(g_params.sf_paths[i], fsr::kPath, "%s", full);
    }
    g_params.sf_count = n;
    snprintf(sfFirstName, sfnCap, "%s", names[0]);
    snprintf(sfLabel, slcap, "SoundFont: %s%s", names[0], n > 1 ? " + ещё" : "");
    if (n > 1) {
        size_t l = strlen(sfLabel);
        snprintf(sfLabel, slcap, "SoundFont: ");
        for (int i = 0; i < n && strlen(sfLabel) + strlen(names[i]) + 4 < slcap; i++) {
            strcat(sfLabel, names[i]);
            if (i + 1 < n) strcat(sfLabel, " + ");
        }
        (void) l;
    }
    g_params.sample_rate = cfg.rate;
    g_params.gain = cfg.gain;
    g_params.speed = cfg.speed;
    g_params.reverb = cfg.reverb;
    g_params.chorus = cfg.chorus;
    g_params.limiter = cfg.limiter;
    g_params.mono = cfg.mono;
    g_params.bits = cfg.bits;
    g_params.interp = cfg.interp;
    g_params.polyphony = cfg.poly;
    g_params.tail_max_s = cfg.tail;
    g_params.max_seconds = cfg.maxmin * 60;
    snprintf(g_params.reverb_engine, sizeof(g_params.reverb_engine), "%s", cfg.engine);
    snprintf(g_params.bank_select, sizeof(g_params.bank_select), "%s", cfg.bank);
    char p[500];
    extraPath(p, sizeof(p));
    readFile(p, g_params.extra, sizeof(g_params.extra));
    if (o.extra[0] && strlen(g_params.extra) + strlen(o.extra) + 1 < sizeof(g_params.extra)) {
        size_t l = strlen(g_params.extra);
        if (l && g_params.extra[l - 1] != '\n') strcat(g_params.extra, "\n");
        strcat(g_params.extra, o.extra);
    }
    g_params.ovr_count = readOvr(g_params.ovr, fsr::kMaxOvr);
    return true;
}

// =============================================================== подкоманды
const char* kHelp =
    "🎹 FluidSynth — рендер MIDI в WAV (команда /fs, алиас /fluid)\n\n"
    "Рендер:\n"
    "/fs render — ответом на .mid (или сразу после отправки файла)\n"
    "/fs render <ссылка на .mid> — скачать и отрендерить\n"
    "Параметры (как у fluidsynth -F): -r частота, -g усиление, -b 16|24|32 (32=float), -m моно, -s темп ×, "
    "-i 0|1|4|7 интерполяция, -t хвост (с), --no-reverb, --no-chorus, --no-limiter, --engine dat|fdn|free|lex|smith, "
    "-f список_SF, -n имя, -o synth.ключ=знач\n"
    "Пример: /fs render -r 48000 -g 0.8 -b 24 --no-chorus\n\n"
    "SoundFont (shell: load/unload/fonts/inst):\n"
    "/fs sf list — библиотека и активные\n"
    "/fs sf add — ответом на .sf2/.dls или со ссылкой (станет активным)\n"
    "/fs sf use 1 2 — активные по порядку (поздние перекрывают ранние)\n"
    "/fs sf unload N · /fs sf del N · /fs sf reset\n"
    "/fs sf inst [N] — список инструментов (банк-программа)\n\n"
    "Инструменты каналов (shell: prog/select):\n"
    "/fs prog 1 25 [банк] — канал 1 = программа 25 (нумерация с 1; ударные — канал 10, банк 128)\n"
    "/fs prog clear [канал]\n\n"
    "Настройки (shell: gain/reverb/chorus/interp/set):\n"
    "/fs set — показать; /fs set gain 0.8; /fs set reverb off\n"
    "/fs set room 0.9 (также damp width rlevel cn clevel cspeed cdepth limit)\n"
    "/fs set synth.reverb.room-size 0.5 — любая synth.*/player.*\n"
    "/fs set unset <ключ> · /fs set reset\n\n"
    "/fs cancel · /fs status · /fs info (ответом на файл — диагностика)\n"
    "Те же основные настройки есть в экране настроек плагина.";

void cmdSfList(long long chat) {
    static DirEnt lib[64];
    int n = listLib(lib, 64);
    char names[fsr::kMaxSf][96];
    int sn = readStack(names, fsr::kMaxSf);
    Out o;
    o.add("🎼 SoundFont-ы (✅ активные, номер = порядок загрузки):\n");
    for (int i = 0; i < n; i++) {
        int pos = -1;
        for (int j = 0; j < sn; j++)
            if (!strcmp(names[j], lib[i].name)) pos = j + 1;
        char sz[24];
        fmtSize(lib[i].size, sz, sizeof(sz));
        o.add("%d. %s (%s)", i + 1, lib[i].name, sz);
        if (pos > 0) o.add(" ✅%d", pos);
        o.add("\n");
    }
    o.add("\nДобавить: /fs sf add (ответом на файл). Выбрать: /fs sf use N");
    sendMsg(chat, o.b);
}

void cmdSf(long long chat, long long replyTo, const Toks& k) {
    const char* sub = k.n > 1 ? k.t[1] : "list";
    static DirEnt lib[64];
    if (!strcmp(sub, "list") || !strcmp(sub, "fonts")) { cmdSfList(chat); return; }

    if (!strcmp(sub, "add") || !strcmp(sub, "load")) {
        char err[1400];
        Input in;
        const char* url = (k.n > 2 && !strncmp(k.t[2], "http", 4)) ? k.t[2] : nullptr;
        if (!obtainInput(chat, replyTo, url, kWantSf, in, err, sizeof(err))) { sendMsg(chat, err); return; }
        if (in.kind != kSf && in.kind != kDls) {
            Out o;
            o.add("❌ Это %s, а не SoundFont (.sf2/.dls).", kindName(in.kind));
            sendMsg(chat, o.b);
            unlink(in.path);
            return;
        }
        char base[128];
        sanitize(in.name[0] ? in.name : "soundfont", base, sizeof(base));
        stripExt(base);
        if (strlen(base) > 60) base[60] = 0;
        const char* ext = in.kind == kDls ? ".dls" : (hasExt(in.name, ".sf3") ? ".sf3" : ".sf2");
        char final[160], full[600];
        for (int i = 1;; i++) {
            if (i == 1) snprintf(final, sizeof(final), "%s%s", base, ext);
            else snprintf(final, sizeof(final), "%s_%d%s", base, i, ext);
            pathIn(full, sizeof(full), "soundfonts", final);
            if (fileSize(full) < 0 || i > 50) break;
        }
        if (rename(in.path, full) != 0) {
            if (!copyFile(in.path, full, 600LL * 1024 * 1024)) { sendMsg(chat, "❌ Не удалось сохранить SoundFont (нет места?)"); unlink(in.path); return; }
            unlink(in.path);
        }
        char names[fsr::kMaxSf][96];
        snprintf(names[0], 96, "%s", final);
        writeStack(names, 1);
        char sz[24];
        fmtSize(fileSize(full), sz, sizeof(sz));
        Out o;
        o.add("✅ %s (%s) добавлен и теперь активный.\nПроверить инструменты: /fs sf inst\nПоверх стандартного: /fs sf use default.sf2 %s", final, sz, final);
        if (hasExt(final, ".sf3")) o.add("\n⚠ SF3 (сжатый Vorbis) в этой сборке не поддерживается — нужен обычный SF2.");
        sendMsg(chat, o.b);
        return;
    }

    if (!strcmp(sub, "use")) {
        int ln = listLib(lib, 64);
        char names[fsr::kMaxSf][96];
        int n = 0;
        for (int i = 2; i < k.n && n < fsr::kMaxSf; i++) {
            int idx = resolveSf(k.t[i], lib, ln);
            if (idx < 0) { Out o; o.add("❌ «%s» не найден (или имя неоднозначно). /fs sf list", k.t[i]); sendMsg(chat, o.b); return; }
            snprintf(names[n++], 96, "%s", lib[idx].name);
        }
        if (!n) { sendMsg(chat, "Укажите номера или имена: /fs sf use 1 2"); return; }
        writeStack(names, n);
        cmdSfList(chat);
        return;
    }

    if (!strcmp(sub, "unload") || !strcmp(sub, "del") || !strcmp(sub, "rm")) {
        bool del = strcmp(sub, "unload") != 0;
        if (k.n < 3) { sendMsg(chat, "Укажите номер или имя: /fs sf unload 2"); return; }
        int ln = listLib(lib, 64);
        int idx = resolveSf(k.t[2], lib, ln);
        if (idx < 0) { sendMsg(chat, "❌ Такого SoundFont нет. /fs sf list"); return; }
        char names[fsr::kMaxSf][96], keep[fsr::kMaxSf][96];
        int sn = readStack(names, fsr::kMaxSf), kn = 0;
        for (int i = 0; i < sn; i++)
            if (strcmp(names[i], lib[idx].name)) snprintf(keep[kn++], 96, "%s", names[i]);
        if (kn == 0) snprintf(keep[kn++], 96, "%s", kDefaultSf);
        writeStack(keep, kn);
        if (del) {
            if (!strcmp(lib[idx].name, kDefaultSf)) { sendMsg(chat, "default.sf2 встроенный, удалять его не нужно (он убран из активных)."); return; }
            char full[600];
            pathIn(full, sizeof(full), "soundfonts", lib[idx].name);
            unlink(full);
        }
        cmdSfList(chat);
        return;
    }

    if (!strcmp(sub, "reset")) {
        char names[1][96];
        snprintf(names[0], 96, "%s", kDefaultSf);
        writeStack(names, 1);
        cmdSfList(chat);
        return;
    }

    if (!strcmp(sub, "inst") || !strcmp(sub, "presets")) {
        if (g_p.active) { sendMsg(chat, "⏳ Сейчас идёт другая задача. /fs status или /fs cancel"); return; }
        int ln = listLib(lib, 64);
        char names[fsr::kMaxSf][96];
        char pick[96];
        if (k.n > 2) {
            int idx = resolveSf(k.t[2], lib, ln);
            if (idx < 0) { sendMsg(chat, "❌ Такого SoundFont нет. /fs sf list"); return; }
            snprintf(pick, sizeof(pick), "%s", lib[idx].name);
        } else {
            readStack(names, fsr::kMaxSf);
            snprintf(pick, sizeof(pick), "%s", names[0]);
        }
        memset(&g_params, 0, sizeof(g_params));
        char full[600];
        pathIn(full, sizeof(full), "soundfonts", pick);
        snprintf(g_params.sf_paths[0], fsr::kPath, "%s", full);
        g_p.kind = kJobInst;
        g_p.chat = chat;
        g_p.progressId = -1;
        snprintf(g_p.sfName, sizeof(g_p.sfName), "%s", pick);
        startJob();
        return;
    }
    sendMsg(chat, "Подкоманды sf: list, add, use, unload, del, reset, inst. Подробнее: /fs help");
}

void showSettings(long long chat) {
    Cfg c;
    loadCfg(c);
    char ep[500], extra[2048];
    extraPath(ep, sizeof(ep));
    readFile(ep, extra, sizeof(extra));
    fsr::ProgOverride ov[fsr::kMaxOvr];
    int on = readOvr(ov, fsr::kMaxOvr);
    Out o;
    o.add("⚙ Настройки рендера\n");
    o.add("gain %g · rate %d · bits %d%s · %s\n", c.gain, c.rate, c.bits, c.bits == 32 ? " (float)" : "", c.mono ? "mono" : "stereo");
    o.add("reverb %s (%s) · chorus %s · limiter %s\n", c.reverb ? "on" : "off", c.engine, c.chorus ? "on" : "off", c.limiter ? "on" : "off");
    o.add("interp %d · poly %d · tempo ×%g · bank-select %s\n", c.interp, c.poly, c.speed, c.bank);
    o.add("хвост до %d с · максимум %d мин\n", c.tail, c.maxmin);
    if (extra[0]) o.add("\nДоп. настройки FluidSynth:\n%s", extra);
    if (on) {
        o.add("\nПринудительные инструменты:\n");
        for (int i = 0; i < on; i++) {
            if (ov[i].bank >= 0) o.add("канал %d → банк %d, программа %d\n", ov[i].chan + 1, ov[i].bank, ov[i].prog);
            else o.add("канал %d → программа %d\n", ov[i].chan + 1, ov[i].prog);
        }
    }
    o.add("\nИзменить: /fs set <ключ> <значение> (/fs help)");
    sendMsg(chat, o.b);
}

void cmdSet(long long chat, const Toks& k) {
    if (k.n < 2) { showSettings(chat); return; }
    char err[220];
    if (!strcmp(k.t[1], "reset")) {
        char p[500];
        extraPath(p, sizeof(p));
        unlink(p);
        sendMsg(chat, "✅ Доп. настройки FluidSynth сброшены (основные — на экране настроек плагина).");
        return;
    }
    if (!strcmp(k.t[1], "unset")) {
        if (k.n < 3) { sendMsg(chat, "Укажите ключ: /fs set unset synth.reverb.room-size"); return; }
        const char* key = k.t[2];
        for (const Alias& a : kAliases)
            if (!strcmp(a.a, key)) key = a.full;
        extraSet(key, nullptr);
        showSettings(chat);
        return;
    }
    if (k.n < 3) { sendMsg(chat, "Нужно значение: /fs set <ключ> <значение>"); return; }
    const char* key = k.t[1];
    const char* val = k.t[2];
    const SDef* d = findDef(key);
    if (d) {
        Cfg tmp;
        char norm[64];
        if (!cfgApply(tmp, d, val, norm, sizeof(norm), err, sizeof(err))) { sendMsg(chat, err[0] ? err : "значение не принято"); return; }
        if (!saveCfgKey(d, norm)) { sendMsg(chat, "❌ Не удалось сохранить настройку"); return; }
        Out o;
        o.add("✅ %s = %s", d->key, norm);
        sendMsg(chat, o.b);
        return;
    }
    for (const Alias& a : kAliases)
        if (!strcmp(a.a, key)) key = a.full;
    if (!fsr::validateSetting(key, val, err, sizeof(err))) {
        Out o;
        o.add("❌ %s: %s", key, err);
        sendMsg(chat, o.b);
        return;
    }
    if (!extraSet(key, val)) { sendMsg(chat, "❌ Слишком много дополнительных настроек. /fs set reset"); return; }
    Out o;
    o.add("✅ %s = %s", key, val);
    sendMsg(chat, o.b);
}

void cmdProg(long long chat, const Toks& k) {
    fsr::ProgOverride ov[fsr::kMaxOvr];
    int n = readOvr(ov, fsr::kMaxOvr);
    if (k.n >= 2 && !strcmp(k.t[1], "clear")) {
        if (k.n >= 3) {
            int ch = atoi(k.t[2]) - 1, m = 0;
            for (int i = 0; i < n; i++)
                if (ov[i].chan != ch) ov[m++] = ov[i];
            writeOvr(ov, m);
        } else {
            writeOvr(ov, 0);
        }
        sendMsg(chat, "✅ Готово. Теперь инструменты берутся из самого MIDI-файла (там, где сброшено).");
        return;
    }
    if (k.n < 3) {
        Out o;
        if (!n) o.add("Принудительных инструментов нет — берутся из MIDI.\n");
        for (int i = 0; i < n; i++) {
            if (ov[i].bank >= 0) o.add("канал %d → банк %d, программа %d\n", ov[i].chan + 1, ov[i].bank, ov[i].prog);
            else o.add("канал %d → программа %d\n", ov[i].chan + 1, ov[i].prog);
        }
        o.add("\nЗадать: /fs prog <канал 1-16> <программа 0-127> [банк]\nСписок: /fs sf inst");
        sendMsg(chat, o.b);
        return;
    }
    int ch = atoi(k.t[1]) - 1, pr = atoi(k.t[2]), bank = k.n > 3 ? atoi(k.t[3]) : -1;
    if (ch < 0 || ch > 15 || pr < 0 || pr > 127 || bank < -1 || bank > 16383) {
        sendMsg(chat, "❌ Канал 1–16, программа 0–127, банк 0–16383. Пример: /fs prog 1 25");
        return;
    }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ov[i].chan != ch) ov[m++] = ov[i];
    if (m < fsr::kMaxOvr) ov[m++] = {ch, bank, pr};
    writeOvr(ov, m);
    Out o;
    o.add("✅ Канал %d → программа %d", ch + 1, pr);
    if (bank >= 0) o.add(", банк %d", bank);
    sendMsg(chat, o.b);
}

void cmdRender(long long chat, long long replyTo, const Toks& k, int argFrom, const Input* preloaded) {
    if (g_p.active) { sendMsg(chat, "⏳ Уже идёт задача. Дождитесь результата или /fs cancel"); return; }
    Cfg cfg;
    loadCfg(cfg);
    RenderOpts o;
    char err[1400];
    if (!parseRenderArgs(k, argFrom, cfg, o, err, sizeof(err))) { Out m; m.add("❌ %s", err); sendMsg(chat, m.b); return; }

    Input in;
    if (preloaded) {
        in = *preloaded;
    } else if (!obtainInput(chat, replyTo, o.url, kWantMidi, in, err, sizeof(err))) {
        sendMsg(chat, err);
        return;
    }
    if (in.kind != kMidi) {
        Out m;
        m.add("❌ Это %s, а не MIDI (.mid).", kindName(in.kind));
        if (in.kind == kSf || in.kind == kDls) m.add(" Чтобы подключить SoundFont: /fs sf add");
        sendMsg(chat, m.b);
        unlink(in.path);
        return;
    }
    prune("out", 3, 0);
    prune("work", 1000, 3600);

    char label[96], base[96];
    if (o.outName[0]) snprintf(base, sizeof(base), "%s", o.outName);
    else snprintf(base, sizeof(base), "%s", in.name[0] ? in.name : "render");
    sanitize(base, label, sizeof(label));
    stripExt(label);
    if (strlen(label) > 50) label[50] = 0;
    char outFile[160], outPath[600];
    snprintf(outFile, sizeof(outFile), "%s_fluid.wav", label);
    pathIn(outPath, sizeof(outPath), "out", outFile);

    char sfLabel[200], sfFirst[96];
    if (!buildParams(cfg, o, in.path, outPath, err, sizeof(err), sfLabel, sizeof(sfLabel), sfFirst, sizeof(sfFirst))) {
        sendMsg(chat, err);
        unlink(in.path);
        return;
    }
    g_p.kind = kJobRender;
    g_p.chat = chat;
    g_p.cfg = cfg;
    snprintf(g_p.tmpMidi, sizeof(g_p.tmpMidi), "%s", in.path);
    snprintf(g_p.label, sizeof(g_p.label), "%s", in.name[0] ? in.name : label);
    snprintf(g_p.sfLabel, sizeof(g_p.sfLabel), "%s", sfLabel);
    snprintf(g_p.sfName, sizeof(g_p.sfName), "%s", sfFirst);
    char t[220];
    snprintf(t, sizeof(t), "⏳ Рендерю %s… 0%%", g_p.label);
    g_p.progressId = -1;
    sendMsg(chat, t, &g_p.progressId);
    startJob();
}

void cmdInfo(long long chat, long long replyTo) {
    Out o;
    Media m;
    char err[1400], dbg[420] = "";
    bool got = false;
    if (replyTo > 0) got = loadMessage(chat, replyTo, m, err, sizeof(err), dbg, sizeof(dbg));
    else {
        for (int i = g_seenN - 1; i >= 0 && !got; i--)
            if (g_seen[i].chat == chat) got = loadMessage(chat, g_seen[i].id, m, err, sizeof(err), dbg, sizeof(dbg));
        if (!got && !err[0]) snprintf(err, sizeof(err), "нет недавних сообщений с файлами");
    }
    if (!got) { o.add("ℹ Файл не найден: %s", err[0] ? err : "?"); sendMsg(chat, o.b); return; }
    o.add("ℹ Найдено в сообщении:\nпуть: %s\nссылка: %s\nимя: %s\n", m.path[0] ? m.path : "—", m.url[0] ? m.url : "—", m.name[0] ? m.name : "—");
    char path[600], e2[200];
    if (fetchMedia(m, path, sizeof(path), e2, sizeof(e2))) {
        char sz[24];
        fmtSize(fileSize(path), sz, sizeof(sz));
        o.add("получен: %s, %s\n", kindName(sniff(path)), sz);
        unlink(path);
    } else {
        o.add("получить не удалось: %s\n", e2);
    }
    if (dbg[0]) o.add("\nmedia (сырой JSON):\n%.420s", dbg);
    sendMsg(chat, o.b);
}

void cmdStatus(long long chat) {
    if (!g_p.active) { sendMsg(chat, "💤 Задач нет. Пример: /fs render (ответом на .mid)"); return; }
    Out o;
    o.add("⏳ %s: %s — %d%%", g_p.kind == kJobRender ? "рендер" : "чтение SoundFont", g_p.label[0] ? g_p.label : g_p.sfName,
          __atomic_load_n(&g_p.job.permille, __ATOMIC_ACQUIRE) / 10);
    sendMsg(chat, o.b);
}

void onCommand(const char* json) {
    long long chat = yg_json_get_int64(json, "chat_id", 0);
    if (!chat) return;
    long long replyTo = getReplyTo(json);
    char args[1200];
    getArgs(json, args, sizeof(args));
    Toks k;
    tokenize(args, k);
    const char* sub = k.n ? k.t[0] : "";

    if (!strcmp(sub, "help") || !strcmp(sub, "?") || !strcmp(sub, "помощь")) { sendMsg(chat, kHelp); return; }
    if (!strcmp(sub, "render") || !strcmp(sub, "r") || !strcmp(sub, "рендер")) { cmdRender(chat, replyTo, k, 1, nullptr); return; }
    if (!strcmp(sub, "sf") || !strcmp(sub, "font") || !strcmp(sub, "fonts")) {
        Toks kk = k;  // «fonts» без подкоманды = list
        if (kk.n == 1) snprintf(kk.t[kk.n++], sizeof(kk.t[0]), "list");
        cmdSf(chat, replyTo, kk);
        return;
    }
    if (!strcmp(sub, "set") || !strcmp(sub, "gain") || !strcmp(sub, "reverb") || !strcmp(sub, "chorus") || !strcmp(sub, "interp")) {
        if (strcmp(sub, "set") != 0) {  // /fs gain 0.8 == /fs set gain 0.8
            Toks kk;
            kk.n = 0;
            snprintf(kk.t[kk.n++], sizeof(kk.t[0]), "set");
            for (int i = 0; i < k.n && kk.n < 24; i++) snprintf(kk.t[kk.n++], sizeof(kk.t[0]), "%s", k.t[i]);
            cmdSet(chat, kk);
        } else {
            cmdSet(chat, k);
        }
        return;
    }
    if (!strcmp(sub, "prog") || !strcmp(sub, "select")) { cmdProg(chat, k); return; }
    if (!strcmp(sub, "cancel") || !strcmp(sub, "stop")) {
        if (g_p.active) { __atomic_store_n(&g_p.job.cancel, 1, __ATOMIC_RELEASE); sendMsg(chat, "⛔ Останавливаю…"); }
        else sendMsg(chat, "Нечего отменять.");
        return;
    }
    if (!strcmp(sub, "status")) { cmdStatus(chat); return; }
    if (!strcmp(sub, "info") || !strcmp(sub, "debug")) { cmdInfo(chat, replyTo); return; }

    // Без подкоманды: если есть файл — делаем очевидное (MIDI -> рендер, SoundFont -> добавить).
    if (!k.n && (replyTo > 0 || g_seenN > 0)) {
        char err[1400];
        Input in;
        if (obtainInput(chat, replyTo, nullptr, kWantAny, in, err, sizeof(err))) {
            if (in.kind == kMidi) { Toks none; none.n = 0; cmdRender(chat, replyTo, none, 0, &in); return; }
            if (in.kind == kSf || in.kind == kDls) {
                // перекладываем найденный файл в библиотеку тем же путём, что и /fs sf add
                unlink(in.path);
                Toks kk;
                snprintf(kk.t[0], sizeof(kk.t[0]), "sf");
                snprintf(kk.t[1], sizeof(kk.t[1]), "add");
                kk.n = 2;
                cmdSf(chat, replyTo, kk);
                return;
            }
            unlink(in.path);
        }
    }
    sendMsg(chat, kHelp);
}

// =============================================================== жизненный цикл
void installDefaultSf() {
    char p[500];
    pathIn(p, sizeof(p), "soundfonts", kDefaultSf);
    if (fileSize(p) == (long long) yg_default_sf2_size) return;
    FILE* f = fopen(p, "wb");
    if (!f) { logW("не могу записать default.sf2"); return; }
    fwrite(yg_default_sf2, 1, yg_default_sf2_size, f);
    fclose(f);
}

void onLoad(const YgHost* host) {
    g_host = host;
    snprintf(g_data, sizeof(g_data), "%s", host->data_dir ? host->data_dir : ".");
    size_t l = strlen(g_data);
    while (l > 1 && g_data[l - 1] == '/') g_data[--l] = 0;
    fsr::initLogging();
    char line[200];
    snprintf(line, sizeof(line), "loaded: host API %u.%u, data=%s", (unsigned) host->api_version, (unsigned) host->api_minor, g_data);
    logI(line);
}

void onEnable() {
    const char* subs[] = {"soundfonts", "work", "out"};
    for (const char* s : subs) {
        char p[500];
        pathIn(p, sizeof(p), s);
        mkdirp(p);
    }
    installDefaultSf();
    prune("work", 1000, 3600);
    prune("out", 3, 0);
    hok("register_command",
        "{\"name\":\"fs\",\"description\":\"FluidSynth: рендер MIDI в WAV\","
        "\"usage\":\"/fs render | /fs sf | /fs set | /fs prog | /fs help\",\"aliases\":[\"fluid\",\"fluidsynth\"]}");
    hok("register_action", "{\"id\":\"fs_render\",\"title\":\"🎹 Рендер MIDI (FluidSynth)\",\"where\":\"message_menu\"}");
    hok("register_action", "{\"id\":\"fs_addsf\",\"title\":\"🎼 Добавить SoundFont\",\"where\":\"message_menu\"}");
    hok("register_ui",
        "{\"id\":\"fs_status\",\"slot\":\"chat_top_bar\",\"kind\":\"icon_button\",\"icon\":\"extension\","
        "\"title\":\"FluidSynth\",\"order\":5}");
    logI("enabled");
}

void onDisable() {
    cancelAndJoin();
    logI("disabled");
}

void onEvent(const char* event, const char* json) {
    if (!event || !json) return;
    if (!strcmp(event, "command")) {
        onCommand(json);
    } else if (!strcmp(event, "timer")) {
        char id[32];
        if (yg_json_get_string(json, "id", id, sizeof(id)) >= 0 && !strcmp(id, "fs_poll")) pollJob();
    } else if (!strcmp(event, "action")) {
        char id[32];
        long long chat = yg_json_get_int64(json, "chat_id", 0);
        long long mid = yg_json_get_int64(json, "message_id", 0);
        if (chat && mid && yg_json_get_string(json, "id", id, sizeof(id)) >= 0) {
            Toks kk;
            kk.n = 0;
            if (!strcmp(id, "fs_render")) {
                cmdRender(chat, mid, kk, 0, nullptr);
            } else if (!strcmp(id, "fs_addsf")) {
                snprintf(kk.t[0], sizeof(kk.t[0]), "sf");
                snprintf(kk.t[1], sizeof(kk.t[1]), "add");
                kk.n = 2;
                cmdSf(chat, mid, kk);
            }
        }
    } else if (!strcmp(event, "message_received")) {
        rememberMedia(json);
    } else if (!strcmp(event, "ui_event")) {
        char id[64];
        if (yg_json_get_string(json, "id", id, sizeof(id)) >= 0 && !strcmp(id, "fs_status")) {
            char names[fsr::kMaxSf][96];
            int n = readStack(names, fsr::kMaxSf);
            char msg[400];
            if (g_p.active) snprintf(msg, sizeof(msg), "Идёт рендер: %d%%", __atomic_load_n(&g_p.job.permille, __ATOMIC_ACQUIRE) / 10);
            else snprintf(msg, sizeof(msg), "SoundFont: %s%s. /fs help — команды", names[0], n > 1 ? " +…" : "");
            toast("FluidSynth", msg);
        }
    }
}

const YgPlugin kPlugin = {YG_PLUGIN_API_VERSION, onLoad, onEnable, onDisable, onEvent, nullptr};

}  // namespace

extern "C" YG_EXPORT const YgPlugin* yougram_plugin_entry(void) { return &kPlugin; }
