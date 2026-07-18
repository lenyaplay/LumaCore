import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';

class LicenseScreen extends StatefulWidget {
  const LicenseScreen({super.key});

  @override
  State<LicenseScreen> createState() => _LicenseScreenState();
}

class _LicenseScreenState extends State<LicenseScreen> {
  final _keyController = TextEditingController();

  @override
  void dispose() {
    _keyController.dispose();
    super.dispose();
  }

  void _activate() {
    // TODO(Этап 6): POST /activate, persist the signed token, then verify
    // offline via lumacore_validate_license before allowing entry.
    context.go('/camera');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 360),
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Text('LumaCore', style: Theme.of(context).textTheme.headlineMedium),
                const SizedBox(height: 8),
                const Text('Enter your license key to activate.', textAlign: TextAlign.center),
                const SizedBox(height: 24),
                TextField(
                  controller: _keyController,
                  decoration: const InputDecoration(labelText: 'License key', border: OutlineInputBorder()),
                ),
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: FilledButton(onPressed: _activate, child: const Text('Activate')),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
