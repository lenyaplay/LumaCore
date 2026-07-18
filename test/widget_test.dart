import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:lumacore/main.dart';

void main() {
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
