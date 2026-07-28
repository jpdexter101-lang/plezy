#include "wayland_video_surface.h"

#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "color-management-v1-client-protocol.h"

namespace mpv {
namespace {

// The client understands up to this version of color-management-v1; KWin 6.4
// implements 1. Binding min(advertised, this) keeps newer compositors working
// without requiring them.
constexpr uint32_t kColorManagerMaxVersion = 3;

bool Fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

// What the compositor answered while its globals were being bound.
struct RegistryTarget {
  wl_subcompositor* subcompositor = nullptr;
  wp_color_manager_v1* color_manager = nullptr;
  bool parametric = false;
  bool pq = false;
  bool bt2020 = false;
  bool mastering = false;
  bool done = false;
};

void ManagerIntent(void* data, wp_color_manager_v1* manager, uint32_t intent) {
  (void)data; (void)manager; (void)intent;
}
void ManagerFeature(void* data, wp_color_manager_v1* manager, uint32_t feature) {
  (void)manager;
  auto* target = static_cast<RegistryTarget*>(data);
  if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) target->parametric = true;
  // Gates set_mastering_luminance as well as the primaries request it is named
  // after. Sending either without this advertised is a fatal protocol error,
  // not a soft failure, so it has to be tracked rather than assumed.
  if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) {
    target->mastering = true;
  }
}
void ManagerTransferFunction(void* data, wp_color_manager_v1* manager, uint32_t tf) {
  (void)manager;
  auto* target = static_cast<RegistryTarget*>(data);
  if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) target->pq = true;
}
void ManagerPrimaries(void* data, wp_color_manager_v1* manager, uint32_t primaries) {
  (void)manager;
  auto* target = static_cast<RegistryTarget*>(data);
  if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) target->bt2020 = true;
}
void ManagerDone(void* data, wp_color_manager_v1* manager) {
  (void)manager;
  static_cast<RegistryTarget*>(data)->done = true;
}

const wp_color_manager_v1_listener kManagerListener = {
    ManagerIntent, ManagerFeature, ManagerTransferFunction, ManagerPrimaries, ManagerDone,
};

void RegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
  auto* target = static_cast<RegistryTarget*>(data);
  if (g_strcmp0(interface, "wl_subcompositor") == 0 && target->subcompositor == nullptr) {
    target->subcompositor =
        static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  } else if (g_strcmp0(interface, "wp_color_manager_v1") == 0 && target->color_manager == nullptr) {
    const uint32_t bind_version = version < kColorManagerMaxVersion ? version : kColorManagerMaxVersion;
    target->color_manager = static_cast<wp_color_manager_v1*>(
        wl_registry_bind(registry, name, &wp_color_manager_v1_interface, bind_version));
  }
}

void RegistryGlobalRemove(void* data, wl_registry* registry, uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

const wl_registry_listener kRegistryListener = {RegistryGlobal, RegistryGlobalRemove};

wl_surface* ParentSurface(GtkWidget* view) {
  GtkWidget* toplevel = gtk_widget_get_toplevel(view);
  if (toplevel == nullptr) return nullptr;
  GdkWindow* window = gtk_widget_get_window(toplevel);
  if (window == nullptr || !GDK_IS_WAYLAND_WINDOW(window)) return nullptr;
  return gdk_wayland_window_get_wl_surface(GDK_WAYLAND_WINDOW(window));
}

}  // namespace

WaylandVideoSurface::~WaylandVideoSurface() { Destroy(); }

bool WaylandVideoSurface::IsSupported(GdkDisplay* display) {
  return display != nullptr && GDK_IS_WAYLAND_DISPLAY(display);
}

bool WaylandVideoSurface::BindGlobals(GdkDisplay* display, std::string* error) {
  wl_display_ = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
  compositor_ = gdk_wayland_display_get_wl_compositor(GDK_WAYLAND_DISPLAY(display));
  if (wl_display_ == nullptr || compositor_ == nullptr) {
    return Fail(error, "Wayland display or compositor is unavailable");
  }

  // Bind on a private queue so the roundtrip cannot dispatch GDK's own events
  // from inside this call, then hand the bound global back to the default queue
  // that GDK's main-loop source already drives.
  wl_event_queue* queue = wl_display_create_queue(wl_display_);
  if (queue == nullptr) return Fail(error, "Failed to create a Wayland event queue");

  wl_registry* registry = wl_display_get_registry(wl_display_);
  if (registry == nullptr) {
    wl_event_queue_destroy(queue);
    return Fail(error, "Failed to obtain the Wayland registry");
  }
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);

  RegistryTarget target;
  wl_registry_add_listener(registry, &kRegistryListener, &target);
  bool round_tripped = wl_display_roundtrip_queue(wl_display_, queue) >= 0;

  // The colour manager reports what it supports right after binding, so a
  // second roundtrip is needed before those answers can be trusted.
  if (round_tripped && target.color_manager != nullptr) {
    wp_color_manager_v1_add_listener(target.color_manager, &kManagerListener, &target);
    for (int attempt = 0; attempt < 4 && !target.done; ++attempt) {
      if (wl_display_roundtrip_queue(wl_display_, queue) < 0) {
        round_tripped = false;
        break;
      }
    }
  }

  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);

  if (!round_tripped) return Fail(error, "Wayland roundtrip failed while binding globals");
  if (target.subcompositor == nullptr) return Fail(error, "Compositor does not expose wl_subcompositor");

  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(target.subcompositor), nullptr);
  subcompositor_ = target.subcompositor;

  if (target.color_manager != nullptr) {
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(target.color_manager), nullptr);
    color_manager_ = target.color_manager;
    // All three are required to describe a plane as HDR10. Anything less and
    // the plane simply stays sRGB and mpv keeps tone-mapping as it does today.
    supports_hdr_ = target.done && target.parametric && target.pq && target.bt2020;
    // Optional on top: without it the plane is still described as PQ / BT.2020,
    // the compositor just has to tone-map against its own assumptions rather
    // than the source's mastering display.
    supports_mastering_ = target.mastering;
    if (!supports_hdr_) {
      g_message(
          "MPV video plane: compositor colour management is incomplete "
          "(parametric=%d pq=%d bt2020=%d); HDR passthrough unavailable",
          target.parametric, target.pq, target.bt2020);
    }
  }
  return true;
}

bool WaylandVideoSurface::InitEgl(std::string* error) {
  // The plane's EGL stack is deliberately independent of Flutter's: nothing is
  // shared, so the context is free to be ES 3.x (mpv wants compute shaders for
  // hdr-compute-peak, and >8-bit render targets for HDR later).
  egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(wl_display_));
  if (egl_display_ == EGL_NO_DISPLAY) return Fail(error, "No EGL display for the Wayland connection");
  if (!eglInitialize(egl_display_, nullptr, nullptr)) {
    egl_display_ = EGL_NO_DISPLAY;
    return Fail(error, "eglInitialize failed for the video plane");
  }

  // Video is opaque, so no alpha channel is requested: the compositor can then
  // treat the plane as opaque and may promote it to a hardware plane.
  //
  // 10 bits first, because PQ quantised to 8 bits bands visibly; 8 is the
  // fallback and simply means HDR stays off.
  for (const int depth : {10, 8}) {
    for (const EGLint renderable : {EGL_OPENGL_ES3_BIT, EGL_OPENGL_ES2_BIT}) {
      const EGLint attributes[] = {
          EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, renderable,
          EGL_RED_SIZE,     depth,          EGL_GREEN_SIZE,      depth,
          EGL_BLUE_SIZE,    depth,          EGL_ALPHA_SIZE,      0,
          EGL_NONE,
      };
      EGLConfig config = nullptr;
      EGLint count = 0;
      if (eglChooseConfig(egl_display_, attributes, &config, 1, &count) && count == 1) {
        egl_config_ = config;
        depth_bits_ = depth;
        return true;
      }
    }
  }
  return Fail(error, "No matching EGL config for the video plane");
}

bool WaylandVideoSurface::Create(GtkWidget* view, std::string* error) {
  if (view == nullptr) return Fail(error, "Video plane requires a realized view");
  GdkDisplay* display = gtk_widget_get_display(view);
  if (!IsSupported(display)) return Fail(error, "Not a Wayland display");

  wl_surface* parent = ParentSurface(view);
  if (parent == nullptr) return Fail(error, "Toplevel has no Wayland surface yet");

  view_ = view;
  if (!BindGlobals(display, error) || !InitEgl(error)) {
    Destroy();
    return false;
  }

  surface_ = wl_compositor_create_surface(compositor_);
  if (surface_ == nullptr) {
    Destroy();
    return Fail(error, "Failed to create the video wl_surface");
  }

  // Input belongs to the Flutter view, never to the video plane. An empty input
  // region makes the compositor route pointer and touch straight through — the
  // Wayland twin of keeping the Windows video child out of the hit-test path.
  wl_region* empty = wl_compositor_create_region(compositor_);
  if (empty != nullptr) {
    wl_surface_set_input_region(surface_, empty);
    wl_region_destroy(empty);
  }

  subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, surface_, parent);
  if (subsurface_ == nullptr) {
    Destroy();
    return Fail(error, "Failed to create the video wl_subsurface");
  }
  // Below the Flutter surface, and independent of its frame loop.
  wl_subsurface_place_below(subsurface_, parent);
  wl_subsurface_set_desync(subsurface_);

  // A 1x1 window keeps EGL happy until the first SetRect() arrives.
  egl_window_ = wl_egl_window_create(surface_, 1, 1);
  if (egl_window_ == nullptr) {
    Destroy();
    return Fail(error, "Failed to create the video wl_egl_window");
  }

  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_, reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    Destroy();
    return Fail(error, "Failed to create the video EGL surface");
  }

  // Note: the swap interval cannot be set here — eglSwapInterval acts on the
  // surface bound to the *current* context, and none is current yet. It is set
  // in MpvPlayer::InitRenderContextForSurface once the context is bound.

  if (color_manager_ != nullptr) {
    color_surface_ = wp_color_manager_v1_get_surface(color_manager_, surface_);
  }
  // PQ in 8 bits bands badly enough to be worse than tone-mapping to SDR, so
  // HDR is only offered when the plane actually got a 10-bit config.
  if (supports_hdr_ && (color_surface_ == nullptr || depth_bits_ < 10)) {
    supports_hdr_ = false;
    g_message("MPV video plane: HDR unavailable (colour surface=%p, depth=%d bits)",
              static_cast<void*>(color_surface_), depth_bits_);
  }
  g_message("MPV video plane: %d bits per channel, HDR %s", depth_bits_,
            supports_hdr_ ? "available" : "unavailable");

  RequestParentCommit();
  return true;
}

void WaylandVideoSurface::ClearImageDescription() {
  if (image_description_ != nullptr) {
    wp_image_description_v1_destroy(image_description_);
    image_description_ = nullptr;
  }
}

void WaylandVideoSurface::HandleImageDescriptionReady(
    void* data, wp_image_description_v1* desc, uint32_t identity) {
  (void)identity;
  auto* self = static_cast<WaylandVideoSurface*>(data);
  if (self->image_description_ != desc || self->color_surface_ == nullptr) return;
  if (!self->hdr_requested_) return;

  wp_color_management_surface_v1_set_image_description(
      self->color_surface_, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  self->hdr_active_ = true;
  g_message("MPV video plane: PQ / BT.2020 image description attached");
  // Surface state is double-buffered; it lands on the next commit.
  self->RequestParentCommit();
}

void WaylandVideoSurface::HandleImageDescriptionFailed(
    void* data, wp_image_description_v1* desc, uint32_t cause, const char* message) {
  (void)desc;
  auto* self = static_cast<WaylandVideoSurface*>(data);
  self->hdr_active_ = false;
  g_warning("MPV video plane: compositor rejected the HDR image description (cause %u): %s", cause,
            message ? message : "no reason given");
  self->ClearImageDescription();
}

namespace {

bool SameMetadata(const HdrMetadata& a, const HdrMetadata& b) {
  return a.max_cll == b.max_cll && a.max_fall == b.max_fall &&
         a.max_luminance == b.max_luminance && a.min_luminance == b.min_luminance;
}

}  // namespace

void WaylandVideoSurface::SetHdr(bool enabled, const HdrMetadata& metadata) {
  if (!supports_hdr_ || color_surface_ == nullptr) return;
  if (enabled == hdr_requested_ && (!enabled || image_description_ != nullptr)) {
    // Already in the requested state, but the caller may have learned more
    // about the source since - HDR is commonly switched on before a file is
    // open, when there is no metadata to read yet.
    if (enabled) RefreshHdrMetadata(metadata);
    return;
  }

  hdr_requested_ = enabled;
  ClearImageDescription();

  if (!enabled) {
    if (hdr_active_) {
      wp_color_management_surface_v1_unset_image_description(color_surface_);
      hdr_active_ = false;
      RequestParentCommit();
    }
    return;
  }

  metadata_ = metadata;
  BuildImageDescription();
}

void WaylandVideoSurface::RefreshHdrMetadata(const HdrMetadata& metadata) {
  if (!hdr_requested_ || !supports_hdr_ || color_surface_ == nullptr) return;
  if (SameMetadata(metadata_, metadata)) return;
  metadata_ = metadata;
  // The description is immutable once created, so new metadata means building
  // a replacement and swapping it in on the next commit.
  ClearImageDescription();
  BuildImageDescription();
}

void WaylandVideoSurface::BuildImageDescription() {
  const HdrMetadata& metadata = metadata_;
  wp_image_description_creator_params_v1* creator =
      wp_color_manager_v1_create_parametric_creator(color_manager_);
  if (creator == nullptr) {
    g_warning("MPV video plane: compositor refused a parametric image-description creator");
    hdr_requested_ = false;
    return;
  }

  wp_image_description_creator_params_v1_set_tf_named(
      creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  wp_image_description_creator_params_v1_set_primaries_named(
      creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);

  // Only forward metadata the source actually carried. Inventing values here
  // would have the compositor tone-map against a mastering display that never
  // existed, which is worse than letting it apply its own default.
  //
  // Without any of this the compositor has to assume the worst case the PQ
  // curve allows - 10000 nits - and rolls the highlights off far harder than
  // the content needs, which is visible as crushed, flat highlights next to a
  // player that does forward it.
  if (supports_mastering_ && metadata.max_luminance > 0) {
    // min L is carried scaled by 10000 to keep four decimals. The protocol
    // raises invalid_luminance - a fatal error, not a rejected description -
    // if max is not strictly greater than min, so clamp rather than trust the
    // file.
    const uint32_t min_lum = static_cast<uint32_t>(metadata.min_luminance * 10000.0 + 0.5);
    const uint32_t max_lum = metadata.max_luminance;
    if (max_lum * 10000u > min_lum) {
      wp_image_description_creator_params_v1_set_mastering_luminance(creator, min_lum, max_lum);
    }
  }
  if (metadata.max_cll > 0) {
    wp_image_description_creator_params_v1_set_max_cll(creator, metadata.max_cll);
  }
  if (metadata.max_fall > 0) {
    wp_image_description_creator_params_v1_set_max_fall(creator, metadata.max_fall);
  }

  // create() consumes the creator, so it must not be destroyed afterwards.
  static const wp_image_description_v1_listener kDescriptionListener = {
      HandleImageDescriptionFailed,
      HandleImageDescriptionReady,
  };
  image_description_ = wp_image_description_creator_params_v1_create(creator);
  if (image_description_ == nullptr) {
    g_warning("MPV video plane: could not create the HDR image description");
    hdr_requested_ = false;
    return;
  }
  wp_image_description_v1_add_listener(image_description_, &kDescriptionListener, this);
}

void WaylandVideoSurface::ClearFrameCallback() {
  if (frame_callback_ != nullptr) {
    wl_callback_destroy(frame_callback_);
    frame_callback_ = nullptr;
  }
  frame_pending_ = false;
}

void WaylandVideoSurface::HandleFrameDone(void* data, wl_callback* callback, uint32_t time) {
  (void)time;
  auto* self = static_cast<WaylandVideoSurface*>(data);
  if (self->frame_callback_ == callback) {
    wl_callback_destroy(self->frame_callback_);
    self->frame_callback_ = nullptr;
  } else if (callback != nullptr) {
    wl_callback_destroy(callback);
  }
  self->frame_pending_ = false;
  // Rendering resumes from here, not from mpv: its redraw latch is still set
  // from the update we declined to serve, so it will not notify again.
  if (self->on_frame_) self->on_frame_();
}

void WaylandVideoSurface::Destroy() {
  if (egl_surface_ != EGL_NO_SURFACE) {
    if (eglGetCurrentSurface(EGL_DRAW) == egl_surface_ || eglGetCurrentSurface(EGL_READ) == egl_surface_) {
      eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglDestroySurface(egl_display_, egl_surface_);
    egl_surface_ = EGL_NO_SURFACE;
  }
  if (egl_window_ != nullptr) {
    wl_egl_window_destroy(egl_window_);
    egl_window_ = nullptr;
  }
  ClearFrameCallback();
  on_frame_ = nullptr;
  ClearImageDescription();
  if (color_surface_ != nullptr) {
    wp_color_management_surface_v1_destroy(color_surface_);
    color_surface_ = nullptr;
  }
  if (color_manager_ != nullptr) {
    wp_color_manager_v1_destroy(color_manager_);
    color_manager_ = nullptr;
  }
  supports_hdr_ = false;
  hdr_requested_ = false;
  hdr_active_ = false;
  depth_bits_ = 8;
  if (subsurface_ != nullptr) {
    wl_subsurface_destroy(subsurface_);
    subsurface_ = nullptr;
  }
  if (surface_ != nullptr) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }
  if (subcompositor_ != nullptr) {
    wl_subcompositor_destroy(subcompositor_);
    subcompositor_ = nullptr;
  }
  // compositor_, wl_display_ and the EGLDisplay itself are owned by GDK/EGL and
  // are shared process-wide; only our own references are dropped here.
  compositor_ = nullptr;
  wl_display_ = nullptr;
  egl_config_ = nullptr;
  egl_display_ = EGL_NO_DISPLAY;
  view_ = nullptr;
  width_ = 0;
  height_ = 0;
  visible_ = false;
  buffer_attached_ = false;
}

void WaylandVideoSurface::RequestParentCommit() {
  // Subsurface position and stacking are double-buffered *parent* state: they
  // only land when the parent surface commits. Asking the view to redraw is the
  // one way to make GTK do that without reaching into its pending state.
  if (view_ != nullptr) gtk_widget_queue_draw(view_);
}

void WaylandVideoSurface::SetRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t scale) {
  if (scale < 1) scale = 1;
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  if (x == x_ && y == y_ && width == width_ && height == height_ && scale == scale_) return;

  const bool size_changed = width != width_ || height != height_;
  const bool scale_changed = scale != scale_;
  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;
  scale_ = scale;

  if (surface_ == nullptr || subsurface_ == nullptr || egl_window_ == nullptr) return;

  if (scale_changed) wl_surface_set_buffer_scale(surface_, scale_);
  if (size_changed || scale_changed) wl_egl_window_resize(egl_window_, width_, height_, 0, 0);
  // Positions are surface-local, i.e. logical units in the parent's frame.
  wl_subsurface_set_position(subsurface_, x_ / scale_, y_ / scale_);
  RequestParentCommit();
}

void WaylandVideoSurface::SetVisible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  if (surface_ == nullptr) return;
  if (!visible) {
    ClearFrameCallback();
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    buffer_attached_ = false;
  }
  // Becoming visible needs no action here: the next Present() attaches a buffer.
  RequestParentCommit();
}

bool WaylandVideoSurface::Present() {
  if (!visible_ || egl_surface_ == EGL_NO_SURFACE || frame_pending_) return false;

  // Ask for the acknowledgement before the commit that eglSwapBuffers performs,
  // so the callback belongs to this frame.
  static const wl_callback_listener kFrameListener = {HandleFrameDone};
  frame_callback_ = wl_surface_frame(surface_);
  if (frame_callback_ != nullptr) {
    wl_callback_add_listener(frame_callback_, &kFrameListener, this);
    frame_pending_ = true;
  }

  if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
    ClearFrameCallback();
    g_warning("MPV video plane: eglSwapBuffers failed: 0x%x", eglGetError());
    return false;
  }
  if (!buffer_attached_) {
    buffer_attached_ = true;
    // The first buffer changes what the plane occludes; make sure the parent's
    // view of the subsurface is up to date.
    RequestParentCommit();
  }
  return true;
}

}  // namespace mpv
