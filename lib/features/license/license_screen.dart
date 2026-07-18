import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:go_router/go_router.dart';
import 'package:http/http.dart' as http;

import '../../core/channels/native_channel.dart';
import '../../core/license/license_config.dart';

class LicenseScreen extends StatefulWidget {
  const LicenseScreen({super.key});

  @override
  State<LicenseScreen> createState() => _LicenseScreenState();
}

class _LicenseScreenState extends State<LicenseScreen> {
  final _keyController = TextEditingController();
  final _storage = const FlutterSecureStorage();
  bool _isLoading = false;
  String? _error;

  @override
  void dispose() {
    _keyController.dispose();
    super.dispose();
  }

  Future<void> _activate() async {
    setState(() {
      _isLoading = true;
      _error = null;
    });
    try {
      final fingerprint = await NativeChannel.getDeviceFingerprint();
      final response = await http.post(
        Uri.parse('$licenseServerBaseUrl/activate'),
        headers: {'Content-Type': 'application/json'},
        body: jsonEncode({'deviceFingerprint': fingerprint, 'licenseKey': _keyController.text.trim()}),
      );
      if (response.statusCode != 200) {
        throw Exception('Server rejected the key (${response.statusCode})');
      }

      final tokenJson = response.body;
      final status = await NativeChannel.validateLicense(tokenJson, fingerprint);
      if (status != 0) {
        throw Exception('Offline validation failed (status=$status)');
      }

      await _storage.write(key: licenseTokenStorageKey, value: tokenJson);
      if (!mounted) return;
      context.go('/camera');
    } catch (e) {
      if (!mounted) return;
      setState(() => _error = 'Activation failed: $e');
    } finally {
      if (mounted) setState(() => _isLoading = false);
    }
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
                  enabled: !_isLoading,
                ),
                if (_error != null) ...[
                  const SizedBox(height: 12),
                  Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
                ],
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: FilledButton(
                    onPressed: _isLoading ? null : _activate,
                    child: _isLoading
                        ? const SizedBox(
                            width: 20,
                            height: 20,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Text('Activate'),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
