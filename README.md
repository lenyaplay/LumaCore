# LumaCore

Портфолио-приложение камеры с real-time GPU-фильтрами и записью видео —
Flutter UI поверх общего C++ ядра (FFmpeg, GPU-рендеринг, лицензирование) на
Android, iOS и Windows. Полная мотивация и детальный roadmap — [Task.md](docs/Task.md),
инженерные решения и риски — [ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Статус: скелет репозитория** (Task.md §6, Этап 0–1). Camera capture, GPU-шейдеры,
сборка FFmpeg и офлайн-валидация лицензии — TODO на последующих этапах; сейчас
собирается и проходит тесты только pure-logic слой (см. «Что уже работает» ниже).

## Соответствие требованиям вакансии

| Требование вакансии | Реализация в LumaCore |
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

Полная таблица с пояснениями — Task.md §3.

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
  Платформенные GPU-бэкенды (GL/Metal/Vulkan) — заголовки + заглушки,
  подключаются в сборку только под соответствующим тулчейном (Этап 2–4, 10).

- **`lib/`** — Flutter-скелет с навигацией license → camera → gallery → settings
  (Riverpod + go_router). Требует [FVM](https://fvm.app) с Flutter 3.38.10
  (см. `.fvmrc`):
  ```
  fvm flutter pub get
  fvm flutter analyze
  fvm flutter test
  ```

- **`server/`** — mock-бэкенд лицензий, реальная Ed25519-подпись/верификация:
  ```
  cd server
  python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
  .venv/bin/python -m pytest tests
  ```

## Roadmap

См. Task.md §6 (Этапы 0–10) и риски в ARCHITECTURE.md §8.
