import 'package:flutter/material.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return CustomScrollView(
      slivers: [
        const SliverAppBar.large(title: Text('Settings')),
        SliverList.list(
          children: const [
            ListTile(
              title: Text('Bitrate'),
              subtitle: Text('TODO Этап 5 — recording bitrate picker'),
              enabled: false,
            ),
            ListTile(
              title: Text('Resolution'),
              subtitle: Text('TODO Этап 5 — recording resolution picker'),
              enabled: false,
            ),
          ],
        ),
      ],
    );
  }
}
