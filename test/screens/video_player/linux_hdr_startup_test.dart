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

/// Startup pushes the HDR preference at a native plane that is allowed to refuse
/// it: the Linux plugin answers `HDR_UNSUPPORTED` whenever the compositor, the
/// output and the source do not all agree. Losing playback over that would make
/// the feature worse than not having it.
///
/// `audio-delay` is written immediately after the HDR block, so its arrival is
/// what says initialization carried on past the refusal. The matching negative
/// control - that a refusal meaning something else is *not* swallowed - lives in
/// linux_hdr_startup_refusal_test.dart, because PlayerBase keeps a static
/// event-channel owner map and a second player in the same isolate waits out
/// debugNativeOwnershipDisposeTimeout instead of initializing.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() async {
    resetSharedPreferencesForTest();
    SettingsService.resetForTesting();
    await SettingsService.getInstance();
    // Non-zero so the write that follows the HDR block actually happens.
    await SettingsService.instance.write(SettingsService.audioSyncOffset, 250);
    PlayerNative.debugUseLinuxVideoBootstrap = true;
  });

  tearDown(() => PlayerNative.debugUseLinuxVideoBootstrap = null);

  testWidgets('an SDR output refusing HDR passthrough does not stop playback starting', (tester) async {
    final calls = <MethodCall>[];

    await withMockPlayerChannels(
      methodChannelName: 'com.plezy/mpv_player',
      eventChannelName: 'com.plezy/mpv_player/events',
      methodHandler: (call) {
        calls.add(call);
        if (call.method == 'setProperty' && (call.arguments as Map)['name'] == 'hdr-enabled') {
          return Future<Object?>.error(PlatformException(code: 'HDR_UNSUPPORTED', message: 'output is not in HDR'));
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
          () => _propertyWrites(calls).contains('audio-delay'),
          describe: () => _propertyWrites(calls).toString(),
        );

        expect(_propertyWrites(calls), contains('hdr-enabled'));

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
      home: VideoPlayerScreen(metadata: testMediaItem(title: 'Linux HDR startup test video'), isOffline: true),
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
