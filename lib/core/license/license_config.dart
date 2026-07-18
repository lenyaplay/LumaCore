/// Mock license backend base URL — a physical device can't reach `localhost`
/// (that's the phone's own loopback), so this is a build-time override, not
/// a hardcoded value. Simulator: default 127.0.0.1 works. Physical device:
/// pass the dev Mac's LAN IP, e.g.
///   flutter run --dart-define=LICENSE_SERVER_URL=http://192.168.1.23:8000
const String licenseServerBaseUrl = String.fromEnvironment(
  'LICENSE_SERVER_URL',
  defaultValue: 'http://127.0.0.1:8000',
);

/// Secure-storage key the activation token is persisted under — shared
/// between license_screen.dart (writes on activation) and splash_screen.dart
/// (reads on every app start for offline re-validation).
const String licenseTokenStorageKey = 'lumacore_license_token';
