# LumaCore

### 🎬 [Демо-видео](media/demo-comic-effect.mp4)

Запись с реального устройства (Android, MediaTek/Mali): живое превью с GPU-эффектом
«комикс» (Sobel edge-detection + постеризация в одном фрагментном шейдере — разбор
математики в [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#как-устроен-эффект-комикса)) и
запись видео со звуком в MP4 (H.264 + AAC) через FFmpeg/MediaCodec.

Приложение камеры с real-time GPU-фильтрами и записью видео — Flutter UI поверх общего
C++ ядра (FFmpeg, GPU-рендеринг, лицензирование) на Android, iOS и Windows. Инженерные
решения и риски — [ARCHITECTURE.md](docs/ARCHITECTURE.md), практический гайд «как собрать
и запустить» — [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

**Статус**: Android- и iOS-пайплайны (захват камеры, 3-проходный GPU-рендер, запись
со звуком, офлайн-лицензирование) реализованы и проверены на реальных устройствах —
не только собираются, но и подтверждённо работают (см. `docs/ai_plans/04-05` про
конкретные баги, найденные и исправленные при отладке на устройстве). Windows
(Vulkan-бэкенд) — не начат.

## Ключевые технические возможности

| Возможность | Реализация в LumaCore |
|---|---|
| Захват/обработка/рендеринг видео в реальном времени | Camera preview → C++ ядро → GPU-шейдер → экран, native-to-native, без Dart VM на кадр |
| Нативные плагины для Flutter (камера, медиафайлы) | Platform Channel + JNI (Android), Platform Channel + Obj-C++ (iOS), Platform Channel + C++ (Windows) |
| Процедурная анимация / GPU-эффекты | 3-проходный шейдерный пайплайн: цветокоррекция → виньетка → частицы |
| FFmpeg, кодеки, контейнеры, буферизация | libavcodec/libavformat, MP4-мукс, drop-oldest кольцевой буфер render→encode |
| OpenGL ES / Vulkan / Metal | OpenGL ES 3.0 (Android), Metal (iOS), Vulkan (Windows) — общий C++ `IRenderBackend` |
| Лицензирование с привязкой к device ID | Ed25519-подписанный токен, офлайн-валидация в C++, device fingerprint на платформу |
| Многопоточность | Отдельные потоки capture/render/encode, lock-free-ориентированный ring buffer |
| Оптимизация под мобильные ограничения | Термо-адаптивная деградация пайплайна (см. ARCHITECTURE.md §1) |
| CI/CD | GitHub Actions — `.github/workflows/ci.yml` |

## Структура репозитория

```
native/     общее C++17-ядро (CMake): render/encode/license, платформенные бэкенды
tools/ffmpeg/  скрипты сборки минимального FFmpeg под каждую платформу
android/ ios/ windows/   тонкий platform-слой (сгенерирован flutter create + MethodChannel-глю)
lib/        Flutter UI: go_router + Riverpod, экраны license/camera/gallery/settings
server/     mock-бэкенд лицензий — FastAPI + Ed25519 (PyNaCl)
docs/       архитектурная схема, профилирование, демо (наполняется на Этапе 9)
```

## Что уже работает

- **`native/`** — C++17/CMake. `lumacore_logic` (RingBuffer, RenderPipeline-оркестрация,
  EffectParams, TokenValidator, публичный C API) собирается и тестируется на
  голом хосте без Android/iOS/Windows-тулчейнов:
  ```
  cmake -S native -B native/build && cmake --build native/build
  ctest --test-dir native/build --output-on-failure
  ```
  Платформенные GPU-бэкенды — `GLRenderBackend` (Android, OpenGL ES 3.0) и
  `MetalRenderBackend` (iOS) реализованы полностью и проверены на устройстве
  (захват камеры → 3-проходный шейдер → превью + запись). `VulkanRenderBackend`
  (Windows) — заголовок + заглушка, Этап 10.

- **`android/` + `ios/`** — CameraX/GL (Android) и AVFoundation/Metal (iOS) пайплайны:
  живое превью с эффектами (цветокоррекция, сепия, комикс/edge-detection, виньетка,
  частицы), запись видео+звука в MP4 через FFmpeg (`h264_mediacodec`/`h264_videotoolbox`
  + AAC), офлайн-валидация лицензии. Как собрать/запустить — [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

- **`lib/`** — Flutter UI: license → camera → gallery → settings
  (Riverpod + go_router). Требует [FVM](https://fvm.app) с Flutter 3.38.10
  (см. `.fvmrc`):
  ```
  fvm flutter pub get
  fvm flutter analyze
  fvm flutter test
  ```

- **`server/`** — mock-бэкенд лицензий, реальная Ed25519-подпись/верификация:
  ```
  cd server && ./run.sh
  ```
  (см. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) про подключение с физического устройства).

## Roadmap

Windows (Vulkan-бэкенд, Media Foundation, D3D11↔Vulkan interop с Flutter) — следующий
этап после Android/iOS; известные риски — [ARCHITECTURE.md §8](docs/ARCHITECTURE.md#8-риски-честно-без-приукрашивания).
