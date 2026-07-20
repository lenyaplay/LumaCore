# Этап 4: iOS Metal render pipeline (GPU-эффекты) — полный вертикальный срез

## Контекст

Прямое продолжение `docs/ai_plans/02-ios-recording-swift-flutter-integration.md`. На физическом
iPhone 13 Pro подтверждено рабочим: захват камеры (`CameraCaptureController`), raw NV12-превью
через pull-модель (`CameraPreviewTexture`), запись H.264/MP4 через VideoToolbox с CPU-copy путём
(`EncoderSession`), сохранение в Photos (видео без звука). Сейчас сырой кадр камеры идёт
**напрямую** и в превью, и в энкодер — GPU-шага в Swift-пути нет вообще:
`AppDelegate.swift`'s `onFrame` замыкание делает `previewTexture.updateFrame(pixelBuffer)` +
`recordingController.submitFrame(pixelBuffer, pts:)` на сыром буфере.

Это Этап 4 роадмапа (`ARCHITECTURE.md` §2) — самый рискованный и трудоёмкий этап
(оценка 1–1.5 недели). Пользователь выбрал **полный вертикальный срез**, не облегчённую версию:
реальный 3-проходный Metal-пайплайн (цветокоррекция → виньетка → частицы), подключённый и к
живому превью, и к записи, плюс `dart:ffi` для параметров эффекта (не Platform Channel) с
интерактивными Flutter-слайдерами — строго по `ARCHITECTURE.md` §2/§3, которая явно требует
именно dart:ffi для высокочастотных вызовов (слайдер = десятки Hz, статистика = 2-4Hz).

**Существующие заглушки, используются как есть, не переизлагаются**: `IRenderBackend.h`,
`EffectParams.h` (`EffectParamsBlock`, alignas(16), std140/Metal-constant-buffer-совместимый),
`RenderPipeline.h/.cpp` (стаб — `renderFrame` дропает импортированный кадр), `MetalRenderBackend.h/.mm`
(все методы — заглушки, ни одного `.metal`-файла ещё нет), `lumacore_api.h/.cpp` (не рендерит,
`lumacore_submit_frame` идёт напрямую в энкодер, `set_effect_params`/`get_stats` — no-op),
`EncoderSession` (CPU-copy NV12, рабочий, не трогается в этом плане), `LumaCoreBridge`,
Swift-цепочка (`AppDelegate`/`CameraCaptureController`/`CameraPreviewTexture`/`RecordingController`),
`lib/core/ffi/lumacore_bindings.dart` (пустая заглушка, нет `ffi`/`ffigen` в pubspec.yaml, нет
`ffigen.yaml` нигде в репозитории).

**Ограничения из предыдущих сессий**: iOS-only (Android `GLRenderBackend`/Windows
`VulkanRenderBackend` остаются стабами — не трогаем); правки `.pbxproj` — только через ruby
`xcodeproj` gem, никогда руками; `IPHONEOS_DEPLOYMENT_TARGET = 14.0` (поднят в плане 02, был 13.0)
— MSL `long`/64-бит поддержка (Metal 2.3+) доступна без проблем; физическое устройство — iPhone 13
Pro (`iPhone14,2`, ProMotion/120Hz дисплей, `DEVELOPMENT_TEAM = XQ9GMC3FJ9`,
`CODE_SIGN_STYLE = Automatic`), подключён по USB; `LumaCoreKit.xcframework` собирается CMake
device+sim → `libtool -static` мерж `liblumacore`+`liblumacore_logic`+FFmpeg-статики per-slice →
`xcodebuild -create-xcframework` — **важно: эти шаги не зафиксированы ни в каком скрипте
репозитория**, выполнялись ad-hoc в терминале в прошлых сессиях. Этот план должен наконец
зафиксировать их в `tools/ios/build_xcframework.sh` (см. §4) — иначе новый `.metallib`-шаг
пристроить один раз вручную, но воспроизвести пересборку с нуля будет нечем.

## Ретроспектива плана 02 (важно для §11)

On-device XCUITest-автоматизация в плане 02 упёрлась в блокирующий баг движка Flutter
(`VSyncClient`/`FlutterViewController.viewDidLoad` — `EXC_BAD_ACCESS`, независимо от нашего кода)
и несколько часов ушло на диагностику зависаний `devicectl`/конкурирующих процессов на
устройстве. В итоге пользователь проверял запись вручную через `flutter run` — сработало сразу,
без единого краша. **В этом плане on-device XCUITest не используется вообще** — основной способ
проверки на физическом устройстве это `flutter run` + визуальный осмотр (§11, пп. 6-7); вся
численно проверяемая логика уходит в headless/симуляторные CLI-проверки, которые ловят баги
дёшево и быстро до похода на устройство.

---

## Оставшаяся работа

### 1. Частицы — чистая C++ математика (тестируемая без GPU)

Безстейтовая функция времени: `(индекс частицы, elapsedSeconds) → (x, y, alpha, size)` — не
мутируемая симуляция. Простой, но не игрушечный: реальный seeded PRNG (xorshift32), реальный
лайфтайм-луп, реальная реакция на intensity.

Новый файл `native/src/render/particles/ParticleSystem.h/.cpp`:

```cpp
namespace lumacore::render::particles {

// Статика частицы — считается один раз из seed, не меняется по кадрам.
// Общая для всех бэкендов (ARCHITECTURE.md §2).
struct ParticleSeed {
  float x0, y0;         // нормализованная стартовая позиция [0,1]^2
  float vx, vy;          // дрейф в единицах экрана/сек
  float lifetimeSec;     // 2.0-5.0s, разное на частицу — иначе виден "тик" всех сразу
  float phaseOffset;     // 0..lifetimeSec, сдвигает точку старта во времени
  float size;            // радиус спрайта в UV-единицах
};

// GPU-инстанс-буфер layout (16 bytes) — зеркало ShaderTypes.h::ParticleInstanceGPU (см. §2).
struct alignas(16) ParticleInstance { float x, y, alpha, size; };

class ParticleSystem {
 public:
  ParticleSystem(int count, uint32_t seed);
  int count() const;
  // Чистая функция — детерминирована, без побочных эффектов и блокировок.
  // Вызывается один раз за кадр из MetalRenderBackend::runPass(Particles, ...).
  void computeInstances(float elapsedSeconds, float intensity,
                         std::vector<ParticleInstance>& outInstances) const;

 private:
  std::vector<ParticleSeed> seeds_;
};

}  // namespace lumacore::render::particles
```

`computeInstances`: `age = fmod(elapsedSeconds + seed.phaseOffset, seed.lifetimeSec)`;
`t = age / lifetimeSec`; `x/y = seed.{x0,y0} + seed.{vx,vy} * age`, с wraparound через `fmod(·, 1.0)`
чтобы частица не улетала за кадр навсегда; `alpha = intensity * sin(t * π)` (плавно появляется и
исчезает за лайфтайм, не мигает); `size = seed.size * (0.5 + 0.5*sin(t*π))`. N = 200 (демонстрируемо,
дёшево — 200×16 байт = 3.2KB инстанс-буфер).

`native/CMakeLists.txt`: `src/render/particles/ParticleSystem.cpp` добавляется в `lumacore_logic`
(платформо-независимая цель — переиспользуется всеми бэкендами).

Тест `native/tests/test_particle_system.cpp` (тот же PASS/FAIL-паттерн без фреймворка, что
`test_ring_buffer.cpp`; добавить `add_executable`/`add_test` в `native/tests/CMakeLists.txt`):
- `computeInstances` отдаёт ровно `count()` элементов;
- все `alpha ∈ [0,1]`, все `size > 0`;
- детерминизм: два вызова с одинаковыми аргументами дают идентичный результат;
- `computeInstances(0.0f, ...)` vs `computeInstances(2.0f, ...)` отличаются хотя бы частично
  (время реально влияет на позиции);
- `intensity=0` → `alpha=0` для всех частиц (эффект реально гасится, не просто игнорируется).

### 2. Шейдеры — конкретная структура прохода (не голословно "написать шейдеры")

Ключевое техническое решение, которого нет буквально в `ARCHITECTURE.md`, но необходимое: камера
отдаёт `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` (2 раздельные плоскости, разного размера —
Y полный, CbCr вдвое меньше по каждой оси), а MRT в Metal требует одинаковых размеров attachments,
так что нельзя одним render-pass-ом писать в обе плоскости. Поэтому:

- **Импорт** (внутри прохода `ColorCorrection`) и **финальный вывод обратно в NV12** (внутри
  прохода `Particles`) — единственные места конвертации цветового пространства. Все промежуточные
  проходы работают в простой RGBA float.
- Это не меняет публичный интерфейс: `PassId` остаётся ровно `{ColorCorrection, Vignette, Particles}`,
  как уже зафиксировано в `IRenderBackend.h` — просто `ColorCorrection`/`Particles` внутри
  `MetalRenderBackend::runPass()` каждый делают больше одного GPU render-pass-encoder за один
  C++-вызов. Приватная деталь `.mm`, не видна `RenderPipeline`/интерфейсу.

Новая директория `native/src/render/metal/shaders/`:

**`ShaderTypes.h`** — общий заголовок для MSL и Obj-C++ через `#if __METAL_VERSION__` (стандартный
Apple-паттерн). Байт-в-байт зеркало `EffectParamsBlock` из `EffectParams.h` — **не** `#include`
того заголовка напрямую в `.metal` (тянет `<cstdint>`/`lumacore_api.h`, не MSL-совместимые);
синхронизация вручную, с комментарием-предупреждением в обоих файлах ("добавили поле в
`EffectParams.h` → обновите `ShaderTypes.h`"):

```c
#ifndef LUMACORE_SHADER_TYPES_H
#define LUMACORE_SHADER_TYPES_H

#ifdef __METAL_VERSION__
#include <metal_stdlib>
#else
#include <simd/simd.h>
#endif

struct alignas(16) EffectParamsGPU {
  float brightness, contrast, saturation, _pad0;
  float vignetteRadius, vignetteSoftness, particleIntensity, _pad1;
  long effectMask, _pad2;
};

struct alignas(16) ParticleInstanceGPU { float x, y, alpha, size; };

#endif
```

`effectMask`-биты (новая конвенция — `ARCHITECTURE.md` не задаёт числа явно, фиксируется здесь):
`0x1` = ColorCorrection, `0x2` = Vignette, `0x4` = Particles. Дефолт `0x7` (все включены).

**`Common.metal`** — fullscreen-triangle вершинная функция без вершинного буфера (классический
no-VBO приём через `vertex_id`) + YCbCr⇄RGB конверсии, BT.601 **full-range** матрицы (совпадает с
`kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`):

```metal
#include <metal_stdlib>
#include "ShaderTypes.h"
using namespace metal;

struct VSOut { float4 position [[position]]; float2 uv; };

vertex VSOut fullscreenTriangleVS(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  out.uv = float2(uv.x, 1.0 - uv.y);
  return out;
}

inline float3 ycbcrToRGB(float y, float cb, float cr) {
  cb -= 0.5; cr -= 0.5;
  return float3(y + 1.402*cr, y - 0.344136*cb - 0.714136*cr, y + 1.772*cb);
}
inline float3 rgbToYCbCr(float3 rgb) {
  float yy =  0.299*rgb.r + 0.587*rgb.g + 0.114*rgb.b;
  float cb = -0.168736*rgb.r - 0.331264*rgb.g + 0.5*rgb.b + 0.5;
  float cr =  0.5*rgb.r - 0.418688*rgb.g - 0.081312*rgb.b + 0.5;
  return float3(yy, cb, cr);
}
```

**`ColorCorrection.metal`** — фьюзит YCbCr→RGB импорт с цветокоррекцией (единственный проход, чей
"src" — не одна RGBA-текстура, а пара Y/CbCr-текстур из `importExternalFrame`):

```metal
fragment float4 colorCorrectFS(VSOut in [[stage_in]],
                                texture2d<float> yTex [[texture(0)]],
                                texture2d<float> cbcrTex [[texture(1)]],
                                constant EffectParamsGPU& p [[buffer(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float y = yTex.sample(s, in.uv).r;
  float2 cbcr = cbcrTex.sample(s, in.uv).rg;
  float3 rgb = ycbcrToRGB(y, cbcr.x, cbcr.y);
  if (p.effectMask & 0x1) {
    rgb = (rgb - 0.5) * p.contrast + 0.5 + p.brightness;
    float luma = dot(rgb, float3(0.299, 0.587, 0.114));
    rgb = mix(float3(luma), rgb, p.saturation);
  }
  return float4(saturate(rgb), 1.0);
}
```

**`Vignette.metal`** — радиальная фолл-off, RGBA→RGBA:

```metal
fragment float4 vignetteFS(VSOut in [[stage_in]],
                            texture2d<float> src [[texture(0)]],
                            constant EffectParamsGPU& p [[buffer(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float4 c = src.sample(s, in.uv);
  if (p.effectMask & 0x2) {
    float d = distance(in.uv, float2(0.5));
    float v = smoothstep(p.vignetteRadius, max(p.vignetteRadius - p.vignetteSoftness, 0.0), d);
    c.rgb *= v;
  }
  return c;
}
```

**`Particles.metal`** — базовый блит + инстансированные квады частиц (аддитивный блендинг,
`sourceAlpha, one` — не alpha-over, частицы "светятся") + две финальные функции конвертации в
NV12 (Y-plane полного размера, CbCr-plane при half-size viewport — билинейный даунсемпл на
семпле бесплатный):

```metal
fragment float4 blitFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  return src.sample(s, in.uv);
}

struct ParticleVSOut { float4 position [[position]]; float2 localUV; float alpha; };

vertex ParticleVSOut particleVS(uint vid [[vertex_id]], uint iid [[instance_id]],
                                 constant ParticleInstanceGPU* instances [[buffer(0)]]) {
  ParticleInstanceGPU p = instances[iid];
  float2 corner = float2((vid << 1) & 2, vid & 2);
  float2 uv = corner * 2.0 - 1.0;
  float2 worldPos = p.xy + uv * p.size;
  ParticleVSOut out;
  out.position = float4(worldPos * 2.0 - 1.0, 0.0, 1.0);
  out.localUV = uv;
  out.alpha = p.alpha;
  return out;
}

fragment float4 particleFS(ParticleVSOut in [[stage_in]]) {
  float r = length(in.localUV);
  float falloff = smoothstep(1.0, 0.0, r);
  return float4(1.0, 1.0, 1.0, in.alpha * falloff);
}

fragment float4 nv12YFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  return float4(rgbToYCbCr(src.sample(s, in.uv).rgb).r, 0, 0, 1);
}

fragment float4 nv12CbCrFS(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(coord::normalized, filter::linear);
  float3 ycbcr = rgbToYCbCr(src.sample(s, in.uv).rgb);
  return float4(ycbcr.gb, 0, 1);
}
```

### 3. `MetalRenderBackend.mm` — реальная реализация

Внутреннее представление `TextureHandle` (приватная деталь `.mm`, не меняет публичный `void*`
контракт `IRenderBackend`):

```objc
struct MetalTextureHandle {
  id<MTLTexture> primary;     // RGBA-интермедиат, либо Y-plane (YCbCr/NV12 хэндлы)
  id<MTLTexture> secondary;   // nil, кроме: импорт камеры (CbCr) и NV12-таргет (CbCr)
  bool isYCbCr = false;
  CVPixelBufferRef owningBuffer = nullptr;  // только для pool-хэндлов
};
```

`initialize(RenderContextParams)`:
1. `MTLCreateSystemDefaultDevice()`, `newCommandQueue`.
2. `[device newLibraryWithURL:[[NSBundle mainBundle] URLForResource:@"lumacore" withExtension:@"metallib"] error:&err]`
   — **hard fail** если nil, не runtime-компиляция из строк (см. §4 про упаковку ресурса).
3. `MTLRenderPipelineState` для каждой fragment-функции (`colorCorrectFS`, `vignetteFS`, `blitFS`,
   `particleVS`+`particleFS`, `nv12YFS`, `nv12CbCrFS`), общий `fullscreenTriangleVS` для всех кроме
   частиц.
4. `CVMetalTextureCacheCreate(...)`.
5. Два `CVPixelBufferPoolCreate` (`_previewPool`, `_encoderPool`) — оба
   `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`, `width`/`height` из параметров,
   `kCVPixelBufferPoolMinimumBufferCountKey = 2`. **Раздельные пулы** (не общий) — preview и
   encoder-экспорт имеют независимые жизненные циклы (превью может отставать/опережать запись),
   общий пул создал бы скрытую связь между путями.
6. Персистентные приватные RGBA-интермедиаты (`MTLStorageModePrivate`, `MTLPixelFormatBGRA8Unorm`,
   `width×height`, выделяются один раз здесь, не пересоздаются каждый кадр):
   `_colorCorrectRGBA`, `_vignetteRGBA`, `_particleCompositeRGBA`.
7. `ParticleSystem _particles{200, /*seed=*/42}` — владение здесь, не в `RenderPipeline` (частицы
   принадлежат бэкенду, который их рисует; `RenderPipeline` передаёт только `elapsedSeconds`/
   `intensity`, см. §5).

`importExternalFrame(NativeImageHandle cameraFrame)`: `CVMetalTextureCacheCreateTextureFromImage`
дважды на `(CVPixelBufferRef)cameraFrame` (`planeIndex 0` → `MTLPixelFormatR8Unorm` полного
размера, `planeIndex 1` → `MTLPixelFormatRG8Unorm` половинного размера) →
`new MetalTextureHandle{luma, chroma, true, nullptr}`.

`beginFrame()`: `_currentCommandBuffer = [_commandQueue commandBuffer]`; освобождает хэндлы,
созданные в предыдущем кадре (простой accumulator-список, очищается в начале следующего кадра —
достаточно для этого объёма, не полноценный аллокатор); достаёт буфер из `_previewPool`
(`CVPixelBufferPoolCreatePixelBuffer`) — **если пул исчерпан**, кадр помечается dropped, остаток
вызовов этого кадра no-op (см. §5/§6 про `LumaStats.droppedFrames`).

`runPass(pass, src, dst, params)` — см. §2: ColorCorrection = 1 render-pass-encoder; Vignette = 1;
Particles = 3 (composite базового слоя + инстансированные квады аддитивно поверх → NV12-Y →
NV12-CbCr). Для Particles: `_particles.computeInstances(elapsedSeconds, params.particleIntensity, _instanceScratch)`
→ `[device newBufferWithBytes:_instanceScratch.data() length:... options:MTLResourceStorageModeShared]`
(маленький буфер, пересоздаётся каждый кадр — 200×16 байт, дёшево, не стоит городить pooling).

`endFrame()`: `[_currentCommandBuffer commit]; [_currentCommandBuffer waitUntilCompleted];` —
**синхронный** коммит (риск ниже), возвращает текущий preview-таргет как `TextureHandle`.

`exportForPreview(TextureHandle h)`: `CVPixelBufferRetain(h->owningBuffer)`, возвращает `void*`
(+1, вызывающий обязан `CVPixelBufferRelease`).

`exportForEncoder(TextureHandle h)`: достаёт **свежий** буфер из `_encoderPool`, 2×
`MTLBlitCommandEncoder copyFromTexture:...toTexture:...` (Y→Y, CbCr→CbCr) в отдельном маленьком
`commandBuffer` + `waitUntilCompleted` (платится только когда реально идёт запись, не на каждый
кадр превью), возвращает retained `void*`.

`destroy()`: `CVMetalTextureCacheFlush`, освобождение обоих пулов, обнуление ARC-managed объектов.

**Риск/чекпойнт** (в духе `ARCHITECTURE.md` "не проверено — прототипировать"): синхронный
`waitUntilCompleted` на каждый кадр убирает CPU/GPU parallelism между кадрами. Для простого
3-прохода на разрешении камеры (`.high` preset, ~1080p) на A15 GPU это должно укладываться в
33мс/кадр (30fps) — **не проверено**, первая реальная прогонка на устройстве с
`lumacore_get_stats.avgFrameMs` (§5/§11 п.6) должна это подтвердить или опровергнуть. Если не
укладывается — fast-follow: `addCompletedHandler` + семафор с 2-3 in-flight буферами
(`MBEDynamicBufferCount`-паттерн), не блокирующий заранее. Не реализуется в этом плане —
осознанно отложено до реального замера.

### 4. CMake-шаг сборки `.metallib` + бандлинг в приложение + фиксация xcframework-скрипта

**Компиляция** — новый custom command в iOS-ветке `native/CMakeLists.txt`, per-slice (device/sim
имеют разные Metal IR, не универсальный fat-lib):

```cmake
set(LUMACORE_METAL_SDK $<IF:$<STREQUAL:${PLATFORM},SIMULATORARM64>,iphonesimulator,iphoneos>)
set(LUMACORE_METAL_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders/Common.metal
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders/ColorCorrection.metal
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders/Vignette.metal
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders/Particles.metal
)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib
  COMMAND xcrun -sdk ${LUMACORE_METAL_SDK} metal -c ${LUMACORE_METAL_SOURCES}
          -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/shaders -o ${CMAKE_CURRENT_BINARY_DIR}/lumacore.air
  COMMAND xcrun -sdk ${LUMACORE_METAL_SDK} metallib ${CMAKE_CURRENT_BINARY_DIR}/lumacore.air
          -o ${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib
  DEPENDS ${LUMACORE_METAL_SOURCES}
)
add_custom_target(lumacore_metallib DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lumacore.metallib)
add_dependencies(lumacore lumacore_metallib)
```

Результат: `native/build-ios-device/lumacore.metallib` и `native/build-ios-sim/lumacore.metallib`
— по одному на конфигурацию.

**Бандлинг в приложение**: статическая библиотека не имеет собственного bundle — линковка
`.metallib` в `.a`/`newDefaultLibrary` не работают. Встраивание как C-байт-массив (`xxd -i`)
рассмотрено и отклонено — усложняет CMake ради спорной выгоды над проверенным паттерном "ресурс в
бандле приложения". Выбор: **`.metallib` — ресурс таргета `Runner`**, копируется отдельным Run
Script build phase (не статичный `Copy Bundle Resources`, т.к. нужно выбрать device/sim вариант по
`$PLATFORM_NAME` динамически на каждой сборке), добавляется через ruby `xcodeproj` gem (тем же
способом, что framework/файлы в плане 02):

```ruby
phase = runner_target.new_shell_script_build_phase("Copy LumaCore Metal Library")
phase.shell_script = <<~SH
  set -e
  SRC_DIR="${SRCROOT}/../native/build-ios-device"
  if [ "${PLATFORM_NAME}" = "iphonesimulator" ]; then
    SRC_DIR="${SRCROOT}/../native/build-ios-sim"
  fi
  cp "${SRC_DIR}/lumacore.metallib" "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/lumacore.metallib"
SH
phase.input_paths = ["${SRCROOT}/../native/build-ios-device/lumacore.metallib", "${SRCROOT}/../native/build-ios-sim/lumacore.metallib"]
phase.output_paths = ["${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/lumacore.metallib"]
```

`MetalRenderBackend.mm` всегда ищет ровно `lumacore.metallib` в `[NSBundle mainBundle]` — единый
путь, без `#if TARGET_OS_SIMULATOR`-ветвления в коде бэкенда.

**Фиксация xcframework-пересборки в скрипт** (новое в этом плане — раньше делалось ad-hoc):
`tools/ios/build_xcframework.sh` — записывает шаги, которые до сих пор выполнялись только в
терминале прошлых сессий (CMake device+sim configure/build → `libtool -static` мерж
`liblumacore`+`liblumacore_logic`+FFmpeg-статики per-slice → `xcodebuild -create-xcframework`),
плюс теперь явно требует, чтобы оба `lumacore.metallib` существовали до пересборки
`Runner.xcodeproj` (иначе Run Script phase скопирует пустоту/старый файл). Это разовая инвестиция
в воспроизводимость — без неё каждая будущая пересборка xcframework снова требует помнить
неформализованную последовательность команд.

**Риск/чекпойнт**: ресурс-бандлинг вокруг статических xcframework-ов исторически хрупкий
(`CocoaPods/CocoaPods#10453` — тот же класс проблемы: static lib + bundled resource). Это должно
быть **первой** проверкой перед тем, как углубляться в остальной пайплайн (§11 п.2) — если Run
Script Phase не подхватывает `.metallib`, весь пайплайн ниже бессмысленно проверять раньше этого.

### 5. `RenderPipeline` — реальная оркестрация + собственный, не переиспользованный, счётчик статистики

```cpp
class RenderPipeline {
 public:
  bool initialize(const RenderContextParams&);
  // false = кадр дропнут (пул истощён / backend не готов).
  bool renderFrame(NativeImageHandle cameraFrame, double elapsedSeconds);
  PlatformImageHandle exportForPreview();   // использует закэшированный TextureHandle
  PlatformImageHandle exportForEncoder();
  void setEffectParams(const LumaEffectParams&);
  void setThermalState(int32_t state);      // 0..3, зеркало ProcessInfo.ThermalState
  LumaStats getStats() const;
  void shutdown();

 private:
  std::unique_ptr<IRenderBackend> backend_;
  EffectParamsBlock currentParams_{};
  TextureHandle lastFrame_ = nullptr;
  int32_t thermalState_ = 0;

  // Скользящее окно последних N таймстампов renderFrame() — НЕ через
  // util::RingBuffer<T> (тот класс — mutex-locking producer/consumer очередь
  // с деструктивным pop, без неразрушающей итерации для усреднения; не тот
  // инструмент). renderFrame() уже вызывается строго с одного потока
  // (ARCHITECTURE.md §1 — MetalRenderThread тикает по приходу кадра камеры,
  // один поток на весь путь), так что блокировки тут не нужны вообще —
  // простой fixed-size circular array локально в этом классе:
  static constexpr int kStatsWindow = 32;
  double frameTimestamps_[kStatsWindow] = {};
  int frameTimestampCount_ = 0;
  int frameTimestampHead_ = 0;
  uint32_t droppedFrames_ = 0;
};
```

`renderFrame`: `beginFrame()` → `src = importExternalFrame(cameraFrame)` → если импорт/пул
провалился — `++droppedFrames_`, return `false` без остальных passes → `runPass(ColorCorrection, src, ...)`
→ `runPass(Vignette, ...)` → **термо-лестница, ступень 1** (§10): если `thermalState_ >= 2`
(THROTTLING), эффективный `effectMask` для этого кадра принудительно гасит бит `0x4` (Particles)
перед передачей в `runPass(Particles, ...)` — сам проход всё равно вызывается (нужен для
NV12-конверсии), просто без частиц; `lastFrame_ = endFrame()`; записать текущий `elapsedSeconds`
в `frameTimestamps_` (circular overwrite). `getStats()`: `fps`/`avgFrameMs` считаются из разностей
соседних элементов `frameTimestamps_` по количеству реально накопленных (`frameTimestampCount_`),
`droppedFrames` — прямое чтение счётчика, `thermalState` — прямое чтение поля.

`initialize()` вызывает `backend_->initialize(params)` — без изменений от текущей сигнатуры,
просто теперь реально что-то делает (Metal setup, §3), не только у `RenderPipeline`, но и
пробрасывается наружу как источник success/failure для `lumacore_render_init` (§6).

### 6. `lumacore_api.cpp` — новая сессия, новый API-контракт

**Выбор архитектуры бэкенда**: `lumacore_api.cpp` компилируется в `lumacore_logic`
(платформо-независимая цель, собирается даже на голом хосте без тулчейнов — CI/skeleton-состояние,
`native/tests`). Прямое `#include "render/metal/MetalRenderBackend.h"` + `new MetalRenderBackend()`
там недопустимо. Решение — маленькая platform-select фабрика, часть `lumacore_logic`:

`native/src/render/PlatformRenderBackendFactory.h`:
```cpp
namespace lumacore::render {
std::unique_ptr<IRenderBackend> createPlatformRenderBackend();
}
```

`native/src/render/PlatformRenderBackendFactory.cpp` (добавляется в `lumacore_logic` в CMake):
```cpp
#include "PlatformRenderBackendFactory.h"
#if defined(__APPLE__)
#include "metal/MetalRenderBackend.h"
#elif defined(__ANDROID__)
#include "gl/GLRenderBackend.h"
#elif defined(_WIN32)
#include "vulkan/VulkanRenderBackend.h"
#endif

namespace lumacore::render {
std::unique_ptr<IRenderBackend> createPlatformRenderBackend() {
#if defined(__APPLE__)
  return std::make_unique<metal::MetalRenderBackend>();
#elif defined(__ANDROID__)
  return std::make_unique<gl::GLRenderBackend>();
#elif defined(_WIN32)
  return std::make_unique<vulkan::VulkanRenderBackend>();
#else
  return nullptr;  // host-only skeleton сборка (CI, тесты без устройства)
#endif
}
}
```

Работает без нарушения host-only skeleton сборки: `MetalRenderBackend.h` — чистый C++-заголовок
(без ObjC-синтаксиса в сигнатурах), инстанцирование его типа компилируется нормально везде;
Metal-*вызовы* живут только в `.mm`, который линкуется в iOS-таргет `lumacore`, а `libtool -static`
мерж уже сегодня комбинирует `liblumacore`+`liblumacore_logic`+FFmpeg в одну статику — символы
`MetalRenderBackend::*` резолвятся на этапе финальной сборки приложения, тем же путём, каким уже
резолвятся FFmpeg-символы. На голом хосте (`lumacore_tests`) ветка `#else` не тянет никаких
Apple/Android/Windows заголовков — компилируется и линкуется чисто.

**`Session`**:
```cpp
struct Session {
  lumacore::render::RenderPipeline pipeline{lumacore::render::createPlatformRenderBackend()};
  lumacore::encode::EncoderSession encoder;
  double startTime = 0;  // monotonic, для elapsedSeconds частиц
};
```

**Новый публичный C API** (осознанное расширение таблицы `ARCHITECTURE.md` §3 — тот же класс
временного-становящегося-постоянным API, что уже случился с `lumacore_submit_frame` в Этапе 3/5;
текущий комментарий в `lumacore_api.h` сам предсказывал его удаление — "see ai_plans/01 §5"):

```c
// Теперь реально initialize()-ит RenderPipeline; -1 при неудаче backend->initialize()
// (раньше был безусловно успешен — небольшое, но осознанное ужесточение).
int64_t lumacore_render_init(void* platformSurfaceOrCtx, int width, int height);
void    lumacore_set_effect_params(int64_t session, const LumaEffectParams* params);
void    lumacore_get_stats(int64_t session, LumaStats* outStats);
void    lumacore_set_thermal_state(int64_t session, int32_t state);  // НОВОЕ, см. §10
int32_t lumacore_start_recording(int64_t session, const char* outPath, int bitrateKbps, int w, int h);
// НОВОЕ — заменяет lumacore_submit_frame. Прогоняет cameraFrame через весь
// 3-проходный пайплайн, отдаёт preview-кадр через outPreviewImage (+1
// retained CVPixelBufferRef, вызывающий обязан освободить), и, если сейчас
// идёт запись, сам форвардит exportForEncoder()-результат в
// EncoderSession::submitFrame внутри одного вызова — Swift делает ОДИН
// нативный вызов на кадр камеры, не два. 0 = ok, -1 = кадр дропнут
// (термо/пул) или сессия не готова.
int32_t lumacore_render_frame(int64_t session, void* cameraFrame, int64_t ptsUs, void** outPreviewImage);
int32_t lumacore_stop_recording(int64_t session);
void    lumacore_release(int64_t session);
```

`lumacore_render_init`: `g_sessions.emplace(id, Session{})` (конструктор уже создаёт backend через
фабрику) → `it->second.pipeline.initialize({platformSurfaceOrCtx, width, height})` → если `false`,
`g_sessions.erase(id)`, вернуть `-1`.

`lumacore_submit_frame` — **удаляется** из публичного API целиком.

Все exported-функции получают `__attribute__((visibility("default"))) __attribute__((used))` под
`__APPLE__` — новый макрос рядом с существующим `LUMACORE_API` в `lumacore_api.h`, критично для
`DynamicLibrary.process()` на iOS (§9 — без этого линкер выбрасывает символы, на которые ссылается
только Dart-код, при статической линковке в бинарь).

### 7. `LumaCoreBridge.h/.mm` — минимальные добавления

- `renderInitWithContext:width:height:` — без изменений сигнатуры.
- **Новый**: `- (nullable CVPixelBufferRef)renderFrame:(int64_t)session pixelBuffer:(CVPixelBufferRef)pixelBuffer ptsUs:(int64_t)ptsUs NS_RETURNS_RETAINED;`
  — единственный per-frame метод, вызывает `lumacore_render_frame`, оборачивает `void*` через
  `(__bridge_transfer CVPixelBufferRef)outBuf` (передача владения из C++ в ARC-мир Obj-C++;
  `NS_RETURNS_RETAINED` обязателен — иначе ARC считает возврат +0 по умолчанию и заавторелизит
  буфер раньше времени).
- **Новый**: `- (void)setThermalState:(int64_t)session state:(int32_t)state;` → `lumacore_set_thermal_state`.
- **Effect params/stats НЕ добавляются в бридж вообще** — dart:ffi вызывает
  `lumacore_set_effect_params`/`lumacore_get_stats` напрямую по C ABI, минуя Obj-C++ (прямое
  следствие `ARCHITECTURE.md` §3), бридж становится меньше, а не больше.
- `submitFrame:pixelBuffer:ptsUs:` — **удаляется** (см. §8 про рефакторинг `RecordingController`).
  `startRecording:.../stopRecording:` — без изменений.

### 8. Swift — новый `EffectsRenderController`, рефакторинг `RecordingController`/`AppDelegate`

**Владение сессией**: **ОДНА** рендер-сессия на весь жизненный цикл камеры (создаётся в
`startCamera`, живёт до `stopCamera`), НЕ пересоздаётся на каждый `startRecording`. Причины: (а)
Metal-инициализация (device/pipeline states/metallib load/пулы) — дорогая one-time операция; (б)
превью должно рендериться с эффектами даже когда запись не идёт — если сессия создаётся только
`startRecording`, превью до первой записи остаётся неотрендеренным сырым кадром, что противоречит
самой цели Этапа 4. `RecordingController` теряет владение `LumaCoreBridge`/`session` — переезжают
в новый класс.

**Новый файл `ios/Runner/EffectsRenderController.swift`**:
```swift
final class EffectsRenderController {
  private let bridge = LumaCoreBridge()
  private(set) var session: Int64 = -1
  private var startTime: CFAbsoluteTime = 0

  func start(width: Int, height: Int) -> Int64 {
    session = bridge.renderInit(withContext: nil, width: Int32(width), height: Int32(height))
    startTime = CFAbsoluteTimeGetCurrent()
    observeThermalState()
    return session
  }

  /// Call from CameraCaptureController.onFrame. Прогоняет кадр через весь
  /// пайплайн И (если идёт запись) сам форвардит в энкодер внутри C++ —
  /// см. lumacore_render_frame. Возвращает отрендеренный кадр для превью.
  func renderFrame(_ pixelBuffer: CVPixelBuffer, pts: CMTime) -> CVPixelBuffer? {
    guard session != -1 else { return nil }
    let ptsUs = Int64((CMTimeGetSeconds(pts) * 1_000_000).rounded())
    return bridge.renderFrame(session, pixelBuffer: pixelBuffer, ptsUs: ptsUs)
  }

  private func observeThermalState() {
    NotificationCenter.default.addObserver(
      forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil
    ) { [weak self] _ in
      guard let self, self.session != -1 else { return }
      self.bridge.setThermalState(self.session, state: Int32(ProcessInfo.processInfo.thermalState.rawValue))
    }
    bridge.setThermalState(session, state: Int32(ProcessInfo.processInfo.thermalState.rawValue))
  }

  func stop() {
    NotificationCenter.default.removeObserver(self, name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
    if session != -1 { bridge.release(session); session = -1 }
  }
}
```

`ProcessInfo.ThermalState.rawValue` совпадает 1:1 с конвенцией `LumaStats.thermalState`
(`nominal=0, fair=1, serious=2, critical=3`) — без ручного маппинга.

**`RecordingController.swift`** — упрощается: убрать `private let bridge`, `private var session`;
`start(session:width:height:completion:)` теперь принимает `session: Int64` параметром (не создаёт
сам, не вызывает `renderInit`); `submitFrame(_:pts:)` **удаляется целиком** (рендер+сабмит теперь
внутри `lumacore_render_frame`, вызываемого из `EffectsRenderController`, не из
`RecordingController`); `stop()` **больше не вызывает** `bridge.release(session)` (сессия не его,
живёт в `EffectsRenderController`), только `bridge.stopRecording(session)`.

**`AppDelegate.swift`**:
- Добавить `private let effectsController = EffectsRenderController()`.
- `handleStartCamera`: после `self.frameSize = frameSize`, вызвать
  `let session = effectsController.start(width:..., height:...)`, добавить
  `"sessionId": session` в возвращаемый Dart-словарь.
- `onFrame`-замыкание переписывается:
  ```swift
  cameraController.onFrame = { [weak effectsController, weak previewTexture] pixelBuffer, pts in
    guard let rendered = effectsController?.renderFrame(pixelBuffer, pts: pts) else { return }
    previewTexture?.updateFrame(rendered)
    // recordingController больше НЕ вызывается здесь — запись уже
    // произошла внутри lumacore_render_frame, если она активна.
  }
  ```
- `handleStartRecording`: `recordingController.start(session: effectsController.session, width:..., height:...) { ... }`.
- `stopCamera`-ветка обработчика метод-канала: добавить `effectsController.stop()` после
  `cameraController.stop()`.

### 9. dart:ffi — реальная проводка

**`pubspec.yaml`**: `dependencies: ffi: ^2.1.3`; `dev_dependencies: ffigen: ^15.0.0` (версии
сверить на pub.dev перед `flutter pub get` — могли обновиться).

**`ffigen.yaml`** (новый файл, корень репозитория):
```yaml
name: LumaCoreBindings
description: ffigen bindings for native/src/api/lumacore_api.h
output: 'lib/core/ffi/lumacore_bindings_generated.dart'
headers:
  entry-points:
    - 'native/src/api/lumacore_api.h'
functions:
  include:
    - 'lumacore_set_effect_params'
    - 'lumacore_get_stats'
structs:
  include:
    - 'LumaEffectParams'
    - 'LumaStats'
```
(`lumacore_render_init`/`render_frame`/`start_recording`/`stop_recording`/`release`/
`set_thermal_state` намеренно НЕ включаются в генерируемые биндинги — это Obj-C++/Swift-only,
per-frame и session-lifecycle логика не идёт по dart:ffi ни при каких условиях, `ARCHITECTURE.md` §3.)

**iOS-специфика символов**: каждая `LUMACORE_API`-функция несёт
`__attribute__((visibility("default"))) __attribute__((used))` под `__APPLE__` (§6). Плюс в Xcode
`Runner`-таргете: `Build Settings → Strip Style → Non-Global Symbols` (через `xcodeproj` gem,
`build_settings['STRIP_STYLE'] = 'non-global'`, не руками) — иначе линкер вычищает символы, на
которые ссылается только Dart-код при статической линковке в бинарь.

**`lib/core/ffi/lumacore_bindings.dart`** — переписывается из заглушки в тонкую обёртку над
сгенерированным `lumacore_bindings_generated.dart`:
```dart
class LumaCoreBindings {
  LumaCoreBindings._() : _bindings = LumaCoreBindingsGenerated(DynamicLibrary.process());
  static final instance = LumaCoreBindings._();
  final LumaCoreBindingsGenerated _bindings;

  void setEffectParams(int sessionId, LumaEffectParamsDart params) { /* маршалит в LumaEffectParams struct */ }
  LumaStatsDart getStats(int sessionId) { /* ... */ }
}
```
(`DynamicLibrary.process()` — статически слинкована в бинарь на iOS, не отдельный `.dylib`. Проект
iOS-only сейчас — `Platform.isAndroid ? DynamicLibrary.open(...) : DynamicLibrary.process()`
сознательно не добавляется в этом плане, решается когда доходит очередь до Android.)

**Передача `sessionId` от Swift к Dart** — единственный реальный "шов" между Platform Channel и
dart:ffi: `NativeChannel.startCamera()` (Platform Channel, редкий вызов) возвращает не только
`textureId/width/height`, но и `sessionId` — тот int64, который C++ уже присвоил внутри
`lumacore_render_init` при старте камеры (§8). `CameraStartResult`
(`lib/core/channels/native_channel.dart`) получает поле `final int sessionId`. Dart дальше держит
`sessionId` как обычное состояние экрана и передаёт его в каждый
`lumacore_set_effect_params(sessionId, ...)`/`lumacore_get_stats(sessionId, ...)` вызов напрямую по
FFI — Dart **никогда** не создаёт/не освобождает сессию сама, только использует id, чей жизненный
цикл целиком контролируется Swift-стороной. После `stopCamera()` Dart обязана перестать вызывать
FFI с этим id (см. §10 UI — контроллер эффектов уничтожается вместе с `dispose()`).

Новый Dart-класс `lib/features/camera/effects_controller.dart` (feature-специфичный, не в `ffi/` —
содержит throttling-логику UI, не голый биндинг):
```dart
class EffectsController {
  EffectsController(this.sessionId);
  final int sessionId;
  Timer? _statsTimer;

  void updateParams(LumaEffectParamsDart params) => LumaCoreBindings.instance.setEffectParams(sessionId, params);
  void startStatsPolling(void Function(LumaStatsDart) onStats) {
    _statsTimer = Timer.periodic(const Duration(milliseconds: 300), (_) {  // 2-4Hz из ARCHITECTURE.md §3
      onStats(LumaCoreBindings.instance.getStats(sessionId));
    });
  }
  void dispose() => _statsTimer?.cancel();
}
```

### 10. UI — `camera_screen.dart`

Компактный нижний bottom sheet (не отдельный экран), поднимается по кнопке-иконке рядом с
record-FAB:
- 6 слайдеров: `brightness [-1,1]`, `contrast [0,2]`, `saturation [0,2]`, `vignetteRadius [0,1]`,
  `vignetteSoftness [0,1]`, `particleIntensity [0,1]`.
- 3 переключателя (`Switch`), маппящихся на биты `effectMask` (§2): ColorCorrection/Vignette/Particles.
- Debug-оверлей (маленький текст поверх превью, в углу) — `fps`/`avgFrameMs`/`droppedFrames`/
  `thermalState` из `LumaStats`, через `EffectsController.startStatsPolling`.
- Каждое изменение слайдера (`onChanged`, не `onChangeEnd`) вызывает
  `effectsController.updateParams(...)` — буквально реализует "десятки Hz при движении слайдера"
  из `ARCHITECTURE.md` §3 (это и есть демонстрация того, зачем нужен dart:ffi, а не Platform
  Channel, для этого конкретного вызова).
- `_CameraScreenState` получает `EffectsController? _effects`, создаётся в `_startCamera()` из
  `camera.sessionId`, `dispose()`-ится вместе со `stopCamera()`.
- Комментарий-заглушка "Raw camera passthrough... GPU shader pipeline lands in Этап 4" —
  заменяется на актуальное описание.
- Долгий тап (long-press) на debug-оверлее в debug-сборке — форсирует
  `lumacore_set_thermal_state(sessionId, 3)` напрямую (без реального перегрева устройства), см. §11 п.7.

### 11. Верификация

Основной способ проверки на устройстве — `flutter run` + визуальный осмотр (см. ретроспективу
плана 02 выше). On-device XCUITest не используется. Численно проверяемая логика — headless/
симуляторные CLI-проверки, до похода на устройство.

**Headless / без устройства:**
1. `cmake --build` host-only (skeleton, без toolchain) — компилируется чисто, `lumacore_tests`
   линкуется и проходит, включая новый `test_particle_system.cpp` (§1). Ловит регрессию в
   платформо-независимой части до какого-либо iOS-специфичного шага.
2. `cmake --build` device (`OS64`) + sim (`SIMULATORARM64`) — компилируется чисто, включая новый
   `.metallib` custom command (§4). Первая проверка, что Metal-компилятор
   (`xcrun -sdk ... metal`) вообще принимает шейдеры без синтаксических ошибок.
3. **Синтетический кадр → пиксельная проверка эффекта** (аналог ffprobe-теста из плана 02, но
   проверяет сам эффект, не контейнер): отдельный CLI-бинарник (в scratchpad, не коммитится, по
   образцу шага 5 из плана 02), линкуемый против собранной статики + `lumacore_render_frame`:
   - создаёт синтетический `CVPixelBuffer` (NV12, сплошной серый — известный Y/CbCr) через `memset`;
   - `lumacore_render_init` → `lumacore_set_effect_params` с экстремальными значениями
     (`brightness=+0.5`, `vignetteRadius=0.3`) → `lumacore_render_frame` (без записи, только превью);
   - читает `outPreviewImage` через `CVPixelBufferLockBaseAddress`, сравнивает Y в **центре**
     (должно быть заметно ярче исходного из-за brightness) и в **углу** (темнее из-за vignette) —
     assert на конкретные числовые пороги, не просто "не упало";
   - отдельный прогон с `effectMask=0` (все эффекты выключены) — проверяет, что YCbCr round-trip
     (импорт→RGB→экспорт NV12) сам по себе не искажает картинку сверх погрешности округления
     (`|ΔY| < 3` на 8-битной шкале) — ловит баги в BT.601-матрицах отдельно от эффектов;
   - запускается через `xcrun simctl spawn` на симуляторе (симулятор Metal — программный, но
     API-поверхность та же, компиляция/линковка/базовая логика проверяемы).
4. `test_particle_system.cpp` — детерминизм, границы, реакция на `intensity=0` — headless, без GPU.

**Симулятор, но не headless:**
5. `flutter build ios --debug --simulator --no-codesign` — чистая сборка Runner с новым
   `EffectsRenderController`/dart:ffi-кодом; `xcrun simctl install/launch` — камеры на симуляторе
   нет, но проверяет, что весь новый Swift/Dart/FFI слой не крашит при старте (символы
   резолвятся, `DynamicLibrary.process()` не бросает `ArgumentError` — чекпойнт из §9 про
   `__attribute__((used))`/Strip Style, здесь либо ловится, либо нет).

**Только физическое устройство (честно ручная/визуальная проверка):**
6. `flutter run` на iPhone 13 Pro — основной способ проверки этапа. Глазами:
   - живое превью реально показывает эффект (не сырой кадр) — сдвинуть slider brightness/contrast,
     увидеть немедленный отклик (демонстрирует dart:ffi-путь, задержка должна быть незаметной);
   - частицы визуально анимируются во времени;
   - toggle-переключатели реально включают/выключают отдельные эффекты;
   - debug-оверлей показывает разумный fps и `avgFrameMs` — первая реальная проверка риска
     синхронного `waitUntilCompleted` (§3);
   - запись видео с активными эффектами → Photos → само видео визуально содержит применённые
     эффекты (не сырой кадр) — открыть в Photos.app на устройстве.
7. **Термо-лестница** (§5/§10) — стретч/ручная проверка. Реальный термотроттлинг трудно вызвать
   намеренно и ненадёжно для демо. Решение: debug-only long-press на оверлее форсирует
   `thermalState=CRITICAL` напрямую (§10) — даёт воспроизводимую проверку глазами, что частицы
   реально гаснут при деградации, плюс управляемый кадр для будущего демо-видео. Настоящий,
   органически термотриггернутый прогон остаётся неавтоматизируемым и опциональным.

---

## Критические файлы

- `native/src/render/metal/MetalRenderBackend.mm` — вся реальная Metal-логика.
- `native/src/render/metal/shaders/{Common,ColorCorrection,Vignette,Particles}.metal`, `ShaderTypes.h`.
- `native/src/render/particles/ParticleSystem.h/.cpp` — частицы, чистая C++, тестируемая.
- `native/src/api/lumacore_api.h/.cpp`, `native/src/render/PlatformRenderBackendFactory.{h,cpp}` —
  новый API-контракт (`lumacore_render_frame`), фабрика бэкенда.
- `native/src/render/RenderPipeline.h/.cpp` — оркестрация, собственный fixed-size stats-буфер.
- `native/CMakeLists.txt` — `.metallib` custom command + новые исходники в `lumacore_logic`.
- `tools/ios/build_xcframework.sh` (новый) — фиксирует ранее ad-hoc шаги пересборки xcframework.
- `ios/Runner/EffectsRenderController.swift` (новый), `ios/Runner/AppDelegate.swift`,
  `ios/Runner/RecordingController.swift` — рефакторинг владения сессией.
- `native/objcxx/LumaCoreBridge.h/.mm` — новые методы `renderFrame:`/`setThermalState:`.
- `lib/core/ffi/lumacore_bindings.dart`, `ffigen.yaml`, `pubspec.yaml` — реальный dart:ffi.
- `lib/features/camera/camera_screen.dart`, `lib/features/camera/effects_controller.dart` (новый),
  `lib/core/channels/native_channel.dart` — UI + sessionId handoff.
- `ios/Runner.xcodeproj/project.pbxproj` — через `xcodeproj` gem: новый Swift-файл в таргет, Run
  Script build phase для `.metallib`, Strip Style.

## Порядок выполнения

1. Частицы (native, headless-тестируемо сразу) — §1.
2. Шейдеры + CMake `.metallib` custom command — §2, §4 (первая проверка: компилируется ли Metal-компилятором).
3. `MetalRenderBackend.mm` реальная реализация — §3.
4. `RenderPipeline` — §5.
5. `lumacore_api.cpp`/`.h` новый контракт + фабрика бэкенда — §6.
6. `LumaCoreBridge` — §7.
7. `tools/ios/build_xcframework.sh` + пересборка xcframework; Xcode через `xcodeproj` gem: новый
   Swift-файл в target, Run Script phase для `.metallib`, Strip Style.
8. Headless/симуляторные проверки (§11 пп.1-5) — до похода на устройство, ловят максимум багов дёшево.
9. Swift-рефакторинг (`EffectsRenderController`/`AppDelegate`/`RecordingController`) — §8.
10. dart:ffi (`pubspec.yaml`, `ffigen.yaml`, `ffigen` run, `lumacore_bindings.dart`) — §9.
11. UI (`camera_screen.dart`, `effects_controller.dart`, `native_channel.dart`) — §10.
12. `flutter run` на физическом устройстве, ручная проверка (§11 пп.6-7) + запись демо-фрагмента
    для будущего README-видео.
13. Итоговый отчёт пользователю.
