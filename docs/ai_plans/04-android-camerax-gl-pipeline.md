# Этап 2+3+4 для Android: захват камеры (CameraX) + GL-эффекты + запись — первый вертикальный срез

## Контекст

iOS-путь полностью готов и проверен на устройстве: захват камеры, 5-эффектный Metal-пайплайн
(color correction/vignette/particles/sepia/edge-detection), запись видео+аудио через FFmpeg,
офлайн-лицензирование (Ed25519), dart:ffi, settings (см. `docs/ai_plans/01-03`). Android сейчас —
чистый Flutter-шаблон: единственный реальный код — `MainActivity.kt` с одним недоделанным методом
(`getDeviceFingerprint` → `notImplemented()`), плюс уже существующие, но нетронутые C++-заглушки
(`GLRenderBackend`, `native/jni/lumacore_jni.cpp` с 2 из 8 нужных JNI-функций). Ни CameraX, ни
JNI-моста, ни GLSL-шейдеров, ни FFmpeg-сборки под Android, ни `externalNativeBuild` в Gradle —
ничего этого не существует. Разведка (3 параллельных Explore-агента) прошла по текущему
состоянию `android/`, referenced-стабам и полному iOS-коду для зеркалирования.

Инструмент подтверждён в dev-окружении: Android SDK/NDK (25.1/26.3/27.0), CMake 3.22.1, adb,
эмулятор с готовым AVD (`Pixel_3a_API_34_extension_level_7_arm64-v8a`) — можно реально собирать
и тестировать, физического Android-устройства нет.

---

## Ключевое архитектурное отличие Android от iOS (не «то же самое, но GLES»)

- **iOS = pull-модель**: `CVPixelBufferPool` → Flutter вызывает `copyPixelBuffer()` когда ему
  нужно. **Android = push-модель**: Kotlin получает `SurfaceTextureEntry` от
  `TextureRegistry.createSurfaceTexture()`, конвертит в `ANativeWindow` через JNI
  (`ANativeWindow_fromSurface`), C++ рендерит **прямо в него** через `eglCreateWindowSurface` +
  `eglSwapBuffers`. Нет отдельного `exportForPreview()`-шага с CPU-хендлом — GPU-запись это и
  есть показ кадра.
- **Два разных Surface, не один**: (1) внутренний `SurfaceTexture`, привязанный к
  `GL_TEXTURE_EXTERNAL_OES`-текстуре, куда CameraX пишет кадры камеры (создаётся и живёт на
  GL-потоке); (2) Flutter-предоставленный `ANativeWindow` — целевой surface показа. Пайплайн
  сэмплит из (1), рендерит в (2).
- **Импорт кадра камеры принципиально другой**: iOS получает NV12 (раздельные Y/CbCr-плоскости)
  через `CVMetalTextureCacheCreateTextureFromImage` дважды. Android's `GL_TEXTURE_EXTERNAL_OES` +
  `samplerExternalOES` **уже отдаёт RGB** при сэмплинге (конверсия YUV→RGB происходит на уровне
  драйвера/GPU сэмплера) — отдельной Y-плоскости для дешёвого Sobel-семплинга нет, luma для
  edge-detection считается из RGB (`dot(rgb, lumaWeights)`) на каждый тап вместо прямого чтения
  Y-текстуры.
- **`EncoderSession::submitFrame` сейчас жёстко на `CVPixelBufferRef`** (CoreVideo-специфично) —
  не переиспользуем как есть для Android. Нужен маленький платформо-независимый рефакторинг (см.
  Раздел A) — **без изменения уже работающего iOS-пути** (просто платформенное ветвление внутри
  одной функции, а не переделка контракта).
- **Один GL-поток на весь путь**: `RenderPipeline`/`GLRenderBackend` уже неявно требуют
  single-thread (см. `RenderPipeline.cpp`'s комментарий про lock-free stats-буфер) — на Android
  это жёстче, чем на Metal, т.к. `EGLContext` привязан к потоку буквально (GL-вызовы с другого
  потока — undefined behavior). Нужен выделенный `HandlerThread`/`GLRenderThread`, который
  инициализирует `EGLContext` один раз и остаётся владельцем на всю жизнь backend'а.
- **Нет прекомпилированных шейдеров**: GLSL ES компилируется на устройстве в рантайме
  (`glCompileShader`), в отличие от предкомпилированного `.metallib`. Ошибки компиляции шейдера
  теперь ловятся только на устройстве/эмуляторе (`glGetShaderiv(GL_COMPILE_STATUS)`), не на
  этапе сборки — тестовая стратегия соответственно смещается позже по циклу.
- **`effectMask` — `int64_t`/`long` на C++/Metal стороне, но GLSL ES 3.0 не имеет 64-битного
  int** — все текущие 5 битов (`0x1`..`0x10`) умещаются в 32 бита, поэтому усечение до `int32_t`
  перед загрузкой в UBO безопасно уже сейчас, но явно фиксируется как осознанное ограничение.

---

## Раздел A — Общий нативный слой (без изменения работающего iOS-пути)

### A.1 `EncoderSession` — платформо-независимый CPU-буфер вместо `CVPixelBufferRef`

Не переписываем контракт `submitFrame`/`exportForEncoder` целиком — добавляем платформенную
ветку **внутри** уже существующего `#ifdef LUMACORE_HAVE_FFMPEG` блока, рядом с текущей
CVPixelBuffer-веткой, не трогая её:

```cpp
// native/src/encode/EncoderSession.h — новый платформо-независимый дескриптор,
// экспортируется наравне с submitFrame(void*, int64_t)
struct NativeNV12Buffer {
  const uint8_t* yPlane;
  size_t yStride;
  const uint8_t* uvPlane;  // interleaved CbCr (or UV depending on platform convention)
  size_t uvStride;
  int width;
  int height;
};
```
`EncoderSession::submitFrame` остаётся принимать `void* platformImageHandle` (не ломаем iOS
call-сайт в `lumacore_api.cpp`), но внутри `.cpp`:
```cpp
#if defined(__APPLE__)
  auto pixelBuffer = static_cast<CVPixelBufferRef>(platformImageHandle);
  // ... существующий CVPixelBufferLockBaseAddress-путь без изменений ...
#elif defined(__ANDROID__)
  auto* buf = static_cast<const NativeNV12Buffer*>(platformImageHandle);
  // тот же цикл копирования по строкам, что уже есть для iOS, только источник — buf->yPlane/uvPlane
#endif
```
`GLRenderBackend::exportForEncoder()` на Android аллоцирует `NativeNV12Buffer` на куче (+
буфер данных), `lumacore_api.cpp`'s `lumacore_render_frame` уже освобождает
`exportForEncoder()`-результат через `releaseEncoderExport()` после `submitFrame` — эта функция
получает свою `#elif defined(__ANDROID__)` ветку (`delete` вместо `CVPixelBufferRelease`).
**Нулевой риск для iOS**: существующая ветка не редактируется, только оборачивается в
`#if defined(__APPLE__)`.

### A.2 FFmpeg Android cross-compile

`tools/ffmpeg/build_android.sh` сейчас — гарантированно падающая заглушка. FFmpeg-сорс уже
провендорен в `native/third_party/ffmpeg/` (реально, не плейсхолдер — уже использовался для
iOS-пересборки в прошлой сессии). Нужно:
- Добавить `FFMPEG_ANDROID_FLAGS` в `tools/ffmpeg/common.sh`: `--enable-mediacodec --enable-jni
  --enable-encoder=h264_mediacodec --target-os=android --enable-cross-compile --enable-pic`.
- **Риск, явно зафиксированный в ARCHITECTURE.md §4 как непроверенный**: принимает ли
  `h264_mediacodec` GPU-resident Surface-вход или только CPU YUV-буфер. **План осознанно
  выбирает безопасный путь**: Android's `GLRenderBackend::exportForEncoder()` делает GPU→CPU
  readback (как и iOS делает CPU-copy, не zero-copy VideoToolbox-путь) — `h264_mediacodec`
  получает обычный CPU NV12-буфер через тот же `avcodec_send_frame`-путь, что уже работает для
  `h264_videotoolbox`. Zero-copy Surface-вход в MediaCodec — не в этом плане, явный fast-follow.
- Тулчейн: `$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/*-clang`, ABI
  `arm64-v8a` (единственная цель — эмулятор и все современные устройства уже arm64; armeabi-v7a
  осознанно не собираем, как и на iOS x86_64-симулятор осознанно исключён).
- Вывод в `native/third_party/ffmpeg_build/android/arm64-v8a/`, аналогично
  `ffmpeg_build/ios/{device,sim}-arm64/`.
- `native/CMakeLists.txt`'s `if(ANDROID)` ветка получает линковку этих статиков (аналогично
  iOS-ветке, `target_include_directories`/`target_link_libraries` на `lumacore_logic`) +
  `-landroid -lmediandk` (для `MediaCodec`/`ANativeWindow` JNI-related символов, если
  `--enable-jni`/`--enable-mediacodec` их тянут).

### A.3 JNI-мост — дополнить `native/jni/lumacore_jni.cpp`

Уже есть 2 из 8: `nativeRenderInit`, `nativeRelease` (класс `com.lumacore.native.LumaCoreBridge`
уже зафиксирован в существующих JNI-именах, дальше используем тот же namespace). Добавить
недостающие 6 (зеркало `native/objcxx/LumaCoreBridge.mm`):

```cpp
JNIEXPORT void JNICALL Java_..._nativeSetEffectParams(JNIEnv*, jobject, jlong session,
    jfloat brightness, jfloat contrast, jfloat saturation, jfloat vignetteRadius,
    jfloat vignetteSoftness, jfloat particleIntensity, jlong effectMask,
    jfloat sepiaAmount, jfloat edgeThreshold, jfloat edgeIntensity);
JNIEXPORT jobject JNICALL Java_..._nativeGetStats(JNIEnv*, jobject, jlong session);  // возвращает Kotlin data-класс через NewObject
JNIEXPORT void JNICALL Java_..._nativeSetThermalState(JNIEnv*, jobject, jlong session, jint state);
JNIEXPORT jint JNICALL Java_..._nativeStartRecording(JNIEnv*, jobject, jlong session, jstring outPath, jint bitrateKbps, jint w, jint h);
JNIEXPORT jint JNICALL Java_..._nativeRenderFrame(JNIEnv*, jobject, jlong session, jlong ptsUs);  // кадр камеры уже в GL-текстуре, не передаётся явно — см. ниже
JNIEXPORT jint JNICALL Java_..._nativeStopRecording(JNIEnv*, jobject, jlong session);
JNIEXPORT jint JNICALL Java_..._nativeSubmitAudioFrame(JNIEnv*, jobject, jlong session, jbyteArray pcmData, jint numFrames, jint sampleRate, jint numChannels, jlong ptsUs);
JNIEXPORT jint JNICALL Java_..._nativeValidateLicense(JNIEnv*, jobject, jstring tokenBlobJson, jstring deviceFingerprint);
```

**Важное отличие от iOS**: `lumacore_render_frame(session, cameraFrame, ptsUs, &outPreview)` на
iOS получает `cameraFrame` явным аргументом (`CVPixelBufferRef`) и возвращает превью явным
`out`-параметром. На Android камера уже сидит в GL-текстуре (`updateTexImage()` уже вызван на
GL-потоке до вызова `nativeRenderFrame`), а превью — это прямая запись в `ANativeWindow`
(`eglSwapBuffers`, ничего не возвращается вызывающему). Поэтому `nativeRenderFrame` на Android
**не принимает `cameraFrame`/не возвращает превью** — только `ptsUs`. `RenderPipeline::renderFrame`
(платформо-независимый, уже существует) продолжает работать с той же сигнатурой
`(NativeImageHandle cameraFrame, double elapsedSeconds)` — `GLRenderBackend::importExternalFrame`
на Android просто игнорирует переданный `NativeImageHandle` (или получает `nullptr`) и вместо
этого использует уже-текущую externally-bound OES-текстуру как свой internal state (заранее
забинженную `updateTexImage()`-вызовом на Kotlin-стороне перед вызовом JNI) — задокументировать
это явно как единственную platform-specific асимметрию в контракте `IRenderBackend`, не меняя
сам интерфейс.

### A.4 Device fingerprint

`MainActivity.kt`'s `getDeviceFingerprint` → `SHA256(Settings.Secure.ANDROID_ID + packageName)`
(ARCHITECTURE.md §6, уже задокументировано дословно) — hex-строка, без Keychain-аналога (не
нужен self-persisted UUID, `ANDROID_ID` уже стабилен между запусками до factory reset). Вызов
`TokenValidator::validate` через новый JNI `nativeValidateLicense` (native-код уже полностью
портативен — `TokenValidator.cpp`/vendored ed25519/sha256 не имеют платформенных `#ifdef`).

---

## Раздел B — OpenGL ES 3.0 рендер-бэкенд (`GLRenderBackend`)

Механический порт Metal-математики (уже вся захвачена в разведке) под GLSL ES 3.0, с
единственной содержательной разницей — импорт кадра камеры (external OES вместо раздельных
Y/CbCr текстур).

### B.1 Структура прохода (аналог iOS §2/§3 из плана 03)

- **ColorCorrection-эквивалент**: fragment-шейдер сэмплит `samplerExternalOES` напрямую (уже
  RGB), применяет brightness/contrast/saturation/sepia/edge-detection (тот же код, что в
  `ColorCorrection.metal`, luma для Sobel — из RGB-тапов, а не отдельной Y-текстуры) → рендерит
  в персистентную FBO-текстуру `colorCorrectRGBA` (аналог iOS).
- **Vignette**: 1:1 порт `Vignette.metal` (нет платформенных отличий — обычный RGBA→RGBA проход).
- **Particles**: composite blit (`vignetteRGBA`→`particleCompositeRGBA`, `GL_FRAMEBUFFER`) +
  инстансированные квады (`glDrawArraysInstanced`, аддитивный блендинг `GL_SRC_ALPHA, GL_ONE`) —
  1:1 порт `Particles.metal`'s `particleVS`/`particleFS`, буфер инстансов — обычный VBO
  (`glBufferData` каждый кадр, 200×16 байт — дёшево, как и на iOS).
- **Presentation** (аналог iOS `endFrame()`/preview-экспорта, но другой механизм): финальный
  blit `particleCompositeRGBA` → напрямую в `ANativeWindow`-привязанный `EGLSurface`
  (`eglMakeCurrent` на него, полноэкранный треугольник, `eglSwapBuffers`). Это не отдельная
  Metal-подобная NV12-конверсия — presentation surface принимает RGBA напрямую (Android composits
  сам).
- **Recording export** (`exportForEncoder`): GPU-side RGB→YCbCr конверсия (1:1 порт
  `nv12YFS`/`nv12CbCrFS` математики) в две маленькие offscreen FBO-текстуры (полный размер для Y,
  половинный для CbCr — билинейный даунсемпл на семпле, как и на iOS) → `glReadPixels` каждой в
  CPU-память → упаковка в `NativeNV12Buffer` (Раздел A.1).

### B.2 EGL-контексты и потоки

`GLRenderBackend::initialize(RenderContextParams)`:
1. `eglGetDisplay`/`eglInitialize`/`eglChooseConfig` (RGBA8, без глубины/трафарета — 2D-пайплайн).
2. `eglCreateContext` — **один контекст на весь жизненный цикл backend'а**, создаётся и
   используется **только на выделенном GL-потоке** (`HandlerThread` на Kotlin-стороне, вызывающем
   JNI). `RenderPipeline`/`IRenderBackend` не знают о потоках — дисциплина обеспечивается
   вызывающей Kotlin-стороной (все `nativeXxx`-вызовы, трогающие GL, идут через один
   `Handler.post {}` на этот поток).
3. `eglCreateWindowSurface(display, config, aNativeWindow, ...)` — из `platformSurfaceOrCtx`
   (полученного через JNI `ANativeWindow_fromSurface` на Kotlin-стороне, передан как `jlong`).
4. Компиляция всех GLSL-шейдеров из встроенных C-строк (см. B.3) — `glCreateShader`/
   `glShaderSource`/`glCompileShader`, явная проверка `GL_COMPILE_STATUS` + `glGetShaderInfoLog`
   на ошибку (hard fail, не тихий пропуск — аналог iOS's "hard fail if metallib load fails").
5. Аллокация персистентных FBO-текстур (`colorCorrectRGBA`/`vignetteRGBA`/
   `particleCompositeRGBA`, `GL_RGBA8`, `width×height`) + двух encoder-export текстур (Y
   full-size, CbCr half-size).
6. `ParticleSystem` — тот же класс, что и на iOS (200 частиц, seed 42), без изменений.

### B.3 Хранение GLSL-исходников

Нет аналога `.metallib` — GLSL ES компилируется в рантайме. Варианты: (a) сырые `.glsl`-файлы
как Android assets, читаемые через JNI `AAssetManager`, или (b) `#include`-строки, встроенные как
C-строковые константы в `.cpp`, собираемые CMake'ом через `configure_file`/`xxd`-подобный шаг.
**Выбор — (b), встроенные строки**: избегает Asset Manager JNI-обвязки ради шейдеров, которые
всё равно фиксированы на этапе сборки (не runtime-конфигурируемы пользователем), проще и
консистентнее с тем, что C++-код уже владеет шейдерной логикой. `native/src/render/gl/shaders/`
получает `.glsl`-файлы (человекочитаемые, с расширением GLSL для подсветки синтаксиса в IDE) +
маленький CMake custom command, оборачивающий каждый в `constexpr const char* kXxxSource = R"(...)"`
через `.h`-генерацию (аналог того, как `.metallib`-шаг уже устроен, но выход — заголовочный файл,
не бинарник).

---

## Раздел C — Kotlin-архитектура (зеркало iOS Swift-цепочки)

Прямое структурное зеркало уже проверенной iOS-цепочки (`AppDelegate`/`CameraCaptureController`/
`EffectsRenderController`/`RecordingController`/`LumaCoreBridge`) — не редизайн:

- **`LumaCoreBridge.kt`** (`com.lumacore.native.LumaCoreBridge`) — тонкая JNI-обёртка,
  `external fun` на каждую из 8 функций из Раздела A.3, `companion object { init { System.loadLibrary("lumacore") } }`.
- **`CameraCaptureController.kt`** — CameraX `Preview` use case (не `ImageAnalysis` — нужен
  GPU-resident `SurfaceTexture`, не CPU `ImageProxy`), `Preview.SurfaceProvider` указывает на
  внутренний `SurfaceTexture` (созданный на GL-потоке, привязанный к OES-текстуре). Отдельно —
  `AudioRecord` на своём `HandlerThread` (аналог iOS `audioQueue`), стрим PCM-чанков в JNI
  `nativeSubmitAudioFrame`. Permissions: `CAMERA` + `RECORD_AUDIO`, тот же принцип "нет
  video-only фолбэка", что и на iOS.
- **`EffectsRenderController.kt`** — владеет `LumaCoreBridge` + `GLRenderThread`
  (`HandlerThread` + `Handler`), один сеанс рендера на весь жизненный цикл камеры (не
  пересоздаётся на запись) — тот же принцип, что и iOS. `onFrameAvailable()` от camera-input
  `SurfaceTexture` постит `updateTexImage()` + `nativeRenderFrame(session, ptsUs)` на GL-поток.
- **`RecordingController.kt`** — занимает `session` от `EffectsRenderController`, вызывает
  `nativeStartRecording`/`nativeStopRecording`, сохраняет через `MediaStore`
  (`MediaStore.Video.Media`, аналог Photos.app-сохранения на iOS, add-only — не запрашивает
  полный доступ к галерее).
- **`MainActivity.kt`** — компонующий класс (заменяет текущий недоделанный стаб), тот же
  `MethodChannel`-контракт 1:1 с iOS `AppDelegate.swift` (те же имена методов/аргументов —
  `getDeviceFingerprint`, `validateLicense`, `setRecordingSettings`, `startCamera`, `stopCamera`,
  `startRecording`, `stopRecording`, `forceThermalStateForTesting`) — Dart-сторона уже полностью
  платформо-независима, менять `lib/` не нужно вообще.

`startCamera`'s `sessionId`/`textureId` в ответе Platform Channel: `textureId` — от
`textures.createSurfaceTexture()` (`TextureRegistry.SurfaceTextureEntry.id()`), `sessionId` — от
`nativeRenderInit` (тот же паттерн, что и iOS, просто источники `textureId` разные по
push/pull-модели).

---

## Раздел D — Gradle/CMake интеграция + разрешения

### D.1 `android/app/build.gradle.kts`

```kotlin
android {
  ...
  defaultConfig {
    ...
    ndk { abiFilters += "arm64-v8a" }
    externalNativeBuild {
      cmake {
        arguments += "-DANDROID_STL=c++_shared"
      }
    }
  }
  externalNativeBuild {
    cmake {
      path = file("../../native/CMakeLists.txt")  // ARCHITECTURE.md §5: второй CMakeLists не создаём
      version = "3.22.1"
    }
  }
}
```
Второй `CMakeLists.txt` под `android/` **не создаётся** — Gradle указывает прямо на
`native/CMakeLists.txt` (уже зафиксировано в ARCHITECTURE.md §5, только не реализовано).

### D.2 `AndroidManifest.xml`

```xml
<uses-permission android:name="android.permission.CAMERA" />
<uses-permission android:name="android.permission.RECORD_AUDIO" />
<uses-feature android:name="android.hardware.camera" android:required="true" />
```
(Запись видео на `MediaStore` через `scoped storage` — `WRITE_EXTERNAL_STORAGE` не нужен на
API 29+, `minSdk=24` — нужно проверить fallback на API 24-28 отдельно, но не блокирует основной
путь на эмуляторе API 34.)

---

## Порядок выполнения (рекомендация — чекпойнт после первого среза)

1. **A.1** (EncoderSession platform-agnostic buffer) + **A.2** (FFmpeg Android cross-compile) —
   можно параллельно, независимо от GL-работы.
2. **D.1/D.2** (Gradle/CMake/Manifest) — разблокирует саму возможность собрать `.so` — первая
   headless-проверка («GLRenderBackend-стаб линкуется через Gradle»).
3. **Раздел B** (GL-бэкенд + GLSL-шейдеры) — самая объёмная часть, порт по одному шейдеру,
   проверка компиляции на эмуляторе на каждом шаге.
4. **A.3** (JNI-мост, 6 недостающих функций).
5. **Раздел C** (Kotlin-цепочка) — CameraX, потоки, MethodChannel.
6. **Чекпойнт**: `flutter run` на эмуляторе (Pixel 3a API 34 AVD, виртуальная камера) — живое
   превью с эффектами, без записи. **Рекомендую остановиться здесь и свериться**, прежде чем
   продолжать на запись/аудио — само по себе это уже большой объём (сопоставим с целым Этапом 4
   на iOS), и подтверждение, что базовый пайплайн реально работает на Android до вложения
   дальнейших часов в запись/аудио/лицензирование, снижает риск.
7. (Следующая итерация) — запись через `nativeStartRecording`/MediaCodec, аудио через
   `AudioRecord`, лицензирование (JNI уже готов из A.4, UI не меняется — Dart уже общий).

## Верификация

**Headless (до эмулятора):**
1. `cmake --build` host-only — не должен сломаться (Android-специфичный код у CMake не участвует
   в host-only ветке `if(ANDROID)`, только `lumacore_logic`, которая уже платформо-независима).
2. Новый `native/tests/`-тест для `NativeNV12Buffer`-пути (синтетический буфер →
   `EncoderSession::submitFrame` → `ffprobe`, тот же паттерн, что и AV-mux тест из iOS-плана,
   но без CoreVideo — чистый malloc'нутый буфер).
3. Gradle `./gradlew :app:externalNativeBuildDebug` (или полный `assembleDebug`) — первая
   реальная проверка, что CMake+NDK-тулчейн собирает `GLRenderBackend.cpp`+JNI+шейдерные
   строки без ошибок компиляции GLSL-**C++-обёртки** (сами GLSL-шейдеры проверяются только на
   эмуляторе, см. риск в контексте выше).

**Эмулятор:**
4. `flutter run -d emulator-5554` (Pixel 3a AVD) — `adb logcat` на `GL_INVALID_*`/
   `E/AndroidRuntime` при старте камеры — первая реальная проверка компиляции GLSL-шейдеров.
5. Визуально (через `adb exec-out screencap` в скрипте, как я делал для iOS-симулятора) —
   подтвердить, что превью показывает хоть какое-то изображение (виртуальная камера AVD выдаёт
   тестовый паттерн/сцену, этого достаточно для проверки, что GPU-пайплайн реально пишет в
   Flutter-текстуру, не для реалистичной проверки эффектов).

**Физическое устройство** — вне охвата этого раунда (нет Android-девайса), эмулятор — основной
способ проверки.

## Критические файлы

- `native/src/encode/EncoderSession.h/.cpp` — платформенная ветка для `NativeNV12Buffer`.
- `tools/ffmpeg/common.sh`, `tools/ffmpeg/build_android.sh` — FFmpeg Android cross-compile.
- `native/jni/lumacore_jni.cpp` — 6 недостающих JNI-функций.
- `native/src/render/gl/GLRenderBackend.h/.cpp`, `native/src/render/gl/shaders/*.glsl` (новые) —
  основной объём работы.
- `native/CMakeLists.txt` — Android-ветка: FFmpeg-линковка, шейдер-строки custom command.
- `android/app/build.gradle.kts` — `externalNativeBuild` → `native/CMakeLists.txt`.
- `android/app/src/main/AndroidManifest.xml` — CAMERA/RECORD_AUDIO permissions.
- Новые Kotlin: `LumaCoreBridge.kt`, `CameraCaptureController.kt`, `EffectsRenderController.kt`,
  `RecordingController.kt`, замена `MainActivity.kt`.

## Осознанные решения, зафиксированные в этом плане

- `EncoderSession` получает платформенную ветку, а не полный рефакторинг контракта — минимизирует
  риск регрессии уже работающего iOS-пути.
- MediaCodec — CPU NV12 буфер через `avcodec_send_frame` (как VideoToolbox на iOS), не
  Surface-based zero-copy — безопасный, уже проверенный на iOS путь; zero-copy — отдельный
  fast-follow.
- GLSL-исходники — встроенные C-строки, не Android assets.
- Только `arm64-v8a` ABI (как iOS — только arm64, без x86_64-симулятора/armeabi-v7a).
- План специально останавливается после «живое превью с эффектами на эмуляторе» — запись/аудио/
  лицензирование на Android — следующая итерация после проверки базового пайплайна.
