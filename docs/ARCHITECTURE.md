# LumaCore — техническая архитектура

Этот документ дополняет [Task.md](Task.md), который остаётся продуктовым ТЗ и roadmap.
Здесь — инженерная спецификация: конкретные решения там, где Task.md сформулирован
декларативно («общий интерфейс рендер-пайплайна», «кросс-компиляция FFmpeg», ECDSA),
плюс явные пометки рисков и мест, требующих прототипирования, а не выдаваемых за решённые.

Целевые платформы: iOS + Android + Windows.

## Ключевые отклонения от Task.md (сознательные, зафиксированы явно)

- **FFmpeg**: собственная минимальная сборка из исходников под NDK/xcframework/Windows —
  не `ffmpeg-kit-flutter` (архивирован в 2023), не community-форк, не гибрид с
  MediaCodec/VideoToolbox как отдельным путём, не готовые Windows-бинарники (gyan.dev/BtbN).
  См. §4.
- **Аудио**: запись звука входит в MVP, не только видео — иначе демо неполноценно для
  мультимедийной роли, а A/V-синхронизация/буферизация прямо фигурирует в требованиях
  вакансии. См. §1.
- **Подпись лицензии**: Ed25519 (libsodium) вместо ECDSA — libsodium нативно даёт EdDSA,
  не ECDSA; проще, без ASN.1-кодирования подписи, эквивалентная безопасность. См. §6.
- **FFmpeg-декод**: сознательно не реализуется — миниатюры/просмотр через нативные OS API.
  См. §4.
- **Windows добавлен третьей платформой с GPU-бэкендом на Vulkan** (не desktop OpenGL,
  не Direct3D) — сознательно закрывает буквальный список вакансии «OpenGL ES / Vulkan /
  Metal» по одному нативному API на платформу (Android=GL ES, iOS=Metal, Windows=Vulkan).
  Заметно сложнее, чем переиспользование desktop OpenGL — см. §2 и §8.

---

## 1. Threading-модель: сенсор → MP4 на диске

### Android
- `CameraCallbackThread` (отдельный `HandlerThread`) — `onFrameAvailable()` от
  `SurfaceTexture` (`GL_TEXTURE_EXTERNAL_OES`), никогда не на главном потоке.
- `GLRenderThread` — один постоянный поток с собственным `EGLContext`. На каждый тик:
  `updateTexImage()` (сам механизм backpressure — SurfaceTexture хранит один слот,
  лишние кадры дропаются платформой без очереди) → 3-проходный шейдерный пайплайн в
  offscreen FBO (цветокоррекция → виньетка → частицы) → блит в EGL-поверхность Flutter
  (preview) + передача кадра на encode, если идёт запись.
- `EncodeThread` — отдельный от render, т.к. асинхронные колбэки MediaCodec и вызовы
  `avcodec_send_frame`/`receive_packet` могут блокироваться при термотроттлинге; это не
  должно откатываться на камеру/рендер.
- **Backpressure между render и encode**: кольцевой буфер на 3–4 слота, политика
  drop-oldest — render никогда не блокируется, отзывчивость preview важнее полноты
  записи при перегреве. Счётчик dropped frames — в структуру статистики (см. §3), это
  и есть метрика «до/после» для README.

### iOS
- `AVCaptureVideoDataOutput` на отдельной serial `DispatchQueue`
  (`alwaysDiscardsLateVideoFrames = true` — аналог Android-механизма «один слот»).
- `MetalRenderThread` — `CVPixelBuffer` → `CVMetalTexture` через `CVMetalTextureCache`
  (zero-copy, IOSurface-backed), тот же логический 3-проходный пайплайн, тикает по
  приходу кадра камеры (не по `CADisplayLink` — частота камеры и экрана могут не
  совпадать, запись не должна зависеть от рефреша дисплея).
- Encode: отрендеренный `CVPixelBuffer` идёт в FFmpeg VideoToolbox-обёртку напрямую
  (`AV_PIX_FMT_VIDEOTOOLBOX`), путь более зрелый и действительно zero-copy.

### Аудио (в MVP)
Отдельный `AudioCaptureThread` (AudioRecord/AVAudioEngine) → программный AAC-энкодер
FFmpeg. Без drop-oldest — глитч звука заметнее пропущенного кадра, буфер побольше.
Видео и аудио используют один monotonic-источник времени для PTS; при дропе
видеокадров muxer работает в variable frame rate (пропускает PTS-слоты), не дублирует
кадры ради постоянного FPS.

### Windows
- `MFCaptureThread` — `IMFSourceReader` (не `IMFCaptureEngine`: последний тащит
  собственный managed pipeline с preview/record sinks, что конфликтует с уже
  существующим кастомным `RenderPipeline`+FFmpeg-энкодом; `IMFSourceReader` — низкоуровневый
  «отдай сырые сэмплы», прямой аналог `SurfaceTexture`-колбэка и `AVCaptureVideoDataOutput`).
  DirectShow сознательно не используется — по актуальной позиции Microsoft вытеснен
  Media Foundation, остаётся только для легаси. Работает в async-режиме
  (`IMFSourceReaderCallback::OnReadSample`) на выделенной MF work queue
  (`MFAllocateWorkQueue(MF_STANDARD_WORKQUEUE, ...)`, а не на дефолтной/общей очереди) —
  аналог `CameraCallbackThread`/capture-`DispatchQueue`, но именно work queue нужно
  явно запрашивать, это не «бесплатно» в отличие от мобильных платформ.
- `VulkanRenderThread` — владеет `VkDevice`/`VkQueue`, гоняет тот же 3-проходный пайплайн
  через `VulkanRenderBackend`, публикует в shared D3D11/Vulkan текстуру для Flutter (см. §3).
- `EncodeThread` — та же роль, что на Android/iOS (колбэки MF-энкодера и
  `avcodec_send_frame`/`receive_packet` не должны блокировать capture/render).
- `AudioCaptureThread` — WASAPI (`IAudioClient`/`IAudioCaptureClient`), та же non-drop
  политика и общий monotonic-источник PTS.
- **Отличие, не по аналогии**: `IMFSourceReader` не даёт бесплатного «один слот, последний
  кадр побеждает» механизма, в отличие от `SurfaceTexture`/`alwaysDiscardsLateVideoFrames`.
  Следующий `ReadSample` запрашивается только после того, как render-поток освободил
  текущий сэмпл (естественный single-in-flight throttle) — используется тот же
  bounded ring buffer/drop-oldest из `native/src/util/`, а не отдельный механизм.

### Термо-адаптивная деградация
`PowerManager` thermal status (Android 10+) / `ProcessInfo.thermalState` (iOS) →
при THROTTLING/CRITICAL сначала отключается самый дорогой проход (частицы), затем
снижается разрешение рендера — до того, как кадр дойдёт до энкодера. Демонстрируемая
фича, не только отчёт профилирования.

**Windows-оговорка**: кросс-вендорного thermal API уровня `PowerManager`/`ProcessInfo`
на Windows нет (ACPI thermal zone через WMI — не консистентно по вендорам, часто
недоступно на ноутбуках). Вместо термального сигнала используется прокси —
`avgFrameMs` из `LumaStats`; превышение бюджета N кадров подряд запускает ту же
лестницу деградации (частицы → разрешение). Это осознанная замена, не паритет с
мобильным термальным сигналом — фиксируется в README как таковая, не выдаётся за то же самое.

---

## 2. GPU render pipeline: где реально общий код

GLSL и MSL исходники шейдеров **не унифицируются** (SPIR-V/SPIRV-Cross — сознательно
отложено, см. Task.md §9 «что дальше»). Общее в C++:

- **`RenderPipeline`** — оркестрация проходов, FBO ping-pong, тайминг кадра.
- **Математика частиц** (позиции/скорости/seed/время) — чистый C++, считается один
  раз, загружается как instance-данные в любой бэкенд.
- **`EffectParamsBlock`** — один байтовый layout с явным `alignas`/паддингом,
  совместимым одновременно с GLSL `std140` и Metal constant buffer layout — один и тот
  же блок памятикопируется и в GL UBO (`glBufferData`), и в Metal buffer
  (`newBufferWithBytes:`) без platform-specific маршалинга.

Платформо-специфично (реальный шов):
- создание контекста/поверхности (EGL vs `MTLDevice`/`CAMetalLayer`/`CVMetalTextureCache`);
- импорт внешнего кадра (`glEGLImageTargetTexture2DOES` vs
  `CVMetalTextureCacheCreateTextureFromImage`);
- компиляция шейдеров (runtime GLSL из строк vs precompiled `.metallib`);
- презентация (`eglSwapBuffers` в SurfaceTexture vs запись в `CVPixelBuffer`-пул для
  pull-модели Flutter, см. §3).

```cpp
class IRenderBackend {
public:
  virtual bool initialize(const RenderContextParams&) = 0;
  virtual TextureHandle importExternalFrame(NativeImageHandle) = 0;
  virtual void beginFrame() = 0;
  virtual void runPass(PassId pass, TextureHandle src, TextureHandle dstFbo,
                        const EffectParamsBlock& params) = 0;
  virtual TextureHandle endFrame() = 0;
  virtual PlatformImageHandle exportForPreview(TextureHandle) = 0;
  virtual PlatformImageHandle exportForEncoder(TextureHandle) = 0;
  virtual void destroy() = 0;
};
```

`RenderPipeline` (общий) владеет `IRenderBackend*` + списком проходов + состоянием
частиц. `GLRenderBackend` и `MetalRenderBackend` (последний — честно `.mm`, не чистый
C++, поскольку у Metal нет C API) реализуют интерфейс.

**Риск, не проверено**: разница дефолтной точности GLSL ES `mediump` vs Metal float,
разные конвенции Y-flip/NDC между OpenGL и Metal — классические источники визуального
расхождения между независимо написанными шейдерами. Общий C++ param-struct не
гарантирует пиксельную идентичность — нужен явный шаг side-by-side сравнения
скриншотов между платформами.

### `VulkanRenderBackend` (Windows)
Offscreen без swapchain — вывод идёт не в окно, а в shared-текстуру для Flutter (см. §3).
`VkInstance` → физическое устройство (предпочтение дискретному, иначе любое доступное) →
`VkDevice` с одной graphics/compute очередью. Управление памятью — через VMA
(AMD VulkanMemoryAllocator, новая third-party зависимость наравне с FFmpeg/libsodium),
а не ручной `vkAllocateMemory`. Между тремя проходами нужны явные
`VkImageMemoryBarrier` — у Vulkan нет неявной синхронизации GL, это чистый доп. boilerplate
без аналога в GL-бэкенде.

**Импорт кадра камеры (`importExternalFrame` для Windows)**: Media Foundation не
гарантированно D3D11-resident на практике, несмотря на архитектурную ориентацию на D3D.
Два реальных пути, выбор которых зависит от драйвера, а не от кода LumaCore:
1. GPU-resident (лучший случай) — `IMFDXGIDeviceManager` привязан к source reader,
   сэмплы приходят как `IMFDXGIBuffer`/`ID3D11Texture2D`, импортируется в Vulkan тем же
   механизмом shared NT handle, что и экспорт (см. §3).
2. CPU-buffer (частый случай, считать базовым) — большинство встроенных/бюджетных USB-камер
   отдают MJPEG/YUY2, MF софтверно декодирует в системную память (NV12), импорт —
   `vkMapMemory` staging-буфера + `vkCmdCopyBufferToImage`, без D3D11-interop вообще.

**Не проверено — прототипировать рано**: реально ли путь (1) достижим на типичном
железе ревьюера (VM, бюджетные USB-камеры, встроенные ноутбучные камеры) — неизвестно
до теста. Путь (2) — базовое допущение, путь (1) — опциональный zero-copy апгрейд, не
архитектурная опора.

**Шейдеры — третье дерево исходников, не переиспользование GLSL Android напрямую**:
Vulkan GLSL — отдельный диалект (явные `layout(set=, binding=)`, инвертированный
clip-space Y и диапазон глубины `[0,1]` вместо GL-шного `[-1,1]`, нет рантайм-компиляции
вообще — только precompiled SPIR-V через `vkCreateShaderModule`). Переиспользуется только
*логика* (математика цветокоррекции/виньетки/частиц, уже общая в C++), не текст шейдеров.
`native/src/render/vulkan/shaders/*.vert|*.frag` компилируются офлайн через
`glslangValidator` (CMake custom command → `.spv`, gitignored). Это делает Windows-шов
ближе к прекомпилированной модели iOS (`.metallib`), чем к рантайм-GLSL Android — список
платформенных швов из начала §2 расширяется третьим вариантом: офлайн-кросс-компиляция
в SPIR-V, рантайм-компиляции нет вовсе. Выбран GLSL (не HLSL/DXC) — не добавляет
четвёртый шейдерный язык, раз Metal тут не участвует.

---

## 3. Граница dart:ffi / Platform Channel

Рендер-цикл камера → C++ → GPU → текстура Flutter → энкодер — **целиком
native-to-native**, через Dart VM не идёт вообще (ни FFI, ни channel, даже
покадрово через FFI — риск без выгоды).

**Platform Channel**: активация/статус лицензии, жизненный цикл камеры
(открыть/закрыть/переключить — редкие вызовы), файловая система/галерея,
экспорт/шаринг, lifecycle приложения, device fingerprint (`ANDROID_ID`, Keychain UUID) —
всё, что требует OS API именно из Kotlin/Swift.

**dart:ffi**: регистрация текстуры (один раз), параметры эффекта (десятки Hz при
движении слайдера — важна отзывчивость), опрос статистики (2–4Hz debug-оверлей),
старт/стоп записи (чтобы конечный автомат записи целиком жил в C++-сессии).

Публичный C API (единственный контракт для JNI, Obj-C++ и dart:ffi):

```c
// lumacore_api.h
typedef struct {
  float brightness, contrast, saturation, vignetteRadius, vignetteSoftness, particleIntensity;
  int64_t effectMask;
} LumaEffectParams;

typedef struct {
  double fps, avgFrameMs;
  uint32_t droppedFrames;
  int32_t thermalState;
} LumaStats;

int64_t lumacore_render_init(void* platformSurfaceOrCtx, int width, int height);
void    lumacore_set_effect_params(int64_t session, const LumaEffectParams* params);
void    lumacore_get_stats(int64_t session, LumaStats* outStats);
int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h);
int32_t lumacore_stop_recording(int64_t session);
void    lumacore_release(int64_t session);
```

### Асимметрия показа текстуры (Android push vs iOS pull) — не «одинаковый интерфейс, два бэкенда»

- **Android — push**: Kotlin получает `SurfaceTextureEntry` от
  `TextureRegistry.createSurfaceTexture()` (обязан быть в Kotlin — Flutter
  Android-embedding API, недоступен из C++), конвертирует в `ANativeWindow` через JNI
  (`ANativeWindow_fromSurface`), передаёт в C++, где `eglCreateWindowSurface` рендерит
  прямо в поверхность Flutter. Texture id передаётся в Dart один раз через Platform
  Channel для `Texture(textureId: id)`.
- **iOS — pull**: у Flutter нет аналога «рендерить прямо в живую поверхность» для
  внешних текстур. Регистрируется Obj-C++ объект, реализующий `FlutterTexture`; raster
  thread Flutter синхронно вызывает `copyPixelBuffer()` по запросу. Нужен
  `CVPixelBufferPool` (2 буфера), рендер в Metal-текстуру вокруг «следующего» буфера
  (zero-copy, IOSurface-backed), atomic swap «последнего готового» указателя,
  `copyPixelBuffer()` возвращает retain на него, затем
  `[registry textureFrameAvailable:textureId]`.
- **Windows — pull, как iOS, не push, как Android** (наивное ожидание «у Windows прямой
  доступ к GPU/ОС, значит мост не нужен» — неверное; структурно Windows ближе к iOS).
  Flutter Windows embedder **не поддерживает нативно Vulkan external textures**
  (`flutter/flutter#117937`, открыт с 2023, без ETA) — единственный GPU-resident путь —
  `FlutterDesktopGpuSurfaceDescriptor` с `kFlutterDesktopGpuSurfaceTypeD3d11Texture2D`.
  Обхода через Vulkan напрямую нет. Более того, прямая передача текстуры из своего
  `ID3D11Device` ненадёжна (`flutter/flutter#121046` — движок Flutter внутри использует
  ANGLE и собственное D3D11-устройство, отличное от вашего) — нужен **shared NT handle**:
  1. Свой `ID3D11Device`/context (нужен и для MF-импорта, см. §2).
  2. `ID3D11Texture2D` с флагом `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`.
  3. `IDXGIResource1::CreateSharedHandle` → Win32 `HANDLE`.
  4. Vulkan-сторона: `VkExternalMemoryImageCreateInfo`
     (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`) + `vkAllocateMemory` с
     `VkImportMemoryWin32HandleInfoKHR` по тому же handle → `VkImage` и `ID3D11Texture2D`
     теперь аллайзят одну GPU-аллокацию.
  5. Финальный проход рендерится прямо в этот image, регистрируется через
     `FlutterDesktopGpuSurfaceTextureConfig`, обновление —
     `FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable`; raster-поток
     Flutter тянет дескриптор синхронно на своём такте.
  Синхронизация между API — намеренно упрощённая: CPU-side `vkWaitForFences` после
  `vkQueueSubmit` на render-потоке перед публикацией «последнего готового» индекса
  (пул 2–3 пар текстур) — консервативнее полного cross-API GPU fence
  (`VK_KHR_external_semaphore_win32`), но не требует второго sync-примитива на MVP.
  **План Б**, если interop не укладывается в сроки: `kFlutterDesktopPixelBufferTexture` —
  обычное CPU pixel-buffer копирование без GPU-interop, без claim про zero-copy, но
  рабочий откат. Решение по плану Б — на фиксированную дату, до того как interop съест
  больше бюджета.

---

## 4. Собственная минимальная сборка FFmpeg

**Конфигурация**: `--disable-everything`, точечный re-enable —
`--enable-avcodec --enable-avformat --enable-swscale`,
`--disable-doc --disable-programs --disable-network --disable-filters --disable-bsfs --disable-hwaccels`
(кроме явно включённого ниже), `--enable-protocol=file`, `--enable-muxer=mp4`.

### GPL-ловушка libx264 — решение
libx264 требует `--enable-gpl`, что делает всю сборку FFmpeg GPL — несовместимо с
закрытым портфолио-приложением без покупки коммерческой лицензии x264.com.
**libx264 не используем.** Вместо этого — hardware-энкодеры через обёртки FFmpeg:
`h264_mediacodec` на Android (`--enable-mediacodec --enable-jni`) и `h264_videotoolbox`
на iOS (`--enable-videotoolbox`, фреймворки `VideoToolbox`/`CoreMedia`/`CoreVideo`).
Остаётся LGPL-чистым и ближе к реальной мобильной практике (батарея/нагрев). Энкодер
MediaCodec (не только декодер) добавлен в FFmpeg 6.0 (2023); VideoToolbox-энкод —
давно устоявшаяся функциональность.

**Не проверено — прототипировать на этапе 3**: принимает ли `h264_mediacodec`-энкодер
GPU-резидентный Surface/hardware-buffer на вход, или только CPU YUV-буфер — влияет на
честность заявления «zero-copy end-to-end» в README.

**Декод**: сознательно не реализуется в FFmpeg. Миниатюры галереи —
`MediaMetadataRetriever` (Android) / `AVAssetImageGenerator` (iOS), воспроизведение —
через системный плеер/шаринг. Ядро остаётся строго write-path, что не ослабляет
историю про кодеки/контейнеры (encode+mux — более сложный и релевантный навык).

**Аудио-энкодер**: программный AAC (`--enable-encoder=aac`) — без лицензионных
вопросов, дёшево по CPU.

**Тулчейн**:
- Android: NDK clang, `arm64-v8a` обязательно (+ опционально `armeabi-v7a`).
- iOS: `xcodebuild`/clang cross-compile, device + Apple-Silicon-simulator arm64 срезы
  (референс-скрипты вроде kewlbear/FFmpeg-iOS-build-script — как шаблон процесса,
  конфигурация своя минимальная).
- Собранный FFmpeg статически линкуется в единый `liblumacore` (.so на ABI Android /
  `LumaCoreKit.xcframework` для iOS) — отдельно FFmpeg `.so` не поставляется.

### Windows
Тулчейн: сборка под MSYS2 с `--toolchain=msvc` (`cl.exe` — реальный компилятор, MSYS2
даёт только unix-подобное окружение для `./configure`/`make` самого FFmpeg). И MSVC, и
MinGW-w64 официально поддерживаются сборочной системой FFmpeg — но именно MSVC-тулчейн
нужен здесь, потому что весь остальной Windows-таргет `native/CMakeLists.txt` и Flutter
Windows-приложение собираются MSVC; смешивание MinGW-собранной статической библиотеки
(тянет `libgcc`/`libwinpthread`) с MSVC-линкуемым `lumacore.dll` — ABI/рантайм-риск на
пустом месте. Готовые Windows-бинарники (gyan.dev, BtbN) сознательно не используются —
по той же причине, по которой отвергнут `ffmpeg-kit-flutter`: собраны MinGW, и не дают
контроля над `--disable-everything`/no-libx264/no-GPL конфигурацией, на которой уже
построен весь проект.

Hardware-энкодер: **`h264_mf`** (обёртка над Media Foundation Transform) — структурный
аналог `h264_mediacodec`/`h264_videotoolbox`: оборачивает OS-уровневую абстракцию
энкодера, а не конкретного вендора. Гарантированно работает на любом железе ревьюера
(включая голые VM без дискретной GPU) за счёт встроенного софтверного H.264 MFT
Microsoft (Windows 8+), опционально подхватывает аппаратные MFT (Quick Sync/NVENC/AMF)
без прямой зависимости от вендорских SDK. `NVENC`/`QSV`/`AMF` напрямую сознательно не
используются — вендор-лок для портфолио-проекта, который должен запускаться на
произвольном железе. Требует `--enable-mediafoundation`, линковка
`mfplat`/`mfuuid`/`strmiids`/`ole32`.

**Не проверено — тот же класс риска, что для MediaCodec/VideoToolbox (§8)**: принимает
ли `h264_mf` (`libavcodec/mfenc.c`) D3D11-resident вход (`MF_SA_D3D11_AWARE`/DXGI-buffer)
или всегда требует CPU-side NV12 внутри — не заявлять zero-copy для Windows-энкода без
прототипа.

Декод: без изменений от общей политики — не реализуется в FFmpeg. Миниатюры галереи —
через `IMFSourceReader` в режиме единичного кадра или WinRT
`StorageItemThumbnail` (конкретный выбор — на этапе реализации), не через FFmpeg-декод.

---

## 5. Структура `native/` (CMake)

```
native/
  CMakeLists.txt                 # один проект → цель `lumacore`
  cmake/                         # android.toolchain.cmake (NDK), leetal/ios-cmake
  third_party/ffmpeg/            # git submodule, pinned tag
  third_party/ffmpeg_build/      # gitignored, собранные статические либы по ABI/arch
  src/api/lumacore_api.h(.cpp)   # единственный стабильный публичный C API
  src/render/
    IRenderBackend.h  RenderPipeline.cpp  EffectParams.h
    gl/GLRenderBackend.cpp  gl/shaders/*.glsl
    metal/MetalRenderBackend.mm  metal/shaders/*.metal
  src/encode/                    # EncoderSession: avcodec/avformat + mediacodec/videotoolbox glue
  src/license/                   # TokenValidator — fingerprint приходит снаружи,
                                  # C++ его сам не добывает (тестируемость на desktop)
  src/util/                      # кольцевой буфер, thread utils, логирование
  jni/lumacore_jni.cpp           # только JNIEXPORT-функции, тонкий слой, без логики
  objcxx/LumaCoreBridge.mm(.h)   # только Obj-C++ glue, тонкий слой, без логики
  tests/                         # pure-logic юниты (математика частиц, парсинг токена,
                                  # ring buffer) — на обычном Linux CI без устройств
```

Android Gradle указывает `externalNativeBuild` прямо на `native/CMakeLists.txt` (второй
CMakeLists.txt под `android/` не создаётся); iOS собирает тот же проект с iOS toolchain
file (device + simulator arm64), затем `xcodebuild -create-xcframework` упаковывает обе
статические либы + заголовки `src/api/`. Один и тот же C API для обеих платформ —
рассинхрон заголовков ловится на этапе сборки, а не молча.

### Windows-ветка
- `WIN32`-guarded блок в том же `native/CMakeLists.txt` (без отдельного проекта),
  по аналогии с существующими Android/iOS toolchain-ветками. Генератор — MSVC
  (Visual Studio 17 2022 или Ninja+`cl.exe`), **только x64** — Windows on ARM осознанно
  вне скоупа (Flutter его поддерживает, но это дополнительно расширяет и так рискованную
  Vulkan/драйверную поверхность).
- `find_package(Vulkan REQUIRED)` — встроенный `FindVulkan` (CMake 3.7+), резолвится
  через переменную окружения `VULKAN_SDK` (ставится инсталлятором LunarG Vulkan SDK).
  **Новое требование к CI**: GitHub Actions Windows-раннеры не имеют Vulkan SDK
  предустановленным (в отличие от NDK/Xcode на своих раннерах) — нужен явный шаг
  установки в `.github/workflows/`.
- Windows требует явного экспорта символов (`__declspec(dllexport/dllimport)`, не как
  в ELF `.so` по умолчанию) — единственное реальное изменение уже написанного
  кросс-платформенного файла: в `src/api/lumacore_api.h` добавляется макрос
  `LUMACORE_API`, разворачивающийся в `__declspec(...)` под `_WIN32` и в no-op на
  остальных платформах.
- Артефакт — `lumacore.dll` + `lumacore.lib`, статическая линковка MSVC-собранного
  FFmpeg `.lib` — тот же принцип «один артефакт на платформу, FFmpeg отдельно не
  поставляется», что для `.so`/`.xcframework`.
- **`native/win32/` — третий тонкий слой, но по другой причине, чем `jni/`/`objcxx/`.**
  Те существуют, чтобы перейти языковую/ABI-границу к Kotlin/Swift. Windows Flutter
  desktop-плагины — обычный C++, языкового моста не нужно. `native/win32/` — это папка
  Windows-специфичных хелперов (создание D3D11-устройства и shared-текстуры, DPAPI
  fingerprint-хелпер, настройка MF work queue) — вещи, которые не входят в переносимое
  ядро из-за специфичности Windows API, а не из-за пересечения языковой границы.
- Windows platform glue (обработчики method channel, texture-registrar wiring, MF
  capture callback) — в `windows/runner/`, по аналогии с
  `android/app/src/main/kotlin/...` и `ios/Runner/...` (glue внутри app-проекта, не
  отдельный pub-плагин).

---

## 6. Лицензионный модуль

**Ed25519 (libsodium) вместо ECDSA** — см. отклонения выше.

Токен — подписанная бинарная структура (не сырой JSON — канонизация JSON классическая
ловушка при верификации), обёрнута + подпись в base64 внутри JSON-конверта для
хранения/передачи:

```json
{
  "license_id": "uuid",
  "device_fingerprint_hash": "sha256(device_fingerprint)",
  "issued_at": 1737200000,
  "expires_at": 1768736000,
  "plan": "portfolio-demo",
  "sig_alg": "ed25519",
  "key_id": "v1",
  "payload_b64": "...",
  "signature_b64": "..."
}
```

**Device fingerprint**:
- Android: `SHA256(ANDROID_ID + package_name)` — приемлемо для MVP; сбрасывается при
  factory reset, переживает переустановку.
- iOS: `identifierForVendor` не подходит (сбрасывается при удалении приложения) —
  генерируем случайный UUID при первом запуске, храним в Keychain (переживает
  переустановку, привязан к устройству+команде подписи), `SHA256(keychainUUID + bundle_id)`.
  Это *мягкая* привязка, не hardware attestation — та часть (App Attest/Play Integrity)
  осознанно отложена в Task.md §9.
- Windows: единого системного аналога `ANDROID_ID` нет. Рассмотрены три варианта:
  `MachineGuid` (`HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`) — простое чтение
  реестра, но **коллизирует между машинами, склонированными из VM-образа без sysprep**
  (для ревьюера, открывающего проект в клонированной VM-снапшоте — реалистичный, не
  экзотический сценарий); WMI `Win32_ComputerSystemProduct.UUID` (SMBIOS UUID) —
  переживает даже переустановку Windows, что **нарушает уже принятый принцип мягкой
  привязки** (слишком сильная связка, плюс известны пустые/дублирующиеся значения на
  части virtualized/бюджетного железа). **Выбор: самогенерируемый UUID + DPAPI**
  (`CryptProtectData`, user-scope) — ближе всего по духу к iOS-паттерну
  (Keychain-persisted random UUID): приложение само владеет идентификатором вместо
  чтения системного, не подвержено проблеме клонированных VM (свежий случайный UUID на
  установку, а не общее для клонов значение), переживает переустановку приложения, пока
  цел `%LOCALAPPDATA%`. Хранится в `%LOCALAPPDATA%\LumaCore\device_id.bin`.
  Fingerprint = `SHA256(dpapi_uuid + MachineGuid_как_соль + "lumacore.windows")` —
  `MachineGuid` используется только как доп. энтропия «в глубину», не как основа, чтобы
  ограничить, а не унаследовать его риск коллизий на клонированных VM.

**Offline-валидация** (без сети после первой активации):
1. `POST /activate {deviceFingerprint, licenseKey}` → mock-бэкенд проверяет
   `licenseKey` по таблице demo-ключей, подписывает токен приватным Ed25519-ключом.
2. Приложение хранит блоб токена в Keychain/EncryptedSharedPreferences. В бинарь C++
   зашит только публичный ключ.
3. `lumacore_validate_license(token_blob, fingerprint)` — чистая офлайн-крипто
   (подпись, fingerprint, expiry) → `VALID/EXPIRED/DEVICE_MISMATCH/INVALID_SIGNATURE/NOT_ACTIVATED`.
4. `/validate` на mock-бэкенде демонстрирует REST-контракт из вакансии; архитектура
   намеренно офлайн-верифицируема. Trade-off, не замалчиваемый: раз валидация полностью
   офлайн, мгновенный server-side revoke не блокирует уже активированное устройство без
   периодической онлайн-проверки.

---

## 7. Структура репозитория

```
LumaCore/
  README.md
  docs/                            # ARCHITECTURE.md, Task.md, Vacancy.md (Task.md/Vacancy.md — gitignored)
  native/                         # см. §5
    src/render/vulkan/            # VulkanRenderBackend, shaders/*.vert|*.frag (+ compiled/*.spv, gitignored)
    src/interop/                  # D3D11VulkanBridge — shared NT handle, оба направления
    win32/                        # DPAPI fingerprint helper, MF work-queue setup
  tools/ffmpeg/build_android.sh  build_ios.sh  build_windows.ps1  common.sh
  android/app/src/main/kotlin/... # CameraX, TextureRegistry glue, MethodChannel handlers
  ios/Runner/...                  # AVFoundation, FlutterTexture glue, method channel handlers
                                   # LumaCoreKit.xcframework — собирается CI, gitignored
  windows/runner/                 # MF capture callback, WASAPI, texture-registrar wiring,
                                   # method channel handlers (та же роль, что android/ios)
  lib/
    main.dart
    core/                         # ffigen-биндинги из lumacore_api.h, channel wrappers
    features/license/  features/camera/  features/gallery/  features/settings/
    widgets/
  server/                         # mock license backend — FastAPI, pynacl Ed25519
    app/  tests/
  .github/workflows/               # + Windows job, установка Vulkan SDK
  docs/                           # архитектурная схема, профилирование до/после, демо
```

---

## 8. Риски (честно, без приукрашивания)

1. **iOS FFmpeg + Metal + CVPixelBuffer zero-copy цепочка — самая рискованная область.**
   Кросс-компиляция FFmpeg под iOS с нуля (без ffmpeg-kit) — многодневная задача даже с
   готовыми скриптами как ориентиром. Плюс не проверено, принимает ли
   `h264_videotoolbox`/`h264_mediacodec` GPU-резидентный вход без CPU-хопа —
   прототипировать до того, как «1–1.5 недели» в roadmap Task.md будут считаться
   реалистичными.
2. **Интеграция текстур Flutter на iOS структурно отличается от Android** (push vs
   pull, см. §3), а не симметрична, как подразумевает формулировка Task.md. Плавный
   preview 30/60fps без разрывов/задержек через pull-модель, синхронизированную с
   Metal render-потоком, — отдельная точка риска, не просто «скомпилировалось — значит
   работает».
3. **Паритет GLSL/MSL — постоянный ручной риск**, не гарантия общего C++-интерфейса.
   Разная точность по умолчанию, разные конвенции Y-flip/NDC, независимо написанные
   шейдеры — картинка может визуально разъезжаться между платформами. Нужен явный шаг
   side-by-side сравнения скриншотов.
4. **Корректность флагов минимальной сборки FFmpeg** (расположение moov-атома/faststart,
   AVCC vs Annex-B bitstream filter для MP4) может незаметно съесть время. «Один
   корректно замукшированный, играющийся в стандартном плеере MP4» — отдельная веха до
   старта работы над GPU-шейдерами.
5. **D3D11↔Vulkan interop для шаринга текстуры с Flutter — подтверждённо обязателен, не
   опционален, и независимо хрупок.** Flutter Windows embedder не имеет нативной
   поддержки Vulkan external textures (`flutter/flutter#117937`, открыт с 2023, без ETA) —
   D3D11 GPU-surface путь единственный. Даже он имеет открытые community-репорты о
   ненадёжности прямой передачи текстуры между устройствами (`flutter/flutter#121046`),
   отсюда обязательность shared-NT-handle обхода. Прототипировать именно этот путь
   (пустой треугольник через Vulkan, отданный в реальный `Texture()`-виджет) — первая
   веха Windows-работы, до всего остального пайплайна, с pixel-buffer планом Б на
   фиксированную дату решения.
6. **Формат вывода Media Foundation по факту не проверен и вероятно CPU-side по
   умолчанию**, а не GPU-resident `ID3D11Texture2D`, как можно наивно предположить из
   «MF архитектурно ориентирован на D3D11». Дизайн уже закладывает CPU-buffer путь как
   базовый (§2), но нужна ранняя проверка на реальном железе — от этого зависит, можно
   ли вообще заявлять «zero-copy end-to-end» для Windows, тот же класс риска, что уже
   отмечен для MediaCodec/VideoToolbox выше.
7. **Vulkan-boilerplate аддитивен поверх обоих interop-рисков, а не отдельная статья
   расходов** — это третий полноценный платформенный пайплайн без переиспользования
   API-уровня кода от GL/Metal (общая только C++-оркестрация/математика, тот же шов, что
   уже есть, но ceremony на каждый метод общего интерфейса у Vulkan заметно выше:
   explicit memory allocation через VMA, explicit barriers между 3 проходами, explicit
   descriptor sets — там, где GL/Metal делают многое неявно или с меньшим церемониалом).
   Реалистичное планирование должно закладывать на один Vulkan-бэкенд время, сравнимое с
   GL+Metal бэкендами вместе взятыми — Windows стоит планировать отдельной, более поздней
   фазой относительно оценок Task.md, а не в рамках того же бюджета на платформу.

**Android→Vulkan ретрофит — явно вне скоупа, одна строка на будущее.** Добавление
Vulkan под Windows создаёт соблазн перевести на него и Android (у которого Vulkan уже
есть нативно). Это уже область Task.md §9 «что дальше»; GL ES на Android и так закрывает
буквальный пункт вакансии «OpenGL ES»; сейчас это утроило бы Vulkan-поверхность без
новой пользы для соответствия вакансии. Одна фраза в README «что дальше» — не более,
и точно не реализация.

---

## Соответствие таблице требований вакансии

Все решения выше остаются в рамках таблицы «требование вакансии → реализация» из
Task.md §3 — этот документ детализирует *как*, не меняет *что*. Единственные
содержательные отклонения от Task.md зафиксированы в начале документа.
