#ifndef PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
#define PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_

#include <EGL/egl.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <string>

struct wl_compositor;
struct wl_display;
struct wl_egl_window;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;

namespace mpv {

// A native Wayland video plane: a wl_subsurface stacked *below* the Flutter
// toplevel surface, carrying its own EGL window surface that mpv renders into
// directly.
//
// This is the Linux analogue of the Windows video child HWND. Flutter's own
// surface keeps the UI and alpha-blends over this one, so video never travels
// through Flutter's compositor — which on GTK3 costs one CPU-side upload of the
// whole window surface per presented frame (see gdk_cairo_draw_from_gl's
// alpha path), previously paid once per *video* frame.
//
// Everything here runs on the GTK main thread. The subsurface is desynchronized
// so its commits are independent of the parent's frame loop; position and
// stacking, however, are *parent* state and only take effect on a parent
// commit, which is why SetRect() asks the view to redraw.
//
// mpv is never told about Wayland: it only ever sees the EGL surface's default
// framebuffer. Embedding mpv into a foreign Wayland surface is not possible
// (mpv's --wid does not work on Wayland and upstream considers it out of
// scope), so the app owns the subsurface and drives the render itself.
class WaylandVideoSurface {
 public:
  WaylandVideoSurface() = default;
  ~WaylandVideoSurface();

  WaylandVideoSurface(const WaylandVideoSurface&) = delete;
  WaylandVideoSurface& operator=(const WaylandVideoSurface&) = delete;

  // True when the process is on a Wayland display. Cheap; safe to call before
  // the view is realized, so it can gate the window's visual.
  static bool IsSupported(GdkDisplay* display);

  // Binds the Wayland globals, creates the subsurface under `view`'s toplevel,
  // and creates an EGL window surface on it.
  //
  // `depth_bits` is the per-channel colour depth of the EGL config (8 today;
  // 10 is what HDR passthrough will want). Returns false and fills `error` on
  // any failure — the caller must then fall back to the Flutter texture path.
  bool Create(GtkWidget* view, int depth_bits, std::string* error);

  // Releases the EGL surface, subsurface and Wayland objects. Idempotent.
  void Destroy();

  bool valid() const { return egl_surface_ != EGL_NO_SURFACE; }
  EGLDisplay egl_display() const { return egl_display_; }
  EGLConfig egl_config() const { return egl_config_; }
  EGLSurface egl_surface() const { return egl_surface_; }

  // Current buffer size in physical pixels. Zero until the first SetRect().
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  bool has_size() const { return width_ > 0 && height_ > 0; }

  // Places and sizes the plane. Coordinates are physical pixels in the
  // toplevel's frame, matching what the Dart side sends via setVideoRect.
  void SetRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t scale);

  // Hides the plane by attaching a null buffer. The next Present() re-shows it.
  void SetVisible(bool visible);
  bool visible() const { return visible_; }

  // Presents whatever was rendered into the EGL surface.
  bool Present();

 private:
  bool BindGlobals(GdkDisplay* display, std::string* error);
  bool InitEgl(int depth_bits, std::string* error);
  void RequestParentCommit();

  GtkWidget* view_ = nullptr;

  wl_display* wl_display_ = nullptr;          // owned by GDK
  wl_compositor* compositor_ = nullptr;       // owned by GDK
  wl_subcompositor* subcompositor_ = nullptr; // bound by us
  wl_surface* surface_ = nullptr;
  wl_subsurface* subsurface_ = nullptr;
  wl_egl_window* egl_window_ = nullptr;

  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLConfig egl_config_ = nullptr;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;

  int32_t x_ = 0;
  int32_t y_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t scale_ = 1;
  bool visible_ = false;
  bool buffer_attached_ = false;
};

}  // namespace mpv

#endif  // PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
