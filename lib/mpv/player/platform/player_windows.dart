import '../player_native.dart';
import '../video_rect_support.dart';

/// Uses libmpv with native window embedding behind the Flutter window.
class PlayerWindows extends PlayerNative with VideoRectSupport {
  // Native window embedding, not a Flutter texture.
  @override
  int? get textureId => null;

  // setVideoRect comes from VideoRectSupport; the geometry request is identical
  // on every native-surface platform.
}
