// Заглушка хоста Yougram: грузит libyg_fluidsynth.so через dlopen, эмулирует host->call
// (сообщения, файлы, таймеры, настройки) и прогоняет сценарий команд. Рендер — настоящий.
//   ./mock_host <plugin.so> <data_dir> <test.mid> <test_long.mid> <font.sf2>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <string>

#include "yougram_plugin.h"

static std::string g_data, g_midi, g_long, g_sf;
static std::map<std::string, std::string> g_settings = {
    {"gain", "\"1.0\""}, {"rate", "\"44100\""}, {"bits", "\"16\""}, {"mono", "false"}, {"reverb", "true"},
    {"chorus", "true"},  {"limiter", "true"},   {"interp", "\"4\""}, {"poly", "256"},  {"tail", "4"},
    {"maxmin", "15"},    {"speed", "\"1.0\""},  {"engine", "\"dat\""}, {"bank", "\"gs\""}};
static bool g_timer = false;
static int g_msgId = 1000;
static int g_sent = 0, g_failures = 0;

static std::string argStr(const char* a, const char* key) {
    static char buf[16384]; buf[0] = 0;
    yg_json_get_string(a, key, buf, sizeof(buf));
    return buf;
}

static bool copy(const std::string& from, const std::string& to) {
    FILE* i = fopen(from.c_str(), "rb");
    if (!i) return false;
    FILE* o = fopen(to.c_str(), "wb");
    if (!o) { fclose(i); return false; }
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), i)) > 0) fwrite(b, 1, n, o);
    fclose(i);
    fclose(o);
    return true;
}

static char* dup(const std::string& s) { return strdup(s.c_str()); }

static char* hostCall(void*, const char* method, const char* args) {
    std::string m = method;
    if (m == "settings_all") {
        std::string r = "{\"ok\":true,\"result\":{";
        bool first = true;
        for (auto& kv : g_settings) {
            if (!first) r += ",";
            first = false;
            r += "\"" + kv.first + "\":" + kv.second;
        }
        return dup(r + "}}");
    }
    if (m == "settings_set") {
        std::string key = argStr(args, "key");
        const char* v = yg_json_find(args, "value");
        const char* e = v;
        if (*v == '"') { e++; while (*e && *e != '"') e++; e++; }
        else while (*e && *e != ',' && *e != '}') e++;
        g_settings[key] = std::string(v, e - v);
        printf("  [host] settings_set %s=%s\n", key.c_str(), g_settings[key].c_str());
        return dup("{\"ok\":true}");
    }
    if (m == "timer_start") { g_timer = true; return dup("{\"ok\":true}"); }
    if (m == "timer_stop") { g_timer = false; return dup("{\"ok\":true}"); }
    if (m == "send_message") {
        std::string t = argStr(args, "text");
        printf("  [BOT] %s\n", t.c_str());
        return dup("{\"ok\":true,\"result\":{\"message_id\":" + std::to_string(++g_msgId) + "}}");
    }
    if (m == "edit_message") {
        printf("  [BOT edit] %s\n", argStr(args, "text").c_str());
        return dup("{\"ok\":true}");
    }
    if (m == "send_document") {
        std::string p = argStr(args, "path");
        struct stat st;
        if (stat(p.c_str(), &st) != 0) { printf("  [host] send_document: FILE MISSING %s\n", p.c_str()); return dup("{\"ok\":false,\"error\":\"no such file\"}"); }
        mkdir((g_data + "/../sent").c_str(), 0755);
        std::string dst = g_data + "/../sent/" + std::to_string(++g_sent) + "_" + p.substr(p.rfind('/') + 1);
        copy(p, dst);
        printf("  [BOT FILE] %s (%lld bytes)\n  caption: %s\n", dst.c_str(), (long long) st.st_size, argStr(args, "caption").c_str());
        return dup("{\"ok\":true,\"result\":{\"message_id\":" + std::to_string(++g_msgId) + "}}");
    }
    if (m == "get_message") {
        long long id = yg_json_get_int64(args, "message_id", 0);
        std::string media;
        switch (id) {
            case 1: media = "{\"type\":\"document\",\"file_name\":\"song.mid\",\"local_path\":\"" + g_midi + "\"}"; break;
            case 2: media = "{\"type\":\"document\",\"file_name\":\"Cool Font (v2).sf2\",\"local_path\":\"" + g_sf + "\"}"; break;
            case 3: media = "{\"type\":\"document\",\"file_name\":\"remote.mid\",\"url\":\"http://test/remote.mid\"}"; break;
            case 4: media = "{\"type\":\"document\",\"file_id\":\"ABC123\",\"size\":1234}"; break;
            case 5: media = "{\"type\":\"document\",\"file_name\":\"long song.mid\",\"local_path\":\"" + g_long + "\"}"; break;
            case 6: {  // нет ключа "media": вложение под чужим ключом, пути с экранированными слэшами
                std::string esc;
                for (char c : g_midi) { if (c == '/') esc += "\\/"; else esc += c; }
                return dup("{\"ok\":true,\"result\":{\"id\":6,\"chat_id\":100,\"type\":\"document\",\"attachment\":{\"name\":\"odd.mid\",\"uri\":\"" + esc + "\"}}}");
            }
            case 7:  // совсем без вложения
                return dup("{\"ok\":true,\"result\":{\"id\":7,\"chat_id\":100,\"type\":\"text\",\"text\":\"hi\"}}");
            default: return dup("{\"ok\":false,\"error\":\"not found\"}");
        }
        return dup("{\"ok\":true,\"result\":{\"id\":" + std::to_string(id) + ",\"chat_id\":100,\"text\":\"\",\"media\":" + media + "}}");
    }
    if (m == "http_download") {
        std::string url = argStr(args, "url"), name = argStr(args, "name");
        std::string dir = g_data + "/downloads";
        mkdir(dir.c_str(), 0755);
        std::string dst = dir + "/" + name;
        std::string src = url == "http://test/remote.mid" ? g_midi : (url == "http://test/font.sf2" ? g_sf : "");
        if (src.empty() || !copy(src, dst)) return dup("{\"ok\":false,\"error\":\"http 404\"}");
        return dup("{\"ok\":true,\"result\":{\"path\":\"" + dst + "\",\"size\":1}}");
    }
    if (m == "register_command" || m == "register_ui" || m == "toast") {
        printf("  [host] %s %s\n", method, args);
        return dup("{\"ok\":true}");
    }
    return dup("{\"ok\":true}");
}
static void hostFree(char* s) { free(s); }
static void hostLog(void*, int lvl, const char* msg) { if (lvl >= 2) printf("  [log%d] %s\n", lvl, msg); }

static const YgPlugin* g_pl;

static void event(const std::string& name, const std::string& json) { g_pl->on_event(name.c_str(), json.c_str()); }

static void cmd(const char* args, long long replyTo = 0) {
    char j[1024];
    if (replyTo) snprintf(j, sizeof(j), "{\"chat_id\":100,\"name\":\"fs\",\"args\":\"%s\",\"reply_to\":%lld}", args, replyTo);
    else snprintf(j, sizeof(j), "{\"chat_id\":100,\"name\":\"fs\",\"args\":\"%s\",\"reply_to\":null}", args);
    printf("\n>>> /fs %s%s\n", args, replyTo ? "  (reply_to)" : "");
    event("command", j);
}

// Эмуляция таймера хоста: пока плагин держит timer активным — шлём событие каждые 200 мс.
static void pump(double maxSec, double cancelAfter = -1, const char* cancelArgs = nullptr) {
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool cancelled = false;
    while (g_timer) {
        usleep(200000);
        timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (cancelAfter >= 0 && !cancelled && el >= cancelAfter) { cancelled = true; cmd(cancelArgs); }
        event("timer", "{\"id\":\"fs_poll\"}");
        if (el > maxSec) { printf("  !!! TIMEOUT waiting for job\n"); g_failures++; break; }
    }
}

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s plugin.so data_dir test.mid test_long.mid font.sf2\n", argv[0]); return 2; }
    g_data = argv[2]; g_midi = argv[3]; g_long = argv[4]; g_sf = argv[5];
    system(("rm -rf '" + g_data + "' '" + g_data + "/../sent'; mkdir -p '" + g_data + "'").c_str());
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto entry = (YgPluginEntryFn) dlsym(h, "yougram_plugin_entry");
    if (!entry) { fprintf(stderr, "no entry\n"); return 1; }
    g_pl = entry();
    static YgHost host = {};
    host.api_version = 2; host.api_minor = 1; host.log = hostLog; host.call = hostCall; host.free_string = hostFree;
    host.plugin_id = "fluidsynth"; host.data_dir = g_data.c_str();
    g_pl->on_load(&host);
    g_pl->on_enable();

    cmd("help");
    cmd("info", 1);
    cmd("info", 4);
    cmd("render", 6);                 pump(60);   // вложение под нестандартным ключом
    cmd("render", 7);                              // нет вложения: должен прийти сырой JSON
    cmd("render", 1);                 pump(60);
    cmd("render -b 24 -r 48000 -g 0.8 --no-chorus -m", 1); pump(60);
    cmd("render -b 32", 1);           pump(60);
    cmd("render http://test/remote.mid -n mytrack", 0); pump(60);
    cmd("render", 3);                 pump(60);
    cmd("render", 4);
    cmd("render", 2);
    cmd("render -r 12345", 1);
    cmd("render --bogus", 1);
    cmd("sf list");
    cmd("sf add", 2);
    cmd("sf list");
    cmd("sf inst");                   pump(60);
    cmd("sf inst 1");                 pump(60);
    cmd("sf use default.sf2 cool");
    cmd("render -f 2", 1);            pump(60);
    cmd("prog 1 25");
    cmd("prog 10 0 128");
    cmd("prog 17 1");
    cmd("prog");
    cmd("render", 1);                 pump(60);
    cmd("prog clear");
    cmd("set");
    cmd("set reverb off");
    cmd("set gain 0.7");
    cmd("set gain 99");
    cmd("set room 0.9");
    cmd("set synth.reverb.room-size 5");
    cmd("set synth.nonexistent 1");
    cmd("set synth.gain 1");
    cmd("set");
    cmd("reverb on");
    cmd("render", 1);                 pump(60);
    cmd("set reset");
    cmd("sf del 2");
    cmd("sf reset");
    cmd("sf list");
    // отмена долгого рендера
    cmd("render", 5);
    pump(60, 0.6, "cancel");
    cmd("status");
    cmd("cancel");
    // файл без reply: плагин видит вложение через message_received
    event("message_received", "{\"message\":{\"id\":1,\"chat_id\":100,\"outgoing\":false,\"text\":\"\",\"media\":{\"type\":\"document\"}}}");
    cmd("render");                    pump(60);
    cmd("", 1);                       pump(60);  // голая /fs ответом на MIDI
    cmd("");
    pump(60);
    printf("\n>>> action fs_render (message_id=1)\n");
    event("action", "{\"id\":\"fs_render\",\"chat_id\":100,\"message_id\":1}");
    pump(60);
    printf("\n>>> action fs_addsf (message_id=2)\n");
    event("action", "{\"id\":\"fs_addsf\",\"chat_id\":100,\"message_id\":2}");
    g_pl->on_disable();
    printf("\nГотово. Отправлено файлов: %d, провалов: %d\n", g_sent, g_failures);
    return g_failures ? 1 : 0;
}
