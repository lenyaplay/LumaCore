import 'package:flutter_riverpod/flutter_riverpod.dart';

enum RecordingResolutionPreset { hd720, hd1080, uhd4k }

extension RecordingResolutionPresetLabel on RecordingResolutionPreset {
  String get label => switch (this) {
    RecordingResolutionPreset.hd720 => '720p',
    RecordingResolutionPreset.hd1080 => '1080p',
    RecordingResolutionPreset.uhd4k => '4K',
  };

  /// Matches the case values CameraCaptureController.applyPendingPreset
  /// switches on natively.
  String get wireValue => switch (this) {
    RecordingResolutionPreset.hd720 => 'hd720',
    RecordingResolutionPreset.hd1080 => 'hd1080',
    RecordingResolutionPreset.uhd4k => 'uhd4k',
  };
}

class RecordingSettings {
  const RecordingSettings({required this.bitrateKbps, required this.resolutionPreset});

  final int bitrateKbps;
  final RecordingResolutionPreset resolutionPreset;

  RecordingSettings copyWith({int? bitrateKbps, RecordingResolutionPreset? resolutionPreset}) =>
      RecordingSettings(
        bitrateKbps: bitrateKbps ?? this.bitrateKbps,
        resolutionPreset: resolutionPreset ?? this.resolutionPreset,
      );
}

class RecordingSettingsNotifier extends StateNotifier<RecordingSettings> {
  RecordingSettingsNotifier()
    : super(
        const RecordingSettings(
          bitrateKbps: 6000,
          resolutionPreset: RecordingResolutionPreset.hd1080,
        ),
      );

  void setBitrate(int kbps) => state = state.copyWith(bitrateKbps: kbps);
  void setResolution(RecordingResolutionPreset preset) =>
      state = state.copyWith(resolutionPreset: preset);
}

final recordingSettingsProvider =
    StateNotifierProvider<RecordingSettingsNotifier, RecordingSettings>(
      (ref) => RecordingSettingsNotifier(),
    );
