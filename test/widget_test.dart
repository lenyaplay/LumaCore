import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:lumacore/main.dart';

void main() {
  const nativeChannel = MethodChannel('com.lumacore/native');

  setUp(() {
    // No native camera in the widget-test host — respond as if the platform
    // rejected the call, so CameraScreen settles into its error state
    // instead of spinning forever waiting for a reply that never arrives.
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(
      nativeChannel,
      (call) async => throw PlatformException(code: 'UNAVAILABLE'),
    );
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(nativeChannel, null);
  });

  testWidgets('app boots to the license screen', (tester) async {
    await tester.pumpWidget(const ProviderScope(child: LumaCoreApp()));
    await tester.pumpAndSettle();

    expect(find.text('LumaCore'), findsOneWidget);
    expect(find.widgetWithText(FilledButton, 'Activate'), findsOneWidget);
  });

  testWidgets('activating navigates to the camera tab', (tester) async {
    await tester.pumpWidget(const ProviderScope(child: LumaCoreApp()));
    await tester.pumpAndSettle();

    await tester.tap(find.widgetWithText(FilledButton, 'Activate'));
    await tester.pumpAndSettle();

    expect(find.byIcon(Icons.fiber_manual_record), findsOneWidget);
    expect(find.byType(NavigationBar), findsOneWidget);
  });
}
