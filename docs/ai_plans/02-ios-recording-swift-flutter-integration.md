# Завершение iOS-интеграции записи (Swift/Flutter/Photos + верификация)

## Контекст

Это прямое продолжение `docs/ai_plans/01-ios-ffmpeg-minimal-recording.md`. Пользователь уже
подтвердил переход от контрольной точки (FFmpeg собран и проверен smoke-тестом) к разделам 3–9.
В текущей сессии уже сделано и подтверждено рабочим сборками:

- FFmpeg завендорен как submodule (`n7.1.5`), собран под `device-arm64`/`sim-arm64`
  (`tools/ffmpeg/build_ios.sh`, `tools/ffmpeg/common.sh`).
- `native/cmake/ios.toolchain.cmake` (leetal/ios-cmake 4.5.0) завендорен, `native/CMakeLists.txt`
  линкует FFmpeg-статики и фреймворки для `PLATFORM=OS64`/`SIMULATORARM64`
  (`LUMACORE_HAVE_FFMPEG=1`). `cmake --build` проходит без ошибок для device, sim и host-only
  (стаб) конфигураций — проверено.
- `EncoderSession.{h,cpp}` — реальная реализация под `#ifdef LUMACORE_HAVE_FFMPEG` (pimpl,
  CPU-copy NV12 из `CVPixelBuffer` с честным `bytesPerRowOfPlane`, hard-fail без
  `h264_videotoolbox`, flush + `av_write_trailer` в `stop()`), стаб сохранён в `#else`.
- `lumacore_api.h/.cpp` — добавлен `lumacore_submit_frame`. `LumaCoreBridge.h/.mm` — добавлены
  `startRecording:outputPath:bitrateKbps:width:height:`, `submitFrame:pixelBuffer:ptsUs:`,
  `stopRecording:`.
- `ios/LumaCoreKit.xcframework` собран (`libtool -static` мерж `liblumacore` +
  `liblumacore_logic` + FFmpeg-статики per-slice → `xcodebuild -create-xcframework`) и подключён
  в `Runner.xcodeproj` через ruby `xcodeproj` gem: xcframework (link-only, без Embed),
  системные фреймворки `VideoToolbox/CoreMedia/AudioToolbox/CoreVideo/CoreFoundation` +
  `libiconv.tbd`/`libz.tbd` (реально используемые символы проверены через `nm -u`), и
  `HEADER_SEARCH_PATHS` на `native/objcxx` и `native/src/api`. `Runner-Bridging-Header.h`
  импортирует `LumaCoreBridge.h`.
- `CameraCaptureController.swift`: `output.videoSettings` закреплён на
  `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`; `onFrame` теперь
  `(CVPixelBuffer, CMTime) -> Void`, `CMSampleBufferGetPresentationTimeStamp` передаётся дальше.
- `ios/Runner/RecordingController.swift` — новый файл создан (владеет `LumaCoreBridge`, своей
  serial `encodeQueue`, считает relative PTS от первого кадра, делает Documents→Photos на
  успешном `stop()`).
- `AppDelegate.swift` — частично: объявлены `recordingController`, `frameSize`, метод-хендлер
  переключает `"startRecording"`/`"stopRecording"` на ещё не написанные методы, `onFrame`
  форвардит кадры и в `recordingController.submitFrame`. **Не хватает**: сама реализация
  `handleStartRecording`/`handleStopRecording`, и `frameSize` фактически нигде не присваивается
  (баг из середины правки).

Что осталось до полной сквозной демки согласно исходному плану (разделы 7–9 + верификация 3–7).

## Оставшаяся работа

### 1. Xcode-проект: добавить `RecordingController.swift` в target

Файл создан на диске, но не добавлен как member `Runner`-таргета (`grep RecordingController
project.pbxproj` → 0 совпадений). Сделать тем же способом, что и xcframework/фреймворки —
через ruby `xcodeproj` gem (уже использовался в этой сессии, безопаснее ручной правки pbxproj):
добавить `PBXFileReference` в группу `Runner`, добавить в `Sources` build phase таргета `Runner`.

### 2. `AppDelegate.swift` — доделать recording-хендлеры

- Исправить баг: в `handleStartCamera` в `.success(let frameSize)` добавить
  `self.frameSize = frameSize` (сейчас нигде не присваивается).
- `RecordingController.start(width:height:completion:)` сейчас возвращает
  `Result<Void, Error>` — поменять на `Result<URL, Error>` (start уже знает `outputURL`),
  чтобы `handleStartRecording` мог вернуть путь во Flutter как `Future<String>` (см. план §9:
  `NativeChannel.startRecording() -> Future<String>`).
- `handleStartRecording(result:)`: guard `frameSize` присутствует (иначе `FlutterError`,
  камера не запущена) → `recordingController.start(width:height:) { result in
  DispatchQueue.main.async { ... } }` → на успех `result(url.path)`, на ошибку `FlutterError`.
- `handleStopRecording(result:)`: `recordingController.stop { result in
  DispatchQueue.main.async { ... } }` → на успех `result(nil)` (Photos-сохранение уже
  произошло внутри `stop()` до колбэка), на ошибку `FlutterError`.

### 3. Photos permission — `Info.plist`

Добавить `NSPhotoLibraryAddUsageDescription` (add-only, не полный доступ) рядом с уже
существующим `NSCameraUsageDescription`.

### 4. Flutter — `native_channel.dart` + `camera_screen.dart`

- `NativeChannel`: добавить `static Future<String> startRecording()` и
  `static Future<void> stopRecording()`, тем же паттерном что `startCamera`/`stopCamera`.
- `camera_screen.dart`: `bool _isRecording = false`; `_toggleRecording()` вызывает
  `NativeChannel.startRecording()`/`stopRecording()`, обновляет `_isRecording`, ловит ошибки
  в `_error`-подобном виде (snackbar или инлайн, по аналогии с `_startCamera`). Подключить
  `FloatingActionButton.large` (сейчас `onPressed: null`, `TODO(Этап 5)` строка 63-64) к
  `_toggleRecording`, сменить иконку (`Icons.fiber_manual_record` → `Icons.stop`) и, по
  желанию, цвет/анимацию во время записи. В `dispose()` — `NativeChannel.stopRecording()`
  fire-and-forget, только если `_isRecording` было true (по аналогии с текущим
  `stopCamera().catchError`).

### 5. Верификация (шаги 3–7 исходного плана)

- **Шаг 3 (cmake --build device+sim)** — уже пройден в этой сессии, повторной работы не
  требует, зафиксировать в отчёте.
- **Шаг 4 (xcframework линкуется, стабы вызываются без крэша)** — после доделки Xcode-проекта:
  `flutter build ios --debug --simulator --no-codesign` должен пройти чисто (в этой сессии
  уже ловили две Swift-ошибки на промежуточном состоянии кода — они устранятся, когда пункты
  1–2 будут закрыты). Дополнительно — установить и запустить на симуляторе
  (`xcrun simctl install`/`launch`), убедиться, что старт камеры не крашит (сам вызов
  `renderInit`/`release` теперь неотделим от recording-пути — старт/стоп записи и есть эта
  проверка).
- **Шаг 5 (синтетические кадры → ffprobe)** — отдельный standalone-тест, тем же приёмом что и
  smoke-тест FFmpeg-энкодера на чекпоинте: маленький Obj-C++/C++ CLI-бинарник, линкуемый
  `clang`-ом напрямую против `native/build-ios-sim/merged/liblumacore_merged.a` (без Xcode),
  который создаёт `CVPixelBuffer` (`kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`),
  заполняет Y/UV одним цветом через `memset`, гоняет ~90 кадров с PTS по 30fps через
  `lumacore_render_init`→`lumacore_start_recording`→`lumacore_submit_frame`×90→
  `lumacore_stop_recording`→`lumacore_release`, пишет файл во временный каталог, запускается
  через `xcrun simctl spawn` (как и в чекпоинте). Для проверки самого файла нужен `ffprobe` —
  на хосте его нет (`which ffprobe` → not found); ставлю через `brew install ffmpeg`
  (подтверждено пользователем — dev-инструмент только для верификации, не часть репозитория/
  сборки приложения).
- **Шаг 6 (живая запись с камеры) + шаг 7 (Photos)** — симулятор не имеет реальной камеры
  (`AVCaptureDevice.default(.builtInWideAngleCamera...)` вернёт `nil`), значит нужен реальный
  прогон на устройстве. В системе обнаружен подключённый физический iPhone
  (`iPhone (26.5)`, `xcrun xctrace list devices`), и в проекте уже настроен `DEVELOPMENT_TEAM`
  (`XQ9GMC3FJ9`, `CODE_SIGN_STYLE = Automatic`) — похоже, до этого уже разворачивали на
  устройство (git log: "feat: add camera preview for ios"). Подтверждено пользователем:
  автоматизирую через XCUITest вместо ручного участия. План:
  - Добавить временный XCUITest в `RunnerTests`-таргет (или отдельный `RunnerUITests`-таргет,
    если его ещё нет — проверить перед созданием), который: запускает приложение → дожидается
    появления камеры-превью → тапает `FloatingActionButton` записи → ждёт фиксированное время
    (~3 сек) → тапает повторно (стоп) → обрабатывает системный permission-alert для Photos
    (`addUIInterruptionMonitor` / `springboard` XCUIApplication, тапает "Allow"/"Разрешить").
  - Прогнать `xcodebuild test -destination 'id=<device-id>'` на это устройство.
  - Скачать записанный файл с устройства (`xcrun devicectl device copy from` — новее и не
    требует `ideviceinstaller`; либо через доступ к Documents-контейнеру приложения) и
    прогнать `ffprobe` + визуально проверить (не зелёный/битый кадр — частая ошибка NV12
    stride) + сверить длительность с временем ожидания в тесте (~3 сек).
  - Проверить в Фото на устройстве (через `xcrun devicectl` снимок экрана Photos.app, либо
    через сравнение количества ассетов до/после — например `xcrun simctl`-аналог для реальных
    устройств недоступен впрямую, поэтому полагаюсь на сам факт успешного
    `PHPhotoLibrary.performChanges` без ошибки в логах теста + опциональный скриншот
    Photos.app как визуальное подтверждение).
  - Permission-prompt текст (`NSPhotoLibraryAddUsageDescription`) будет виден на скриншоте,
    снятом XCUITest непосредственно перед автоматическим тапом "Allow".
  - После теста — удалить временный XCUITest-файл/таргет (это dev-артефакт для одноразовой
    верификации, не часть репозитория), если пользователь не попросит оставить его как
    постоянный regression-тест.

## Критические файлы

- `ios/Runner/AppDelegate.swift` — доделать recording-хендлеры, поправить `frameSize`
- `ios/Runner/RecordingController.swift` — поправить сигнатуру `start()` на `Result<URL, Error>`
- `ios/Runner.xcodeproj/project.pbxproj` — добавить `RecordingController.swift` в target (через
  ruby `xcodeproj`, не руками)
- `ios/Runner/Info.plist` — `NSPhotoLibraryAddUsageDescription`
- `lib/core/channels/native_channel.dart`, `lib/features/camera/camera_screen.dart`
- Новый временный CLI-файл для шага 5 (в scratchpad, не коммитится в репозиторий)

## Порядок выполнения

1. Xcode target membership для `RecordingController.swift`.
2. `AppDelegate.swift` + `RecordingController.swift` fix.
3. `Info.plist`.
4. Flutter (`native_channel.dart`, `camera_screen.dart`).
5. `flutter build ios --debug --simulator --no-codesign` — чистая сборка (шаг 4 верификации).
6. `brew install ffmpeg` (для `ffprobe`) + standalone synthetic-frame тест на симуляторе
   (шаг 5 верификации).
7. Временный XCUITest на подключённом iPhone: тап записи → ~3 сек → тап стоп → Photos
   permission-alert → `ffprobe`/визуальная проверка результата (шаги 6–7 верификации).
   Удалить временный UI-тест после верификации.
8. Итоговый отчёт пользователю.
