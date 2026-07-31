import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:plezy/mpv/player/player_native.dart';
import 'package:plezy/providers/playback_state_provider.dart';
import 'package:plezy/screens/video_player_screen.dart';
import 'package:plezy/services/settings_service.dart';
import 'package:provider/provider.dart';

import '../../test_helpers/media_items.dart';
import '../../test_helpers/mock_player_channels.dart';
import '../../test_helpers/prefs.dart';

/// The negative control for linux_hdr_startup_test.dart: startup tolerates
/// `HDR_UNSUPPORTED` specifically, not every failure of the HDR property. A
/// refusal that means something else is a real fault and must still abort
/// initialization rather than leave the player half-configured.
///
/// Separate file because PlayerBase keeps a static event-channel owner map, so a
/// second screen in the same isolate waits out debugNativeOwnershipDisposeTimeout
/// and never initializes at all. Every other test here that mounts
/// VideoPlayerScreen is one-per-file for the same reason.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() async {
    resetSharedPreferencesForTest();
    SettingsService.resetForTesting();
    await SettingsService.getInstance();
    // Would be written immediately after the HDR block, had it not thrown.
    await SettingsService.instance.write(SettingsService.audioSyncOffset, 250);
    PlayerNative.debugUseLinuxVideoBootstrap = true;
  });

  tearDown(() => PlayerNative.debugUseLinuxVideoBootstrap = null);

  testWidgets('a refusal that is not HDR_UNSUPPORTED is not swallowed', (tester) async {
    final calls = <MethodCall>[];

    await withMockPlayerChannels(
      methodChannelName: 'com.plezy/mpv_player',
      eventChannelName: 'com.plezy/mpv_player/events',
      methodHandler: (call) {
        calls.add(call);
        if (call.method == 'setProperty' && (call.arguments as Map)['name'] == 'hdr-enabled') {
          return Future<Object?>.error(PlatformException(code: 'MPV_ERROR', message: 'property unavailable'));
        }
        return switch (call.method) {
          'initialize' => Future<Object?>.value(73),
          _ => Future<Object?>.value(null),
        };
      },
      eventHandler: (call) async => null,
      testBody: () async {
        await tester.pumpWidget(_screen());
        await _pumpUntil(
          tester,
          () => _propertyWrites(calls).contains('hdr-enabled'),
          describe: () => _propertyWrites(calls).toString(),
        );
        // Let anything that was going to follow the rethrow actually arrive.
        await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 50)));
        await tester.pump();

        expect(_propertyWrites(calls), isNot(contains('audio-delay')));

        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 10)));
        await tester.pump();
      },
    );
  });
}

List<String> _propertyWrites(List<MethodCall> calls) => [
  for (final call in calls)
    if (call.method == 'setProperty') (call.arguments as Map)['name'] as String,
];

Widget _screen() {
  return ChangeNotifierProvider(
    create: (_) => PlaybackStateProvider(),
    child: MaterialApp(
      home: VideoPlayerScreen(metadata: testMediaItem(title: 'Linux HDR refusal test video'), isOffline: true),
    ),
  );
}

Future<void> _pumpUntil(WidgetTester tester, bool Function() condition, {String Function()? describe}) async {
  for (var i = 0; i < 200 && !condition(); i++) {
    await tester.pump(const Duration(milliseconds: 10));
    if (!condition()) {
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 5)));
    }
  }
  expect(condition(), isTrue, reason: describe == null ? null : 'observed ${describe()}');
}
