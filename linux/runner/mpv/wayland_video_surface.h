#ifndef PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
#define PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_

#include <EGL/egl.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>

#include "hdr_metadata.h"

struct wl_callback;
struct wl_compositor;
struct wl_display;
struct wl_egl_window;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_color_management_surface_v1;
struct wp_color_management_surface_feedback_v1;
struct wp_color_manager_v1;
struct wp_image_description_v1;
struct wp_image_description_info_v1;

namespace mpv {

// The compositor's preferred colour encoding for a surface, as delivered by
// wp_image_description_info_v1 in response to get_information.
//
// Only the fields that matter for video are kept. The important one is
// max_luminance: it is the output's *target* peak (KWin sources it from an HDR
// peak override, else the EDID's desired maximum, else 800 nits), which is the
// number a player needs if it is going to tone-map for the display itself.
// Nothing else in the protocol reveals it — PQ's own encoded maximum is always
// 10000 regardless of the panel.
struct PreferredColorDescription {
  bool valid = false;                 // a complete info burst has arrived
  bool pq = false;                    // transfer function is ST2084 PQ
  bool bt2020 = false;                // container primaries are BT.2020
  uint32_t max_luminance = 0;         // nits, the output's target peak
  uint32_t min_luminance_scaled = 0;  // nits * 10000, the output's target floor
  uint32_t reference_luminance = 0;   // nits, diffuse/SDR white
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
  // whether the *display* is HDR — see output_is_hdr() for that.
  bool supports_hdr() const { return supports_hdr_; }

  // What the compositor says it would prefer for this surface, from
  // wp_color_management_surface_feedback_v1. This is the only way to learn the
  // output's *real* peak: an HDR output's preferred description carries the
  // panel's target luminance, where PQ's own nominal maximum is always 10000.
  const PreferredColorDescription& preferred() const { return preferred_; }

  // True when the output this surface sits on is actually in HDR, i.e. the
  // compositor's preferred transfer function for it is PQ. Offering an HDR
  // toggle on an SDR output only invites the compositor to tone-map a plane
  // that never needed to be PQ.
  bool output_is_hdr() const { return preferred_.valid && preferred_.pq; }

  // Invoked on the GTK main thread when the compositor's preferred description
  // changes — a monitor move, or HDR being switched on or off under us.
  void SetPreferredChangedCallback(std::function<void()> callback) { on_preferred_changed_ = std::move(callback); }

  // Number of bits per colour channel the plane actually got (10 when the
  // driver offered a 10-bit window config, otherwise 8). PQ in 8 bits bands
  // badly, so HDR is only offered at 10.
  int depth_bits() const { return depth_bits_; }

  // Changing HDR is a two-phase transition, because the surface's colour state
  // and the buffer it applies to land on the *same* commit — and that commit is
  // the child surface's, performed by eglSwapBuffers inside Present().
  //
  // Doing it in one step cannot be atomic in either order. Attach first and mpv
  // is still producing the old curve; change mpv first and the description is
  // still being validated by the compositor, so several frames of the new curve
  // get presented under the old description — PQ read as sRGB, which is a visible
  // flash, not a subtlety.
  //
  // So: Begin() stages and validates, and holds Present() while it does; the
  // caller switches mpv only once it settles; Commit() then attaches and releases
  // the hold so the very next render pairs the new pixels with the new state on
  // one commit. Abort() backs out and changes nothing.
  //
  // `describe` is an instruction, not a request: DecideHdr in hdr_metadata.h has
  // already weighed the app's permission, this surface's capabilities, the
  // output's state and the source.
  //
  // `on_settled(token, true)` means Commit may proceed. It fires synchronously
  // when there is nothing to validate, so the caller must tolerate re-entry.
  //
  // The token identifies *this* transition, and Commit and Abort ignore any other.
  // Without it a transition that has settled but not yet committed — its mpv
  // property request still in flight — could be replaced by a newer one, and the
  // older request's completion would then commit the newer description alongside
  // the older pixels. A token of zero means nothing was staged, so there is
  // nothing to commit or abort.
  void BeginHdrTransition(bool describe, const HdrMetadata& metadata, std::function<void(uint64_t, bool)> on_settled);

  // Applies the transition named by `token`. Returns true when the plane should
  // be re-rendered and presented at once, so the new state reaches the screen
  // instead of waiting for whatever frame mpv happens to produce next. Ignores a
  // token that is not the staged one.
  bool CommitHdrTransition(uint64_t token);

  // Discards the transition named by `token` and releases the hold. The committed
  // colour state is left exactly as it was. Ignores a stale token.
  void AbortHdrTransition(uint64_t token);

  // True while a transition is staged, i.e. while Present() is being held.
  bool hdr_transition_staged() const { return transition_staged_; }

  // Drops any staged transition and unsets the description immediately.
  //
  // For the case where mpv's colour space had to be forced back to SDR while
  // unwinding a refused change: the description already committed is then no
  // longer true of the pixels, and aborting alone would leave it in place. Returns
  // true when the plane should be re-rendered and presented at once.
  bool ForceUndescribed();

  // Whether this source could be described at all: it carries an HDR curve, a
  // BT.2020 container, and the compositor advertised that specific named pair.
  //
  // Public because the caller has to know the answer *before* it changes mpv's
  // output colour space — the pixels have to be committed to before the surface
  // is described, or the two disagree for a frame.
  bool CanDescribeSource(const HdrMetadata& metadata) const;

  bool hdr_requested() const { return hdr_requested_; }
  bool hdr_active() const { return hdr_active_; }

 private:
  bool BindGlobals(GdkDisplay* display, std::string* error);
  void BuildImageDescription();
  bool InitEgl(std::string* error);
  void RequestParentCommit();
  void ClearFrameCallback();
  // Destroys the description staged for the pending transition, if any. The
  // attached one is never held: set_image_description has copy semantics, so the
  // object is destroyed as soon as it has been handed over.
  void ClearStagedDescription();
  // Tears the staged transition down unconditionally and tells whoever was
  // waiting that it will not be committed. The token-checked Abort delegates here;
  // supersession and teardown call it directly.
  void DiscardTransition();
  // Shared tail of the transition's outcome, whichever event delivered it.
  void SettleTransition(bool ok);

  static void HandleFrameDone(void* data, wl_callback* callback, uint32_t time);
  // Interface version 1 only; version 2 and later send ready2 in its place.
  static void HandleImageDescriptionReady(void* data, wp_image_description_v1* desc, uint32_t identity);
  // Interface version 2+. Leaving this null would not be a missed notification
  // but a call through a null pointer: libwayland invokes implementation[opcode]
  // without checking it.
  static void HandleImageDescriptionReady2(
      void* data, wp_image_description_v1* desc, uint32_t identity_hi, uint32_t identity_lo);
  static void HandleImageDescriptionFailed(
      void* data, wp_image_description_v1* desc, uint32_t cause, const char* message);

  // Creates the preferred-description query. The returned description is ready
  // immediately per the protocol, so get_information follows on ready, and the
  // accumulated fields are committed when the info burst ends with done.
  void BeginPreferredQuery();
  void ClearPreferredQuery();
  void CommitPreferredQuery();

  static void HandlePreferredChanged(void* data, wp_color_management_surface_feedback_v1* feedback, uint32_t identity);
  static void HandlePreferredChanged2(
      void* data, wp_color_management_surface_feedback_v1* feedback, uint32_t identity_hi, uint32_t identity_lo);
  static void HandlePreferredReady(void* data, wp_image_description_v1* desc, uint32_t identity);
  static void HandlePreferredReady2(
      void* data, wp_image_description_v1* desc, uint32_t identity_hi, uint32_t identity_lo);
  static void HandlePreferredFailed(void* data, wp_image_description_v1* desc, uint32_t cause, const char* message);

  // The eleven-member info listener, built once. A member rather than a
  // file-local so it can name the private handlers below.
  static const wp_image_description_info_v1_listener& InfoListener();

  // wp_image_description_info_v1 has eleven events and every one of them must
  // be present in the listener. Only tf_named, primaries_named, luminances and
  // target_luminance carry anything we use; the rest are deliberate no-ops
  // rather than omissions.
  static void HandleInfoDone(void* data, wp_image_description_info_v1* info);
  static void HandleInfoIccFile(void* data, wp_image_description_info_v1* info, int32_t icc, uint32_t icc_size);
  static void HandleInfoPrimaries(
      void* data, wp_image_description_info_v1* info, int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x,
      int32_t b_y, int32_t w_x, int32_t w_y);
  static void HandleInfoPrimariesNamed(void* data, wp_image_description_info_v1* info, uint32_t primaries);
  static void HandleInfoTfPower(void* data, wp_image_description_info_v1* info, uint32_t eexp);
  static void HandleInfoTfNamed(void* data, wp_image_description_info_v1* info, uint32_t tf);
  static void HandleInfoLuminances(
      void* data, wp_image_description_info_v1* info, uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum);
  static void HandleInfoTargetPrimaries(
      void* data, wp_image_description_info_v1* info, int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x,
      int32_t b_y, int32_t w_x, int32_t w_y);
  static void HandleInfoTargetLuminance(
      void* data, wp_image_description_info_v1* info, uint32_t min_lum, uint32_t max_lum);
  static void HandleInfoTargetMaxCll(void* data, wp_image_description_info_v1* info, uint32_t max_cll);
  static void HandleInfoTargetMaxFall(void* data, wp_image_description_info_v1* info, uint32_t max_fall);

  GtkWidget* view_ = nullptr;

  wl_display* wl_display_ = nullptr;           // owned by GDK
  wl_compositor* compositor_ = nullptr;        // owned by GDK
  wl_subcompositor* subcompositor_ = nullptr;  // bound by us
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
  // The description being validated for a staged transition. Never the attached
  // one: set_image_description copies, so the object is destroyed immediately
  // after it is handed over.
  wp_image_description_v1* staged_description_ = nullptr;
  // A transition is staged: Present() is held, and Commit or Abort will release
  // it. `staged_describe_` is what Commit will apply, and `transition_token_` is
  // what Commit and Abort must match to act on it.
  bool transition_staged_ = false;
  uint64_t transition_token_ = 0;
  bool staged_describe_ = false;
  bool staged_settled_ = false;
  HdrMetadata staged_metadata_;
  std::function<void(uint64_t, bool)> on_transition_settled_;

  // Feedback lives for the whole surface lifetime so preferred_changed keeps
  // arriving; the description and info objects are transient, created per query
  // and destroyed as soon as their values have been copied out.
  wp_color_management_surface_feedback_v1* color_feedback_ = nullptr;
  wp_image_description_v1* preferred_description_ = nullptr;
  wp_image_description_info_v1* preferred_info_ = nullptr;
  PreferredColorDescription preferred_;
  PreferredColorDescription pending_preferred_;
  std::function<void()> on_preferred_changed_;
  HdrMetadata metadata_;
  int depth_bits_ = 8;
  // supports_hdr_ is the aggregate gate; the three below are what the compositor
  // advertised individually, because whether a *given* source can be described
  // depends on its own curve, not on the aggregate.
  bool supports_hdr_ = false;
  bool supports_pq_ = false;
  bool supports_hlg_ = false;
  bool supports_bt2020_ = false;
  // What the compositor will accept in a luminance description. Consulted by
  // PlanHdrLuminance, which turns it plus the source into a legal request set.
  CompositorLuminanceSupport luminance_support_;
  // Whether a description is currently wanted for the source in hand. Set only
  // by SetHdr, from the instruction it was given.
  bool hdr_requested_ = false;
  bool hdr_active_ = false;
};

}  // namespace mpv

#endif  // PLEZY_LINUX_MPV_WAYLAND_VIDEO_SURFACE_H_
