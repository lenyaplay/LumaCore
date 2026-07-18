import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../features/camera/camera_screen.dart';
import '../../features/gallery/gallery_screen.dart';
import '../../features/license/license_screen.dart';
import '../../features/settings/settings_screen.dart';
import '../../widgets/app_shell.dart';

final appRouterProvider = Provider<GoRouter>((ref) {
  return GoRouter(
    initialLocation: '/license',
    routes: [
      GoRoute(path: '/license', builder: (context, state) => const LicenseScreen()),
      ShellRoute(
        builder: (context, state, child) => AppShell(location: state.uri.toString(), child: child),
        routes: [
          GoRoute(path: '/camera', builder: (context, state) => const CameraScreen()),
          GoRoute(path: '/gallery', builder: (context, state) => const GalleryScreen()),
          GoRoute(path: '/settings', builder: (context, state) => const SettingsScreen()),
        ],
      ),
    ],
  );
});
