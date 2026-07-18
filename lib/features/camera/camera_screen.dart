import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../core/channels/native_channel.dart';
import '../../core/ffi/lumacore_bindings.dart';
import '../../core/settings/recording_settings.dart';
import 'effects_controller.dart';

const int _kEffectColorCorrection = 0x1;
const int _kEffectVignette = 0x2;
const int _kEffectParticles = 0x4;
const int _kEffectSepia = 0x8;
const int _kEffectEdges = 0x10;
// Sepia/Edges are opt-in — off by default, unlike the base trio.
const int _kEffectMaskDefault =
    _kEffectColorCorrection | _kEffectVignette | _kEffectParticles;

class CameraScreen extends ConsumerStatefulWidget {
  const CameraScreen({super.key});

  @override
  ConsumerState<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends ConsumerState<CameraScreen> {
  CameraStartResult? _camera;
  String? _error;
  bool _isRecording = false;

  EffectsController? _effects;
  LumaStatsDart? _stats;

  double _brightness = 0.0;
  double _contrast = 1.0;
  double _saturation = 1.0;
  double _vignetteRadius = 0.75;
  double _vignetteSoftness = 0.3;
  double _particleIntensity = 0.5;
  double _sepiaAmount = 0.6;
  double _edgeThreshold = 0.3;
  double _edgeIntensity = 0.6;
  int _effectMask = _kEffectMaskDefault;

  @override
  void initState() {
    super.initState();
    _startCamera();
  }

  Future<void> _startCamera() async {
    try {
      final settings = ref.read(recordingSettingsProvider);
      await NativeChannel.setRecordingSettings(
        bitrateKbps: settings.bitrateKbps,
        resolutionPreset: settings.resolutionPreset.wireValue,
      );
      final camera = await NativeChannel.startCamera();
      if (!mounted) return;
      setState(() {
        _camera = camera;
        _error = null;
      });
      final effects = EffectsController(camera.sessionId);
      effects.startStatsPolling((stats) {
        if (!mounted) return;
        setState(() => _stats = stats);
      });
      _effects = effects;
      _pushEffectParams();
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = 'Camera unavailable: $e';
      });
    }
  }

  void _pushEffectParams() {
    _effects?.updateParams(
      LumaEffectParamsDart(
        brightness: _brightness,
        contrast: _contrast,
        saturation: _saturation,
        vignetteRadius: _vignetteRadius,
        vignetteSoftness: _vignetteSoftness,
        particleIntensity: _particleIntensity,
        effectMask: _effectMask,
        sepiaAmount: _sepiaAmount,
        edgeThreshold: _edgeThreshold,
        edgeIntensity: _edgeIntensity,
      ),
    );
  }

  Future<void> _toggleRecording() async {
    if (_isRecording) {
      try {
        await NativeChannel.stopRecording();
      } catch (e) {
        if (!mounted) return;
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('Failed to stop recording: $e')));
      } finally {
        if (mounted) {
          setState(() {
            _isRecording = false;
          });
        }
      }
      return;
    }

    try {
      await NativeChannel.startRecording();
      if (!mounted) return;
      setState(() {
        _isRecording = true;
      });
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Failed to start recording: $e')));
    }
  }

  void _showEffectsSheet() {
    showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setSheetState) {
            void update(VoidCallback change) {
              setSheetState(change);
              setState(() {});
              _pushEffectParams();
            }

            return SafeArea(
              child: ConstrainedBox(
                constraints: BoxConstraints(
                  maxHeight: MediaQuery.of(context).size.height * 0.8,
                ),
                child: SingleChildScrollView(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 20,
                    vertical: 12,
                  ),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        'Effects',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                      _EffectSlider(
                        label: 'Brightness',
                        value: _brightness,
                        min: -1,
                        max: 1,
                        onChanged: (v) => update(() => _brightness = v),
                      ),
                      _EffectSlider(
                        label: 'Contrast',
                        value: _contrast,
                        min: 0,
                        max: 2,
                        onChanged: (v) => update(() => _contrast = v),
                      ),
                      _EffectSlider(
                        label: 'Saturation',
                        value: _saturation,
                        min: 0,
                        max: 2,
                        onChanged: (v) => update(() => _saturation = v),
                      ),
                      _EffectSlider(
                        label: 'Vignette radius',
                        value: _vignetteRadius,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _vignetteRadius = v),
                      ),
                      _EffectSlider(
                        label: 'Vignette softness',
                        value: _vignetteSoftness,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _vignetteSoftness = v),
                      ),
                      _EffectSlider(
                        label: 'Particle intensity',
                        value: _particleIntensity,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _particleIntensity = v),
                      ),
                      _EffectSlider(
                        label: 'Sepia amount',
                        value: _sepiaAmount,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _sepiaAmount = v),
                      ),
                      _EffectSlider(
                        label: 'Edge threshold',
                        value: _edgeThreshold,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _edgeThreshold = v),
                      ),
                      _EffectSlider(
                        label: 'Edge intensity',
                        value: _edgeIntensity,
                        min: 0,
                        max: 1,
                        onChanged: (v) => update(() => _edgeIntensity = v),
                      ),
                      const Divider(),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: const Text('Color correction'),
                        value: _effectMask & _kEffectColorCorrection != 0,
                        onChanged:
                            (on) => update(
                              () =>
                                  _effectMask =
                                      on
                                          ? _effectMask |
                                              _kEffectColorCorrection
                                          : _effectMask &
                                              ~_kEffectColorCorrection,
                            ),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: const Text('Vignette'),
                        value: _effectMask & _kEffectVignette != 0,
                        onChanged:
                            (on) => update(
                              () =>
                                  _effectMask =
                                      on
                                          ? _effectMask | _kEffectVignette
                                          : _effectMask & ~_kEffectVignette,
                            ),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: const Text('Particles'),
                        value: _effectMask & _kEffectParticles != 0,
                        onChanged:
                            (on) => update(
                              () =>
                                  _effectMask =
                                      on
                                          ? _effectMask | _kEffectParticles
                                          : _effectMask & ~_kEffectParticles,
                            ),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: const Text('Sepia'),
                        subtitle: const Text(
                          'Pure B&W: set Saturation to 0 above',
                        ),
                        value: _effectMask & _kEffectSepia != 0,
                        onChanged:
                            (on) => update(
                              () =>
                                  _effectMask =
                                      on
                                          ? _effectMask | _kEffectSepia
                                          : _effectMask & ~_kEffectSepia,
                            ),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: const Text('Edges (comic)'),
                        value: _effectMask & _kEffectEdges != 0,
                        onChanged:
                            (on) => update(
                              () =>
                                  _effectMask =
                                      on
                                          ? _effectMask | _kEffectEdges
                                          : _effectMask & ~_kEffectEdges,
                            ),
                      ),
                    ],
                  ),
                ),
              ),
            );
          },
        );
      },
    );
  }

  Future<void> _forceThermalCritical() async {
    if (!kDebugMode || _camera == null) return;
    await NativeChannel.forceThermalStateForTesting(3);
  }

  @override
  void dispose() {
    _effects?.dispose();
    // Fire-and-forget: dispose() can't await, and there's nothing meaningful
    // to do with a stopCamera/stopRecording failure once the screen is gone.
    if (_isRecording) {
      NativeChannel.stopRecording().catchError((_) {});
    }
    NativeChannel.stopCamera().catchError((_) {});
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Expanded(
          child: Container(
            width: double.infinity,
            color: Colors.black,
            alignment: Alignment.center,
            child: Stack(
              fit: StackFit.expand,
              children: [
                Center(child: _buildPreview()),
                if (_stats != null)
                  Positioned(
                    top: 8,
                    left: 8,
                    child: GestureDetector(
                      onLongPress: _forceThermalCritical,
                      child: _DebugStatsOverlay(stats: _stats!),
                    ),
                  ),
              ],
            ),
          ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 16),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                FloatingActionButton(
                  heroTag: 'effects',
                  onPressed: _camera == null ? null : _showEffectsSheet,
                  tooltip: 'Effects',
                  child: const Icon(Icons.tune),
                ),
                const SizedBox(width: 24),
                FloatingActionButton.large(
                  heroTag: 'record',
                  onPressed: _camera == null ? null : _toggleRecording,
                  tooltip: _isRecording ? 'Stop Recording' : 'Start Recording',
                  backgroundColor: _isRecording ? Colors.white : Colors.red,
                  child: Icon(
                    _isRecording ? Icons.stop : Icons.fiber_manual_record,
                    color: _isRecording ? Colors.red : null,
                  ),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildPreview() {
    if (_error != null) {
      return Padding(
        padding: const EdgeInsets.all(24),
        child: Text(
          _error!,
          textAlign: TextAlign.center,
          style: const TextStyle(color: Colors.white70),
        ),
      );
    }
    final camera = _camera;
    if (camera == null) {
      return const CircularProgressIndicator(color: Colors.white70);
    }
    // GPU-processed preview (iOS pull-model, ARCHITECTURE.md §3): the native
    // side runs every frame through the 3-pass Metal pipeline
    // (color correction -> vignette -> particles) before it ever reaches
    // this texture — see ai_plans/03-ios-metal-render-pipeline.md.
    // AspectRatio prevents Texture() from stretching the buffer to fill the
    // container — Texture always sizes to its parent's full constraints.
    return AspectRatio(
      aspectRatio: camera.width / camera.height,
      child: Texture(textureId: camera.textureId),
    );
  }
}

class _EffectSlider extends StatelessWidget {
  const _EffectSlider({
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
  });

  final String label;
  final double value;
  final double min;
  final double max;
  final ValueChanged<double> onChanged;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        SizedBox(width: 130, child: Text(label)),
        Expanded(
          child: Slider(
            value: value,
            min: min,
            max: max,
            // onChanged (not onChangeEnd) — this is what demonstrates why
            // this call goes over dart:ffi rather than a Platform Channel:
            // it fires at tens of Hz while dragging.
            onChanged: onChanged,
          ),
        ),
      ],
    );
  }
}

class _DebugStatsOverlay extends StatelessWidget {
  const _DebugStatsOverlay({required this.stats});

  final LumaStatsDart stats;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: Colors.black54,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Text(
        'fps: ${stats.fps.toStringAsFixed(1)}  '
        'frame: ${stats.avgFrameMs.toStringAsFixed(1)}ms\n'
        'dropped: ${stats.droppedFrames}  thermal: ${stats.thermalState}',
        style: const TextStyle(
          color: Colors.white,
          fontSize: 11,
          fontFamily: 'monospace',
        ),
      ),
    );
  }
}
