import 'package:flutter/material.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:go_router/go_router.dart';

import '../../core/channels/native_channel.dart';
import '../../core/license/license_config.dart';

/// Offline re-validation gate: if a previously-activated token is already on
/// disk, revalidate it against the current device fingerprint (no network —
/// see lumacore_validate_license) and skip straight to /camera, so the user
/// doesn't have to re-enter their license key on every app launch.
class SplashScreen extends StatefulWidget {
  const SplashScreen({super.key});

  @override
  State<SplashScreen> createState() => _SplashScreenState();
}

class _SplashScreenState extends State<SplashScreen> {
  @override
  void initState() {
    super.initState();
    _checkExistingLicense();
  }

  Future<void> _checkExistingLicense() async {
    const storage = FlutterSecureStorage();
    final token = await storage.read(key: licenseTokenStorageKey);
    if (token == null) {
      if (mounted) context.go('/license');
      return;
    }
    final fingerprint = await NativeChannel.getDeviceFingerprint();
    final status = await NativeChannel.validateLicense(token, fingerprint);
    if (!mounted) return;
    context.go(status == 0 ? '/camera' : '/license');
  }

  @override
  Widget build(BuildContext context) {
    return const Scaffold(body: Center(child: CircularProgressIndicator()));
  }
}
