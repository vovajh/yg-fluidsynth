# FluidSynth для Yougram

Плагин рендерит MIDI в WAV настоящим движком **FluidSynth** (libfluidsynth собирается внутрь `.so`)
и присылает готовый файл прямо в чат. Встроен небольшой SoundFont (Vintage Dreams Waves),
так что работает «из коробки»; свои `.sf2` / `.dls` добавляются командой.

## Сборка `.ygplugin`

Нужен Android NDK. Любой из способов:

1. **NDK на ПК/WSL/Linux:** `ANDROID_NDK_HOME=/путь/к/ndk ./build.sh` → `out/fluidsynth.ygplugin`
   (arm64-v8a + armeabi-v7a; `ABIS="arm64-v8a" ./build.sh` — только одна).
2. **GitHub Actions** (без установки чего-либо): залить проект в репозиторий → Actions → *Build ygplugin* →
   скачать артефакт `fluidsynth.ygplugin`. Workflow уже лежит в `.github/workflows/build.yml`.
3. **Termux:** `./build-termux.sh` (не проверено на устройстве).

Установка — как у остальных плагинов Yougram (файл `.ygplugin`). Нужны разрешения из манифеста:
`chat.read`, `chat.send`, `chat.modify`, `network`, `hooks`, `ui`.

## Команды (`/fs`, алиасы `/fluid`, `/fluidsynth`)

| Что | Команда | Аналог в FluidSynth |
|---|---|---|
| Рендер | `/fs render` ответом на .mid, или `/fs render <ссылка>`; также пункт «Рендер MIDI» в меню сообщения | `fluidsynth -F out.wav song.mid` |
| Частота / усиление / разрядность | `-r 48000` `-g 0.8` `-b 16\|24\|32` (32 = float) | `-r` `-g` `-O` |
| Темп, интерполяция, хвост | `-s 1.25` `-i 0\|1\|4\|7` `-t 6` | `player_tempo`, `interp` |
| Реверб / хорус / лимитер | `--no-reverb` `--no-chorus` `--no-limiter` `--engine dat\|fdn\|free\|lex\|smith`, `-m` моно | `-R` `-C`, `reverb`, `chorus` |
| Любая настройка движка | `-o synth.reverb.room-size=0.9` | `-o` |
| SoundFont | `/fs sf list · add · use 1 2 · unload N · del N · reset · inst [N]` | `fonts`, `load`, `unload`, `inst` |
| Инструмент на канале | `/fs prog 1 25 [банк]` · `/fs prog clear [канал]` (ударные: канал 10, банк 128) | `prog`, `select` |
| Постоянные настройки | `/fs set gain 0.8`, `/fs set reverb off`, `/fs set room 0.9`, `/fs set synth.… знач`, `/fs set` | `gain`, `reverb`, `set` |
| Служебное | `/fs cancel` · `/fs status` · `/fs info` (диагностика вложения) · `/fs help` | |

Основные настройки дублируются на экране настроек плагина. Результат — файл `<имя>_fluid.wav` в чате
с подписью (длительность, формат, SoundFont, время расчёта).

## Как устроено

* `src/renderer.cpp` — рендер: `fluid_player` + `fluid_synth`, тайминг по сэмплам (в десятки раз быстрее реального
  времени), свой WAV-писатель (16/24 бит PCM с TPDF-дизерингом, 32 бит float), хвост реверба, лимит длины.
* `src/plugin.cpp` — команды, настройки, получение файлов из чата, фоновый поток с опросом таймером
  (вызовы `host->call` — только из служебного потока Yougram).
* `third_party/fluidsynth` — исходники FluidSynth (LGPL-2.1, см. `licenses/`). Встроенный SoundFont — см. там же.

## Проверка

`./run_tests.sh` — нативный прогон на x86_64: настоящий FluidSynth, в заглушке только «хост» (чат, файлы, таймеры).
Те же тесты прогнаны на ARM64 и ARMv7 под qemu: размер файла и громкость результата совпадают с x86_64.

## Ограничения

* SF3 (сжатый Vorbis) не поддерживается — нужен обычный SF2/DLS.
* `http_download` в Yougram ограничен 32 МБ — SoundFont больше качать по ссылке нельзя, присылайте файлом.
* Формат вложения (`media`) в SDK не описан: плагин ищет в нём `local_path`/`path`/`url` и пр.
  Если файл не находится, `/fs info` ответом на сообщение покажет сырой JSON — по нему легко поправить `extractFrom()`.
