import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../core/settings/recording_settings.dart';

const List<int> _kBitrateOptionsKbps = [4000, 6000, 10000];

class SettingsScreen extends ConsumerWidget {
  const SettingsScreen({super.key});

  Future<void> _pickBitrate(BuildContext context, WidgetRef ref, int current) async {
    final chosen = await showDialog<int>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Bitrate'),
        children: [
          RadioGroup<int>(
            groupValue: current,
            onChanged: (v) => Navigator.of(context).pop(v),
            child: Column(
              children: [
                for (final kbps in _kBitrateOptionsKbps)
                  RadioListTile<int>(title: Text('${kbps ~/ 1000} Mbps'), value: kbps),
              ],
            ),
          ),
        ],
      ),
    );
    if (chosen != null) ref.read(recordingSettingsProvider.notifier).setBitrate(chosen);
  }

  Future<void> _pickResolution(BuildContext context, WidgetRef ref, RecordingResolutionPreset current) async {
    final chosen = await showDialog<RecordingResolutionPreset>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Resolution'),
        children: [
          RadioGroup<RecordingResolutionPreset>(
            groupValue: current,
            onChanged: (v) => Navigator.of(context).pop(v),
            child: Column(
              children: [
                for (final preset in RecordingResolutionPreset.values)
                  RadioListTile<RecordingResolutionPreset>(title: Text(preset.label), value: preset),
              ],
            ),
          ),
        ],
      ),
    );
    if (chosen != null) ref.read(recordingSettingsProvider.notifier).setResolution(chosen);
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final settings = ref.watch(recordingSettingsProvider);
    return CustomScrollView(
      slivers: [
        const SliverAppBar.large(title: Text('Settings')),
        SliverList.list(
          children: [
            ListTile(
              title: const Text('Bitrate'),
              subtitle: Text('${settings.bitrateKbps ~/ 1000} Mbps'),
              onTap: () => _pickBitrate(context, ref, settings.bitrateKbps),
            ),
            ListTile(
              title: const Text('Resolution'),
              subtitle: Text(settings.resolutionPreset.label),
              onTap: () => _pickResolution(context, ref, settings.resolutionPreset),
            ),
          ],
        ),
      ],
    );
  }
}
