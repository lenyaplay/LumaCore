# Разработка LumaCore — практический гайд

Этот документ — *как* собрать, запустить и не наступить на уже найденные грабли.
*Почему* устроено так, а не иначе — в [ARCHITECTURE.md](ARCHITECTURE.md); детальные
пошаговые планы по конкретным этапам — в [ai_plans/](ai_plans/).

## Требования

| Инструмент | Версия | Зачем |
|---|---|---|
| [FVM](https://fvm.app) + Flutter | 3.38.10 (см. `.fvmrc`) | Все команды ниже — `fvm flutter ...` |
| Android SDK + NDK | NDK **27.0.12077973** (закреплён в `android/app/build.gradle.kts`) | Сборка `native/` под Android — см. «Подводные камни» ниже, почему именно эта версия |
| CMake | 3.22.1+ (через Android SDK cmake-компонент или системный) | `externalNativeBuild` + `native/CMakeLists.txt` |
| Xcode | текущий стабильный | Сборка/запуск iOS, `xcodebuild -create-xcframework` |
| Python 3 | любой актуальный | `server/` — mock-бэкенд лицензий |

## Сборка и запуск

### Android

```bash
cd android
# NDK версия должна совпадать с той, что закреплена в app/build.gradle.kts —
# если ANDROID_NDK_HOME не задан или указывает на другую версию, Gradle всё
# равно возьмёт закреплённую, но лучше не расходиться.
ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/27.0.12077973 \
  fvm flutter build apk --debug --target-platform android-arm64
```

Native-слой (`liblumacore.so`) собирается автоматически через
`externalNativeBuild` → `native/CMakeLists.txt` (см. `android/app/build.gradle.kts`) —
отдельного шага не нужно, если FFmpeg для Android уже собран (см. ниже).

Для живого запуска на подключённом устройстве/эмуляторе: `fvm flutter run`.

**FFmpeg для Android нужно собрать один раз заранее**:
```bash
ANDROID_NDK_HOME=$HOME/Library/Android/sdk/ndk/27.0.12077973 tools/ffmpeg/build_android.sh
```
Собирает `arm64-v8a`-срез в `native/third_party/ffmpeg_build/android/arm64-v8a/`
(gitignored — пересобирается локально, не коммитится).

### iOS

```bash
tools/ffmpeg/build_ios.sh          # один раз — FFmpeg device+sim arm64 срезы
tools/ios/build_xcframework.sh     # собирает native/ + линкует LumaCoreKit.xcframework
fvm flutter run                    # на подключённом устройстве или симуляторе
```

### Mock-сервер лицензий

```bash
cd server && ./run.sh
```

Слушает `0.0.0.0:8000`. Печатает LAN IP Мака и готовую команду для физического
устройства. Симулятор iOS и Android-эмулятор достают `127.0.0.1:8000` из коробки.

Физическое устройство:
- **Android**: `adb reverse tcp:8000 tcp:8000` — трафик с телефона идёт через USB
  обратно на Mac, IP менять не нужно.
- **iOS**: телефон и Mac должны быть в одной Wi-Fi сети, приложение нужно запустить с
  `--dart-define=LICENSE_SERVER_URL=http://<LAN_IP_МАКА>:8000` (см. готовую конфигурацию
  `.vscode/launch.json` → «LumaCore (physical device, LAN license server)» — IP там
  зашит по DHCP, может понадобиться обновить, если Mac получил другой адрес).

## Структура репозитория

Практическая версия — детальная таблица с обоснованиями в
[ARCHITECTURE.md §5/§7](ARCHITECTURE.md#5-структура-native-cmake). Коротко, где что искать:

```
native/src/render/gl/       GLRenderBackend (Android) + шейдеры (ShaderSources.h)
native/src/render/metal/    MetalRenderBackend (iOS) + шейдеры (.metal)
native/src/encode/          EncoderSession — FFmpeg avcodec/avformat, mediacodec/videotoolbox glue
native/src/license/         TokenValidator — офлайн Ed25519-верификация
native/jni/                 JNI-мост Android (тонкий, без логики)
native/objcxx/               Obj-C++-мост iOS (тонкий, без логики)
android/app/.../kotlin/     CameraCaptureController, EffectsRenderController, RecordingController
ios/Runner/                 те же роли на Swift
lib/core/ffi/               dart:ffi-биндинги (setEffectParams/getStats — единственные,
                            остальное идёт через Platform Channel, lib/core/channels/)
server/                     mock license backend (FastAPI + PyNaCl Ed25519)
docs/ai_plans/              пошаговые планы по каждому крупному этапу, с реальными находками
```

## Как устроен эффект комикса

Эффект на демо-видео (снято на iPhone, см. корневой [README](../README.md)) — не
отдельный шейдер, а одна ветка внутри общего прохода цветокоррекции: на iOS —
`native/src/render/metal/shaders/ColorCorrection.metal` (Metal Shading Language), на
Android — 1:1 порт в `native/src/render/gl/shaders/ShaderSources.h`
(`kColorCorrectFragmentSource`, GLSL ES). Математика идентична на обеих платформах,
включена по биту `effectMask & 0x10`. Шесть шагов на пиксель:

1. **Sobel-градиент по яркости.** Берутся 8 соседних тапов (3×3 без центра: `tl/tc/tr/
   ml/mr/bl/bc/br`). Яркость каждого — `dot(rgb, vec3(0.299, 0.587, 0.114))` (на Android
   нет отдельной Y-плоскости, как на iOS — `samplerExternalOES` уже отдаёт RGB, luma
   считается из RGB на каждый тап). Классические ядра Собеля:
   ```
   gx = -tl - 2·ml - bl + tr + 2·mr + br
   gy = -tl - 2·tc - tr + bl + 2·bc + br
   ```
2. **Порог с мягким краем.** `edgeMag = clamp(length(gx, gy) / 0.8, 0, 1)` — делитель
   `0.8` эмпирический: реальные кадры камеры (оптическое/сенсорное/энкодерное
   размытие) дают градиенты заметно слабее синтетической резкой границы, без него порог
   почти никогда не срабатывал бы на реальном видео. Затем
   `edge = smoothstep(edgeThreshold, edgeThreshold + 0.06, edgeMag)` — плавный, не
   бинарный переход, `edgeThreshold` — пользовательский слайдер.
3. **Постеризация цвета.** `posterized = floor(rgb * 4 + 0.5) / 4` — квантование в 4
   уровня на канал, даёт плоские заливки вместо гладких градиентов (собственно
   «комиксовый» вид).
4. **«Бумажная» база.** `paper = mix(posterized, white, 0.15)` — небольшая подмешка
   белого; без неё результат читается как «всё стало одним серым пятном», а не как
   стилизованная подложка под рисунок.
5. **Чёрные контуры.** `comic = mix(paper, black, edge)` — где `edge` близко к 1
   (настоящая граница объекта), цвет уводится к чёрному — это и есть обводка тушью.
6. **Финальный микс по интенсивности.** `rgb = mix(rgb, comic, edgeIntensity)` —
   пользовательский слайдер плавно смешивает оригинал с «комиксовым» результатом, а не
   переключает эффект жёстко on/off.

Порядок в пайплайне: камера (`samplerExternalOES`, уже RGB) → цветокоррекция + сепия +
комикс (**один** проход) → виньетка → частицы → GPU-конвертация в NV12 для энкодера.
Подробности прохода — `docs/ai_plans/04-android-camerax-gl-pipeline.md` §B.1.

## Подводные камни, найденные при реальной отладке на устройстве

Ничего из этого не было очевидно из планов до того, как код запустили на настоящем
телефоне (Android-эмулятор и текстовый code review всё это пропустили). Стоит прочитать
перед тем, как чинить что-то похожее с нуля.

- **Два разных `abiFilters` в AGP.** `android.defaultConfig.ndk.abiFilters` управляет
  только тем, что попадёт в APK, а `externalNativeBuild.cmake.abiFilters` — тем, под
  какие ABI реально конфигурируется CMake. Ограничение только первого не помешало Gradle
  дополнительно сконфигурировать `armeabi-v7a` — CMake упал на собственном arm64-only
  guard'е в `native/CMakeLists.txt`. Нужно `.clear()` + заполнить **оба** поля.
- **`flutter.ndkVersion` по умолчанию не совпадает с `android.ndkVersion`.** Flutter
  Gradle Plugin триггерит скачивание своей рекомендуемой версии NDK через отдельный
  internal-проект `:jni`, независимо от того, что явно прописано в `android.ndkVersion`.
  Итог — лишние ГБ загрузок первой сборки, если не закрепить версию жёстко (см. таблицу
  требований выше) и не считать, что дефолт «просто совпадёт».
- **Выходная `SurfaceTexture` Flutter тоже нуждается в `setDefaultBufferSize()`.** Не
  только камера-входная (`camSurfaceTexture`) — без вызова на `entry.surfaceTexture()`
  буфер по умолчанию ~1×1, а превью выглядит как один сплошной цвет, слабо меняющийся
  во времени (реальный кадр рендерится, просто в микроскопический буфер).
- **`SurfaceTexture.getTransformMatrix()` обязателен.** Без применения этой матрицы к UV
  при сэмплинге камеры в шейдере, кадр приходит в сыром сенсорном layout — превью
  получается повёрнутым/отражённым относительно того, что должно быть на экране.
- **FFmpeg `h264_mediacodec` требует конкретные bitstream filters.** При
  `AV_CODEC_FLAG_GLOBAL_HEADER` (всегда true для нашего MP4-мукса)
  `libavcodec/mediacodecenc.c` внутри себя тянет `h264_metadata` (сигнал реального crop,
  если MediaCodec округлил размер до кратного 16 внутри) и `extract_extradata` (SPS/PPS
  для контейнера). Общий `--disable-bsfs` в конфигурации FFmpeg (см.
  `tools/ffmpeg/common.sh`) их выключает — `avcodec_open2` падает с «Bitstream filter not
  found» на нестандартных/не 16-aligned разрешениях. Нужен точечный
  `--enable-bsf=h264_metadata,extract_extradata` для Android-сборки.
- **Гонка потоков в `EncoderSession`.** `audioAccum`/`audioAccumSamples` читались и
  писались с audio-capture-потока (`submitAudioFrame`) и encode/recording-потока
  (`stop()`) без мьютекса — рваное чтение приводило к underflow беззнакового вычисления
  и `memset()` далеко за пределы буфера (SIGSEGV на реальном устройстве, эмулятор не
  проявлял). Тот же риск существует и в общем с iOS файле `EncoderSession.cpp` — просто
  там не проявился при более редкой конкуренции потоков.
- **`glReadPixels` отдаёт строки снизу-вверх.** GL-конвенция framebuffer'а (row 0 —
  нижняя строка) не совпадает с ожиданием NV12/H.264 (row 0 — верхняя). Живое превью
  этой проблемы не касается (показывается через `eglSwapBuffers`, без CPU-readback) —
  страдает только записанное видео, которое без явного переворота при упаковке
  получается перевёрнутым по вертикали.
- **Синхронный энкод на GL/render-потоке душит и превью, и запись.**
  `EncoderSession::submitFrame()` изначально гонял весь FFmpeg/MediaCodec round-trip
  (`avcodec_send_frame` → аппаратный энкод → `avcodec_receive_packet` → mux-запись)
  синхронно на том же потоке, что рисует превью — во время записи весь пайплайн
  проседал до скорости энкодера (70мс/кадр → 160мс/кадр). Исправлено переносом энкода на
  отдельный `videoEncodeThread` (только Android — на iOS такого симптома не было).

Более подробный разбор каждого пункта — `docs/ai_plans/04-android-camerax-gl-pipeline.md`
и `docs/ai_plans/05-android-recording-performance-fix.md`.
