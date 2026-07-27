#ifndef PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
#define PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_

#include <EGL/egl.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>

struct wl_callback;
struct wl_compositor;
struct wl_display;
struct wl_egl_window;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_color_management_surface_v1;
struct wp_color_manager_v1;
struct wp_image_description_v1;

namespace mpv {

// HDR10 static metadata carried by the source, as reported by mpv's
// video-params. A zero field means the source did not carry it, and the
// corresponding protocol request is then skipped so the compositor can apply
// its own default rather than being told something untrue.
struct HdrMetadata {
  uint32_t max_cll = 0;           // nits, maximum content light level
  uint32_t max_fall = 0;          // nits, maximum frame-average light level
  uint32_t max_luminance = 0;     // nits, mastering display maximum
  double min_luminance = 0.0;     // nits, mastering display minimum
};

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
  // The plane is created at 10 bits per channel when the driver offers such a
  // window config, falling back to 8. Returns false and fills `error` on any
  // failure — the caller must then fall back to the Flutter texture path.
  bool Create(GtkWidget* view, std::string* error);

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

  // True while a committed frame has not yet been acknowledged by the
  // compositor. Callers must not render while this holds: the compositor stops
  // acknowledging frames for an occluded or minimized surface, and rendering
  // regardless would queue work that can never drain.
  bool frame_pending() const { return frame_pending_; }

  // Invoked on the GTK main thread when the compositor acknowledges a frame.
  // This is what resumes rendering after the plane becomes visible again, so
  // it must trigger a render — mpv's redraw latch stays set while frames are
  // being skipped and will not notify again on its own.
  void SetFrameCallback(std::function<void()> callback) { on_frame_ = std::move(callback); }

  // Presents whatever was rendered into the EGL surface. No-op while hidden or
  // while a frame is still pending.
  bool Present();

  // True when the compositor can describe a PQ / BT.2020 surface, i.e. it
  // offers a parametric image-description creator and advertises both the
  // ST2084 transfer function and BT.2020 primaries. This says nothing about
  // whether the *display* is HDR: an SDR output simply means the compositor
  // tone-maps the plane instead of passing it through.
  bool supports_hdr() const { return supports_hdr_; }

  // Number of bits per colour channel the plane actually got (10 when the
  // driver offered a 10-bit window config, otherwise 8). PQ in 8 bits bands
  // badly, so HDR is only offered at 10.
  int depth_bits() const { return depth_bits_; }

  // Describes the plane as PQ / BT.2020, or clears the description so it is
  // treated as sRGB again. Building an image description is asynchronous: the
  // compositor validates it and answers ready or failed, and only then is it
  // attached, landing on a subsequent commit.
  void SetHdr(bool enabled, const HdrMetadata& metadata);
  bool hdr_requested() const { return hdr_requested_; }
  bool hdr_active() const { return hdr_active_; }

 private:
  bool BindGlobals(GdkDisplay* display, std::string* error);
  bool InitEgl(std::string* error);
  void RequestParentCommit();
  void ClearFrameCallback();
  void ClearImageDescription();

  static void HandleFrameDone(void* data, wl_callback* callback, uint32_t time);
  static void HandleImageDescriptionReady(void* data, wp_image_description_v1* desc, uint32_t identity);
  static void HandleImageDescriptionFailed(void* data, wp_image_description_v1* desc, uint32_t cause,
                                           const char* message);

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
  bool frame_pending_ = false;
  wl_callback* frame_callback_ = nullptr;
  std::function<void()> on_frame_;

  wp_color_manager_v1* color_manager_ = nullptr;
  wp_color_management_surface_v1* color_surface_ = nullptr;
  wp_image_description_v1* image_description_ = nullptr;
  int depth_bits_ = 8;
  bool supports_hdr_ = false;
  bool hdr_requested_ = false;
  bool hdr_active_ = false;
};

}  // namespace mpv

#endif  // PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
