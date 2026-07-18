# iOS: минимальная сквозная запись MP4 (пере-секвенированные Этап 3 + часть Этапа 5)

## Контекст

Этап 2 (нативный захват камеры) полностью готов и проверен на устройстве: `CameraCaptureController`
→ `CameraPreviewTexture` → `Texture()` виджет во Flutter — живой превью с камеры работает.

Дальше по Task.md §6 идёт Этап 3 (C++ ядро + сборка FFmpeg) → Этап 4 (GPU/Metal) → Этап 5
(запись/экспорт). Пользователь явно расставил приоритет: не эксхаустивная глубина по каждому этапу,
а **минимальная работающая демка**. Сам ARCHITECTURE.md (риск-регистр §8, риск №4) советует то же
самое: "один корректно замукшированный MP4" должен быть отдельной вехой, доказанной **до** начала
работы над GPU-шейдерами. Поэтому эта задача сознательно пересекает границы этапов и берёт только
вертикальный срез: **сырой кадр с камеры → FFmpeg (VideoToolbox H.264) → MP4 → Photos** — без Metal,
без аудио, без Android/Windows.

Полный аудит репозитория подтвердил: `native/` (CMake, C++ API `lumacore_api.h`, `EncoderSession`)
существует только как заглушки, не подключён к Xcode вообще (`project.pbxproj` не содержит ни одной
ссылки на `native/`, `.mm`, `.cpp`, xcframework). `tools/ffmpeg/build_ios.sh` — стаб с `exit 1`.
FFmpeg-флаги для write-path (без libx264/GPL) уже прописаны в `tools/ffmpeg/common.sh`, но без
iOS-специфичных `--enable-videotoolbox --enable-encoder=h264_videotoolbox`.

**Из скоупа исключено (сознательно отложено, не проектируется сейчас):** Metal/GPU-рендеринг,
аудио-захват, Android/Windows кодирование, лицензирование, thermal-деградация.

## Контрольная точка после сборки FFmpeg (по решению пользователя)

Кросс-компиляция FFmpeg под iOS — риск №1 в ARCHITECTURE.md §8, потенциально многодневная задача.
Пользователь выбрал: **остановиться и отчитаться после шагов 1–2 верификации** (FFmpeg собрался,
`h264_videotoolbox` энкодер присутствует и открывается в отдельном standalone-смоук-тесте вне Xcode)
— и только после подтверждения продолжать интеграцию в CMake/Xcode/Swift. Это первая веха работы,
не финальная.

## Дизайн

### 1. FFmpeg-флаги — `tools/ffmpeg/common.sh` / `tools/ffmpeg/build_ios.sh`

Добавить iOS-специфичный набор флагов (общие уже есть в `common.sh`):
```bash
FFMPEG_IOS_FLAGS=(
  --enable-videotoolbox
  --enable-encoder=h264_videotoolbox
  --extra-ldflags="-framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework AudioToolbox"
)
```
`--enable-videotoolbox` сам по себе не включает энкодер под `--disable-everything` — нужен явный
`--enable-encoder=h264_videotoolbox`. Декод/hwaccel не трогаем (write-path only, как и раньше).

Переписать `tools/ffmpeg/build_ios.sh`: две среза — `iphoneos/arm64` (устройство) и
`iphonesimulator/arm64` (симулятор Apple Silicon), через `xcrun --sdk ... -f clang` +
`configure --target-os=darwin --arch=arm64 --enable-cross-compile --sysroot=...`, вывод в
`native/third_party/ffmpeg_build/ios/{device-arm64,sim-arm64}/` (уже gitignored).
`MIN_IOS_VERSION=13.0` — совпадает с `IPHONEOS_DEPLOYMENT_TARGET` в Xcode-проекте.

### 2. Контрольная точка: standalone smoke-test (до Xcode/CMake)

`nm -g libavcodec.a | grep -i videotoolbox` — символы должны присутствовать.
Затем 15-строчный C-файл, скомпилированный `clang` напрямую против собранных `.a` (без Xcode,
таргет — simulator SDK, чтобы запускался локально), проверяющий
`avcodec_find_encoder_by_name("h264_videotoolbox") != NULL`.

**Стоп здесь, отчёт пользователю, ждать подтверждения перед следующим разделом.**

### 3. CMake интеграция — `native/CMakeLists.txt`, `native/cmake/ios.toolchain.cmake`

Вендорим `leetal/ios-cmake` как один файл `native/cmake/ios.toolchain.cmake` (не submodule —
проще для этого масштаба). Конфигурация раздельно для device/simulator:
```bash
cmake -S native -B native/build-ios-device -DCMAKE_TOOLCHAIN_FILE=native/cmake/ios.toolchain.cmake -DPLATFORM=OS64 -DDEPLOYMENT_TARGET=13.0
cmake -S native -B native/build-ios-sim    -DCMAKE_TOOLCHAIN_FILE=native/cmake/ios.toolchain.cmake -DPLATFORM=SIMULATORARM64 -DDEPLOYMENT_TARGET=13.0
```

В `native/CMakeLists.txt`, внутри существующей ветки `elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")`
(строки 34-40 сейчас): добавить `target_include_directories`/`target_link_libraries` на собранные
FFmpeg-статики + `-framework VideoToolbox -framework CoreMedia -framework AudioToolbox`, и
`target_compile_definitions(lumacore_logic PRIVATE LUMACORE_HAVE_FFMPEG=1)`.

**Важно:** `EncoderSession.cpp` компилируется и в host-only конфигурации (для `lumacore_tests` на
голом CI, без тулчейна — см. комментарий в CMakeLists.txt строки 22-25). Реальную FFmpeg-логику
обернуть в `#ifdef LUMACORE_HAVE_FFMPEG`, оставив текущий стаб (`recording_ = true; return true;`)
как `#else`-ветку — не ломает существующий "скелет собирается везде".

### 4. `EncoderSession` — `native/src/encode/EncoderSession.cpp`

Сигнатуры уже зафиксированы (`native/src/encode/EncoderSession.h`): `start(outPath, bitrateKbps, w, h)`,
`submitFrame(void* platformImageHandle, int64_t ptsUs)`, `stop()`.

- **Формат кадра**: закрепить в `CameraCaptureController.configureSession()` явный
  `output.videoSettings = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]`
  → 1:1 соответствует FFmpeg `AV_PIX_FMT_NV12` (full-range, `AVCOL_RANGE_JPEG`).
- **Путь передачи кадра — CPU-копия (Path B), не zero-copy hw-wrap.** `CVPixelBufferLockBaseAddress`
  → `memcpy` по реальным `bytesPerRowOfPlane` (не считать stride == width) в `AVFrame` с
  `format=AV_PIX_FMT_NV12` → `avcodec_send_frame`/`avcodec_receive_packet` → `av_interleaved_write_frame`.
  `h264_videotoolbox` штатно принимает software NV12 AVFrame (внутри копирует в свой
  `CVPixelBufferPool`) — это не тот неподтверждённый риск из §8, а стандартное поведение ffmpeg.
  Zero-copy `AV_PIX_FMT_VIDEOTOOLBOX`-wrap сознательно не делаем в этом майлстоуне — тот путь
  реально понадобится позже вместе с Metal (`CVPixelBuffer→CVMetalTexture`), и тогда его есть смысл
  делать один раз, а не дважды.
- `avcodec_find_encoder_by_name("h264_videotoolbox") == NULL` → жёсткий провал (нет software
  H.264-фолбэка, libx264 исключён намеренно) — не глотать молча.
- Синхронно на своей очереди (без `std::thread`/`RingBuffer` — эскалация только если появится
  видимый статтер превью во время записи; `RingBuffer` уже реализован и протестирован на будущее).
- `stop()`: flush (`send_frame(nullptr)`) → `av_write_trailer` (пишет moov atom — обязателен на
  всех путях выхода, включая ошибки) → `avio_closep`/`avcodec_free_context`/`avformat_free_context`.

### 5. C API / Obj-C++ мост

`native/src/api/lumacore_api.h` получает новую функцию — **это единственное отклонение от
формально "зафиксированного" API**, обосновано тем, что итоговая архитектура ожидает кадры на
энкодер только изнутри C++ через `RenderPipeline::exportForEncoder()`, а этот путь появится
только с Metal (Этап 4). Пока его нет — нужен явный вход:
```c
LUMACORE_API void lumacore_submit_frame(int64_t session, void* pixelBuffer, int64_t ptsUs);
```
С комментарием в коде, что это временный passthrough-вход до Metal, дальше либо удаляется, либо
станет explicit "no-effects" режимом записи.

`native/objcxx/LumaCoreBridge.h/.mm` — добавить тонкие методы `startRecording:outputPath:bitrateKbps:width:height:`,
`submitFrame:pixelBuffer:ptsUs:`, `stopRecording:` (прямой форвардинг в C API, без логики).

### 6. Линковка в Xcode — прямой static-link xcframework, без CocoaPods

`libtool -static` мержит `liblumacore.a` + `liblumacore_logic.a` + FFmpeg-статики в один архив на
каждый срез (device/sim) → `xcodebuild -create-xcframework` → `ios/LumaCoreKit.xcframework`
(путь уже зарезервирован в `.gitignore`). Добавить в `Runner.xcodeproj` через "Frameworks, Libraries
and Embedded Content" (Do Not Embed — статическая либа). CocoaPods не вводим — единственная
локальная зависимость без версионирования, не стоит доп. слоя `Podfile`/`Pods.xcodeproj`.

`ios/Runner/Runner-Bridging-Header.h` (сейчас только `GeneratedPluginRegistrant.h`) — добавить
`#import "LumaCoreBridge.h"`, header search path на `native/objcxx`/`native/src/api`.

### 7. Swift — новый `RecordingController.swift` + правки существующих файлов

Новый класс (не раздувать `AppDelegate`): владеет `LumaCoreBridge`, своей serial `encodeQueue`
(критично — `captureQueue` от `CameraCaptureController` не должен блокироваться на энкод),
считает относительный PTS от первого кадра сессии записи.

- `CameraCaptureController.onFrame`: сигнатура меняется на `(CVPixelBuffer, CMTime) -> Void` —
  нужен `CMSampleBufferGetPresentationTimeStamp` (аппаратные часы камеры, не wall-clock).
- `AppDelegate.swift`: в существующий `channel.setMethodCallHandler` switch (сейчас
  `getDeviceFingerprint`/`startCamera`/`stopCamera`) добавить `startRecording`/`stopRecording`.
  Размер кадра берём из уже вычисленного в `handleStartCamera` (сохранить на `AppDelegate`).

### 8. Сохранение файла — Documents + сразу Photos (в рамках этого же майлстоуна)

Пишем в `FileManager...documentDirectory/lumacore_<ts>.mp4`, сразу по завершении успешной записи —
`PHPhotoLibrary.shared().performChanges { PHAssetCreationRequest.forAsset().addResource(.video, ...) }`.
`ios/Runner/Info.plist` — добавить `NSPhotoLibraryAddUsageDescription` (add-only, не полный доступ).

### 9. Flutter — `lib/core/channels/native_channel.dart`, `lib/features/camera/camera_screen.dart`

`NativeChannel.startRecording() -> Future<String>` / `stopRecording() -> Future<void>`, тем же
паттерном что `startCamera`/`stopCamera`. В `_CameraScreenState` — `bool _isRecording`, подключить
существующую `FloatingActionButton.large` (сейчас `onPressed: null`, `TODO(Этап 5)` на строке 63-64)
к `_toggleRecording()`, смена иконки/цвета в записи, `stopRecording()` fire-and-forget в `dispose()`
если запись была активна (по аналогии с текущим `stopCamera()`).

## Верификация (пошагово, каждый шаг изолирует один класс ошибок)

1. FFmpeg собирается для device-среза, `nm` показывает `h264_videotoolbox` в `libavcodec.a`.
2. Standalone `clang`-смоук-тест вне Xcode: `avcodec_find_encoder_by_name` не NULL.
   **→ Стоп, отчёт, ждать подтверждения (см. раздел "Контрольная точка").**
3. `cmake --build` для iOS device и simulator тулчейнов проходит без ошибок линковки.
4. xcframework собирается, линкуется в Runner, приложение на устройстве успешно вызывает
   уже существующие стабы `lumacore_render_init`/`lumacore_release` без крэша (проверяем линковку
   отдельно от логики энкодера).
5. `EncoderSession` проверяется на синтетических кадрах (сплошной цвет, `CVPixelBufferCreate` +
   `memset`, ~90 кадров по фейковому PTS 30fps) через временный debug-путь — снятый файл проверить
   `ffprobe`/QuickTime на playable + корректную длительность. Изолирует "энкодер работает" от
   "AVFoundation-плагинг работает".
6. Живая запись с камеры: кнопка записи → реальный MP4 → `ffprobe`/QuickTime (играется?) →
   визуальная проверка (не зелёный/битый кадр — частая ошибка с NV12 stride) → длительность
   соответствует времени удержания кнопки.
7. Photos-сохранение: файл появляется в приложении Фото после записи, permission-prompt с текстом
   из `NSPhotoLibraryAddUsageDescription` показывается при первом сохранении.

## Критические файлы

- `native/src/api/lumacore_api.h`, `native/src/api/lumacore_api.cpp`
- `native/src/encode/EncoderSession.h`, `native/src/encode/EncoderSession.cpp`
- `native/CMakeLists.txt`, `native/cmake/ios.toolchain.cmake` (новый)
- `tools/ffmpeg/common.sh`, `tools/ffmpeg/build_ios.sh`
- `native/objcxx/LumaCoreBridge.h`, `native/objcxx/LumaCoreBridge.mm`
- `ios/Runner/CameraCaptureController.swift`
- `ios/Runner/RecordingController.swift` (новый)
- `ios/Runner/AppDelegate.swift`
- `ios/Runner/Info.plist`
- `ios/Runner.xcodeproj/project.pbxproj`
- `ios/Runner/Runner-Bridging-Header.h`
- `lib/core/channels/native_channel.dart`
- `lib/features/camera/camera_screen.dart`

---

1
