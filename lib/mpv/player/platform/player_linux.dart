import '../player_native.dart';
import '../video_rect_support.dart';

/// Uses libmpv on one of two render paths, chosen natively at initialize time.
///
/// On Wayland, video goes to a native `wl_subsurface` stacked below the Flutter
/// surface — the same shape Windows gets from a child HWND. `initialize` then
/// returns a bool, [textureId] stays null, and geometry arrives via
/// [setVideoRect].
///
/// Everywhere else (X11, or a compositor without `wl_subcompositor`) the native
/// side falls back to `FlTextureGL`: `initialize` returns a texture id and the
/// `Video` widget renders a `Texture`, leaving [setVideoRect] a native no-op.
class PlayerLinux extends PlayerNative with VideoRectSupport {
  @override
  Future<void> setVideoRect({
    required int left,
    required int top,
    required int right,
    required int bottom,
    required double devicePixelRatio,
  }) async {
    await invoke('setVideoRect', {
      'left': left,
      'top': top,
      'right': right,
      'bottom': bottom,
      'devicePixelRatio': devicePixelRatio,
    });
  }
}
