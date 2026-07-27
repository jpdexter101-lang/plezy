// Measurement harness entrypoint. NOT part of the app.
//
// Drives the real PlayerLinux/mpv/Video rendering path with a local file so
// the compositing cost can be measured without a Plex server. Build with:
//   flutter build linux --target=lib/harness_main.dart
// Run with:
//   PLEZY_HARNESS_MEDIA=/path/to/file.mp4 PLEZY_HARNESS_SECONDS=40 ./plezy
//
// Set PLEZY_HARNESS_MPV_LOG=v (or debug) for mpv's own log stream.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'mpv/models.dart';
import 'mpv/player/player.dart';
import 'mpv/player/player_base.dart';
import 'mpv/video.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const _HarnessApp());
}

class _HarnessApp extends StatefulWidget {
  const _HarnessApp();

  @override
  State<_HarnessApp> createState() => _HarnessAppState();
}

class _HarnessAppState extends State<_HarnessApp> {
  Player? _player;
  String _status = 'starting';

  int _frames = 0;
  int _buildUs = 0;
  int _rasterUs = 0;
  final Stopwatch _clock = Stopwatch()..start();
  Timer? _reportTimer;
  Timer? _probeTimer;
  Timer? _quitTimer;
  Timer? _insetTimer;
  double _inset = 0;

  @override
  void initState() {
    super.initState();
    SchedulerBinding.instance.addTimingsCallback(_onFrames);
    _reportTimer = Timer.periodic(const Duration(seconds: 2), (_) => _report());
    _probeTimer = Timer.periodic(const Duration(seconds: 2), (_) => _probe());

    final seconds = int.tryParse(Platform.environment['PLEZY_HARNESS_SECONDS'] ?? '');
    if (seconds != null && seconds > 0) {
      _quitTimer = Timer(Duration(seconds: seconds), () {
        _report();
        stdout.writeln('HARNESS_DONE');
        exit(0);
      });
    }
    final inset = double.tryParse(Platform.environment['PLEZY_HARNESS_INSET'] ?? '');
    if (inset != null && inset > 0) {
      _insetTimer = Timer.periodic(const Duration(seconds: 6), (_) {
        setState(() => _inset = _inset == 0 ? inset : 0);
        stdout.writeln('HARNESS_INSET $_inset');
      });
    }
    _start();
  }

  void _onFrames(List<FrameTiming> timings) {
    for (final t in timings) {
      _frames++;
      _buildUs += t.buildDuration.inMicroseconds;
      _rasterUs += t.rasterDuration.inMicroseconds;
    }
  }

  void _report() {
    final n = _frames;
    final secs = _clock.elapsedMilliseconds / 1000.0;
    final fps = secs > 0 ? n / secs : 0.0;
    final build = n > 0 ? (_buildUs / n / 1000.0) : 0.0;
    final raster = n > 0 ? (_rasterUs / n / 1000.0) : 0.0;
    stdout.writeln(
      'FRAMESTAT t=${secs.toStringAsFixed(1)} frames=$n '
      'fps=${fps.toStringAsFixed(2)} build_ms=${build.toStringAsFixed(2)} '
      'raster_ms=${raster.toStringAsFixed(2)}',
    );
    _frames = 0;
    _buildUs = 0;
    _rasterUs = 0;
    _clock.reset();
  }

  // Independent of Flutter's frame loop: tells us whether mpv is actually
  // advancing even when nothing is being composited.
  Future<void> _probe() async {
    final player = _player;
    if (player == null) return;
    try {
      final pos = await player.getProperty('time-pos');
      final paused = await player.getProperty('pause');
      final dropped = await player.getProperty('frame-drop-count');
      final decoded = await player.getProperty('decoder-frame-drop-count');
      final texture = player is PlayerBase ? player.textureId : null;
      stdout.writeln(
        'PROBE time-pos=$pos pause=$paused drops=$dropped dec_drops=$decoded texture=$texture',
      );
    } catch (e) {
      stdout.writeln('PROBE_ERROR $e');
    }
  }

  Future<void> _start() async {
    final media = Platform.environment['PLEZY_HARNESS_MEDIA'];
    if (media == null || media.isEmpty) {
      setState(() => _status = 'set PLEZY_HARNESS_MEDIA');
      return;
    }
    final uri = media.startsWith('/') ? 'file://$media' : media;
    try {
      final player = Player();
      setState(() => _player = player);
      final level = Platform.environment['PLEZY_HARNESS_MPV_LOG'];
      if (level != null && level.isNotEmpty) {
        await player.setLogLevel(level);
        stdout.writeln('HARNESS_MPV_LOG $level');
      }
      if (Platform.environment['PLEZY_HARNESS_HDR'] == '1') {
        try {
          await player.setProperty('hdr-enabled', 'yes');
          stdout.writeln('HARNESS_HDR requested');
        } catch (e) {
          stdout.writeln('HARNESS_HDR_ERROR $e');
        }
      }
      await player.open(Media(uri));
      stdout.writeln('HARNESS_OPENED $uri');
      final id = player is PlayerBase ? player.textureId : null;
      stdout.writeln('HARNESS_TEXTURE_ID $id');
    } catch (e, st) {
      stdout.writeln('HARNESS_ERROR $e\n$st');
      setState(() => _status = 'error: $e');
    }
  }

  @override
  void dispose() {
    _reportTimer?.cancel();
    _probeTimer?.cancel();
    _quitTimer?.cancel();
    _insetTimer?.cancel();
    SchedulerBinding.instance.removeTimingsCallback(_onFrames);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final player = _player;
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      // Transparent all the way down: in plane mode the video is a Wayland
      // subsurface *below* this surface, so anything opaque here hides it.
      home: Scaffold(
        backgroundColor: Colors.transparent,
        body: player == null
            ? Center(child: Text(_status, style: const TextStyle(color: Colors.white)))
            : Padding(
                // PLEZY_HARNESS_INSET insets the video rect so the native plane
                // has to move *and* resize to a non-zero origin, which a plain
                // window resize would never exercise.
                padding: EdgeInsets.all(_inset),
                child: Video(player: player, backgroundColor: Colors.transparent),
              ),
      ),
    );
  }
}
