#include "wayland_video_surface.h"

#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>

namespace mpv {
namespace {

bool Fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

struct RegistryTarget {
  wl_subcompositor* subcompositor = nullptr;
};

void RegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
  auto* target = static_cast<RegistryTarget*>(data);
  if (g_strcmp0(interface, "wl_subcompositor") == 0 && target->subcompositor == nullptr) {
    target->subcompositor =
        static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  }
  (void)version;
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
  const bool round_tripped = wl_display_roundtrip_queue(wl_display_, queue) >= 0;

  wl_registry_destroy(registry);
  wl_event_queue_destroy(queue);

  if (!round_tripped) return Fail(error, "Wayland roundtrip failed while binding globals");
  if (target.subcompositor == nullptr) return Fail(error, "Compositor does not expose wl_subcompositor");

  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(target.subcompositor), nullptr);
  subcompositor_ = target.subcompositor;
  return true;
}

bool WaylandVideoSurface::InitEgl(int depth_bits, std::string* error) {
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
  for (const EGLint renderable : {EGL_OPENGL_ES3_BIT, EGL_OPENGL_ES2_BIT}) {
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE,     depth_bits,     EGL_GREEN_SIZE,      depth_bits,
        EGL_BLUE_SIZE,    depth_bits,     EGL_ALPHA_SIZE,      0,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(egl_display_, attributes, &config, 1, &count) && count == 1) {
      egl_config_ = config;
      return true;
    }
  }
  return Fail(error, "No matching EGL config for the video plane");
}

bool WaylandVideoSurface::Create(GtkWidget* view, int depth_bits, std::string* error) {
  if (view == nullptr) return Fail(error, "Video plane requires a realized view");
  GdkDisplay* display = gtk_widget_get_display(view);
  if (!IsSupported(display)) return Fail(error, "Not a Wayland display");

  wl_surface* parent = ParentSurface(view);
  if (parent == nullptr) return Fail(error, "Toplevel has no Wayland surface yet");

  view_ = view;
  if (!BindGlobals(display, error) || !InitEgl(depth_bits, error)) {
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

  RequestParentCommit();
  return true;
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
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    buffer_attached_ = false;
  }
  // Becoming visible needs no action here: the next Present() attaches a buffer.
  RequestParentCommit();
}

bool WaylandVideoSurface::Present() {
  if (!visible_ || egl_surface_ == EGL_NO_SURFACE) return false;
  if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
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
