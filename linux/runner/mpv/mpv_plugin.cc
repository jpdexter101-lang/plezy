#include "mpv_plugin.h"

#include <cstring>
#include <deque>
#include <functional>
#include <new>

#include "mpv_texture.h"
#include "wayland_video_surface.h"

enum class VideoBootstrapState { kIdle, kPending, kReady, kFailed };
using PlayerPtr = std::unique_ptr<mpv::MpvPlayer>;
using VideoSurfacePtr = std::unique_ptr<mpv::WaylandVideoSurface>;

// One queued HDR transaction: what to apply, and who to tell when it settles.
//
// `mode` is only the caller's when it *is* the mode request. Anything else
// inherits whatever is in force at the moment its turn comes: a mode captured at
// enqueue time could have been refused since, and applying it then would put a
// curve on screen that Dart has already been told did not take.
struct PendingHdrRequest {
  bool allow = false;
  bool inherit_mode = true;
  mpv::HdrToneMapping mode = mpv::HdrToneMapping::kCompositor;
  std::function<void(int)> done;
};

using HdrQueue = std::deque<PendingHdrRequest>;

struct _MpvPlugin {
  GObject parent_instance;

  FlPluginRegistrar* registrar;
  FlMethodChannel* method_channel;
  FlEventChannel* event_channel;
  FlTextureRegistrar* texture_registrar;

  PlayerPtr player;
  MpvTexture* texture;  // owned via GObject ref
  // Non-null when video goes to a native Wayland plane below the Flutter
  // surface instead of through a Flutter texture. Mutually exclusive with
  // |texture|; see try_start_video_plane().
  VideoSurfacePtr video_surface;
  // Set when the plane must be redrawn even though mpv has no new frame -
  // after a resize or after becoming visible, where the buffer on screen is
  // stale or absent. Sticky, because the render it asks for may first have to
  // wait out an unacknowledged frame.
  gboolean plane_needs_render;
  gboolean texture_registered;
  gboolean visible;
  gboolean initialized;
  gboolean audio_only;
  // What Dart last asked for via hdr-enabled. Remembered because the answer can
  // change without Dart saying anything: moving the window to another monitor
  // changes whether the output is in HDR at all, and the plane has to be
  // re-described when it does.
  gboolean hdr_wanted;
  VideoBootstrapState bootstrap_state;
  gchar* bootstrap_error;
  FlMethodCall* ready_call;
  guint64 generation;
  // Who tone-maps when HDR is on. Defaults to the compositor, which is the
  // behaviour that shipped first and needs no knowledge of the display; the
  // player-side path is opted into.
  //
  // Two fields, deliberately. `hdr_tone_mapping` is the mode mpv last *accepted*;
  // `hdr_tone_mapping_desired` is what the app last asked for. Requests are
  // applied strictly in order, so an internal re-apply (playback restart,
  // preferred-description change) queued behind a user's mode change must carry
  // the desired mode - reading the committed one would send the stale mode last
  // and make it final.
  mpv::HdrToneMapping hdr_tone_mapping = mpv::HdrToneMapping::kCompositor;
  mpv::HdrToneMapping hdr_tone_mapping_desired = mpv::HdrToneMapping::kCompositor;
  // Bumped per user mode request, so a failure only reverts `desired` when no
  // newer request has already replaced it.
  uint64_t hdr_mode_request_serial = 0;
  // Exactly one HDR transaction runs at a time, end to end.
  //
  // A transaction spans staging and validating the image description, switching
  // mpv's output colour space, and committing both. Interleaving two cannot be
  // made consistent after the fact: a superseded transaction may already have
  // moved mpv, so refusing its commit leaves mpv on one curve and the surface
  // describing another. Serializing the whole thing is what makes that
  // unreachable, rather than something to detect and unwind.
  bool hdr_transaction_in_flight = false;
  // Waiting user requests, in order. Each keeps its own callback because each has
  // a Dart method call to answer, and answering one with another's outcome is the
  // same divergence one level up.
  HdrQueue hdr_queue;
  // Internal re-applies coalesce into a flag instead of queueing: they have no
  // caller to answer, playback-restart fires on every seek, and each transaction
  // re-reads the source when its turn comes, so collapsing several loses nothing.
  bool hdr_reapply_pending = false;
  // The peak mpv was last told to tone-map to, 0 meaning "do not". Compared
  // against the display's current peak so a move between two HDR outputs is not
  // mistaken for no change at all.
  uint32_t applied_target_peak = 0;
  guint ready_timeout_source_id;
};

// g_type_create_instance zeroes the instance and runs no constructor, so the
// member initialisers above never execute: every scalar starts at zero and
// nothing else assigns these. kCompositor has to *be* zero for that to land on
// the intended default.
static_assert(
    static_cast<int>(mpv::HdrToneMapping::kCompositor) == 0,
    "MpvPlugin's zeroed instance memory must decode as kCompositor");

G_DEFINE_TYPE(MpvPlugin, mpv_plugin, G_TYPE_OBJECT)

// Forward declarations
static void mpv_plugin_handle_method_call(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data);

static mpv::HdrMetadata read_source_hdr_metadata(MpvPlugin* self);
static void apply_hdr_state(MpvPlugin* self, bool allow, mpv::HdrToneMapping mode, std::function<void(int)> done);
static void request_hdr_reapply(MpvPlugin* self);
static void run_next_hdr_transaction(MpvPlugin* self);
static void submit_hdr_transaction(MpvPlugin* self, bool allow, std::function<void(int)> done);
static void submit_hdr_mode_transaction(
    MpvPlugin* self, bool allow, mpv::HdrToneMapping mode, std::function<void(int)> done);

// Events reach Dart unchanged; this only watches them go past. A playback
// restart is the first moment the source's colour space and HDR metadata are
// knowable, and HDR is normally permitted well before that - typically before
// any file is open - so the decision made back then has to be revisited.
//
// It goes through the full apply path rather than only refreshing the
// description, because a new file can change the *curve*: PQ to HLG, or HDR to
// SDR entirely, each of which mpv has to be reconfigured for and not just
// re-described. Self-cancelling when nothing changed, which matters because this
// also fires on every seek.
static void observe_event_for_hdr(MpvPlugin* self, FlValue* event) {
  if (!self->video_surface) return;
  if (event == nullptr || fl_value_get_type(event) != FL_VALUE_TYPE_MAP) return;
  FlValue* name = fl_value_lookup_string(event, "name");
  if (name == nullptr || fl_value_get_type(name) != FL_VALUE_TYPE_STRING) return;
  if (g_strcmp0(fl_value_get_string(name), "playback-restart") != 0) return;
  request_hdr_reapply(self);
}

static void send_event(MpvPlugin* self, FlValue* event) {
  observe_event_for_hdr(self, event);
  if (self->event_channel) {
    g_autoptr(GError) error = nullptr;
    if (!fl_event_channel_send(self->event_channel, event, nullptr, &error) && error != nullptr) {
      g_warning("Failed to send event: %s", error->message);
    }
  }
}

static gboolean handle_ready_timeout(gpointer user_data);

static void complete_ready_call(MpvPlugin* self, gboolean success, const char* message) {
  if (self->ready_timeout_source_id != 0) {
    g_source_remove(self->ready_timeout_source_id);
    self->ready_timeout_source_id = 0;
  }
  if (!self->ready_call) return;
  g_autoptr(FlMethodResponse) response =
      success ? FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr))
              : FL_METHOD_RESPONSE(fl_method_error_response_new(
                    "INIT_FAILED", message ? message : "Video initialization failed", nullptr));
  fl_method_call_respond(self->ready_call, response, nullptr);
  g_object_unref(self->ready_call);
  self->ready_call = nullptr;
}

static void release_video_resources(MpvPlugin* self) {
  ++self->generation;
  // Queued transactions will never run, and each may be holding a reference to a
  // Dart method call that has to be answered or it is leaked along with its
  // response.
  auto queued = std::move(self->hdr_queue);
  self->hdr_queue.clear();
  self->hdr_transaction_in_flight = false;
  self->hdr_reapply_pending = false;
  for (auto& request : queued) {
    if (request.done) request.done(MPV_ERROR_UNINITIALIZED);
  }
  if (self->player) {
    // The texture is a raw callback target. Revoke both callback paths before
    // unregistering or unreferencing it; Dispose then drains any callback
    // already holding a native lease.
    self->player->SetRedrawCallback(nullptr);
    self->player->SetEventCallback(nullptr);
  }
  if (self->texture) {
    if (self->texture_registered && self->texture_registrar) {
      fl_texture_registrar_unregister_texture(self->texture_registrar, FL_TEXTURE(self->texture));
      self->texture_registered = FALSE;
    }
    mpv_texture_dispose(self->texture);
    g_object_unref(self->texture);
    self->texture = nullptr;
  }
  if (self->player) {
    self->player->Dispose();
    self->player.reset();
  }
  // After the player is gone: disposal hands the render context to the
  // process-lifetime teardown queue, which releases it surfacelessly, so the
  // plane's EGL surface does not have to outlive it.
  if (self->video_surface) {
    self->video_surface->Destroy();
    self->video_surface.reset();
  }
  self->initialized = FALSE;
  self->visible = FALSE;
  self->plane_needs_render = FALSE;
}

// Renders and presents one frame on the native video plane. Skipped while the
// plane is hidden or has not been given a rect yet; both of those paths render
// explicitly once the condition clears, because mpv's redraw latch stays set
// until a render consumes it and would otherwise suppress every later frame.
//
// |force| is for the callers who need pixels regardless of whether mpv has
// produced a new frame: a resize, or the plane becoming visible again.
static void render_video_plane(MpvPlugin* self, gboolean force) {
  if (force) self->plane_needs_render = TRUE;
  if (!self->player || !self->video_surface || !self->video_surface->valid()) return;
  if (!self->video_surface->visible() || !self->video_surface->has_size()) return;
  // Present() is held while a colour transition is staged, so a render now would
  // be discarded. The forced render after CommitHdrTransition is what resumes;
  // plane_needs_render stays set meanwhile, so nothing is lost.
  if (self->video_surface->hdr_transition_staged()) return;
  // Skip entirely while the compositor has not acknowledged the last frame:
  // an occluded plane is never acknowledged, and rendering into it anyway
  // would burn GPU work on frames that can never be shown.
  if (self->video_surface->frame_pending()) return;
  // The frame callback fires once per *display* refresh, so rendering from it
  // unconditionally pins the plane to the monitor's rate - 120 swaps/s for
  // 60fps content on a 120Hz output, half of them redrawing the same picture.
  // mpv's redraw latch is what says a new frame actually exists.
  if (!self->plane_needs_render && !self->player->NeedsRedraw()) return;
  if (self->player->RenderToSurface(
          self->video_surface->egl_surface(), self->video_surface->width(), self->video_surface->height())) {
    self->plane_needs_render = FALSE;
    self->video_surface->Present();
  }
}

// Collects what the source actually is, plus its HDR10 static metadata.
//
// Both halves matter. The colour space decides whether the plane may be
// described as HDR at all — describing it because a setting is on, rather than
// because the stream carries an HDR curve, asks the compositor to undo a
// transform nobody applied. The luminances then decide how hard the compositor
// tone-maps: without them it must assume the worst case PQ permits, 10000 nits,
// and rolls highlights off far harder than the content needs.
//
// The protocol carries luminances as whole nits, and a value that rounds to zero
// would read as "not stated", so anything positive is kept at a minimum of 1.
static mpv::HdrMetadata read_source_hdr_metadata(MpvPlugin* self) {
  mpv::HdrMetadata metadata;
  if (!self->player) return metadata;
  mpv::MpvPlayer::SourceHdrMetadata source;
  if (!self->player->ReadSourceHdrMetadata(&source)) return metadata;

  // mpv's own trc / primaries names, from video/csputils.c's tables.
  if (source.transfer == "pq") {
    metadata.transfer = mpv::SourceTransfer::kPq;
  } else if (source.transfer == "hlg") {
    metadata.transfer = mpv::SourceTransfer::kHlg;
  }
  if (source.primaries == "bt.2020") {
    metadata.primaries = mpv::SourcePrimaries::kBt2020;
  }

  auto nits = [](double value) -> uint32_t {
    if (!(value > 0.0)) return 0;
    const double rounded = value + 0.5;
    if (rounded >= 4294967295.0) return 4294967295u;
    const uint32_t whole = static_cast<uint32_t>(rounded);
    return whole > 0 ? whole : 1;
  };
  metadata.max_cll = nits(source.max_cll);
  metadata.max_fall = nits(source.max_fall);
  metadata.max_luminance = nits(source.max_luminance);
  metadata.min_luminance = source.min_luminance;
  g_message(
      "MPV video plane: source is %s / %s, MaxCLL=%u MaxFALL=%u mastering=%.4f-%u nits",
      source.transfer.empty() ? "(no stream)" : source.transfer.c_str(),
      source.primaries.empty() ? "(no stream)" : source.primaries.c_str(), metadata.max_cll, metadata.max_fall,
      metadata.min_luminance, metadata.max_luminance);
  return metadata;
}

// Whether the client and the display could carry HDR at all, before the source
// is considered. Both halves must agree: this client has to be able to describe
// an HDR plane, and the output the surface sits on has to actually be in HDR.
// The second can change under us when the window moves between monitors, which
// is why nothing caches it.
static bool hdr_available(MpvPlugin* self) {
  return self->video_surface != nullptr && self->video_surface->supports_hdr() && self->video_surface->output_is_hdr();
}

// Applies an HDR state to both halves of the plane, atomically on screen.
//
// The surface's colour state and the buffer it describes land on the *same*
// child-surface commit, and that commit is performed by eglSwapBuffers inside
// Present(). So neither "pixels first" nor "description first" is atomic on its
// own: whichever goes second leaves a window in which presented frames carry one
// colour space while labelled with the other. On enable that window is the
// compositor's validation round-trip, and PQ frames read as sRGB are a visible
// flash.
//
// The three-step transition closes it:
//
//   1. BeginHdrTransition stages and validates the description, holding Present()
//      so nothing can commit mid-change.
//   2. Once it settles, mpv's output colour space is switched.
//   3. CommitHdrTransition attaches the state and releases the hold, and the
//      plane is rendered and presented immediately - so the first buffer in the
//      new colour space is the one that carries it.
//
// Any failure aborts, leaving both halves exactly as they were.
//
// `mode` is the *desired* tone-map owner, passed in rather than read from the
// plugin, so the committed field is updated only when the request carrying that
// mode is the one mpv accepted. Requests are applied in order, so the last
// success is what mpv holds and what gets committed last.
//
// The source is read once and the same snapshot drives every step.
static void apply_hdr_state(MpvPlugin* self, bool allow, mpv::HdrToneMapping mode, std::function<void(int)> done) {
  if (self->video_surface == nullptr || self->player == nullptr) {
    if (done) done(MPV_ERROR_UNINITIALIZED);
    return;
  }
  const mpv::HdrMetadata source = read_source_hdr_metadata(self);

  // One gate, in hdr_metadata.h, so the four conditions and the peak clamp are
  // testable without a compositor.
  mpv::HdrInputs inputs;
  inputs.allowed = allow;
  inputs.client_can_describe = self->video_surface->supports_hdr();
  inputs.output_is_hdr = self->video_surface->output_is_hdr();
  inputs.source_describable = self->video_surface->CanDescribeSource(source);
  inputs.requested = mode;
  inputs.display_peak_nits = self->video_surface->preferred().max_luminance;
  const mpv::HdrDecision decision = mpv::DecideHdr(inputs, source);

  // What the buffer will actually contain: the source untouched, or the same
  // curve and gamut reduced to the peak we are about to declare. DecideHdr
  // already clamped that peak to the curve's primary colour volume, so mpv aims
  // at exactly what the compositor is told.
  const mpv::HdrMetadata described =
      decision.tone_map_in_player ? mpv::DescribeTonemappedTo(source, decision.target_peak_nits) : source;
  const mpv::SourceTransfer transfer = decision.describe ? described.transfer : mpv::SourceTransfer::kSdr;
  const guint64 generation = self->generation;

  self->video_surface->BeginHdrTransition(
      decision.describe, described, [self, decision, transfer, mode, generation, done](uint64_t token, bool staged) {
        if (self->generation != generation || self->video_surface == nullptr) {
          if (done) done(MPV_ERROR_UNINITIALIZED);
          return;
        }
        if (!staged) {
          self->video_surface->AbortHdrTransition(token);
          g_warning("MPV video plane: colour transition abandoned; leaving HDR as it was");
          if (done) done(MPV_ERROR_UNSUPPORTED);
          return;
        }
        self->player->SetHdrOutput(
            transfer, decision.target_peak_nits,
            [self, decision, mode, generation, token, done](mpv::MpvPlayer::HdrOutputResult result, int error) {
              using Result = mpv::MpvPlayer::HdrOutputResult;
              // The plane may have been torn down while the property was in flight.
              if (self->generation != generation || self->video_surface == nullptr) {
                if (done) done(error);
                return;
              }
              switch (result) {
                case Result::kApplied: {
                  // Pixels and state now agree; publish them together. Transactions
                  // are serialized, so the only staged transition can be this one
                  // and the token is belt and braces: a false return means token
                  // zero, i.e. nothing needed staging because nothing changed.
                  const bool committed = self->video_surface->CommitHdrTransition(token);
                  // An earlier kUnknown hid the plane rather than show pixels it
                  // could not label. mpv answers again, so the visibility Dart
                  // actually asked for is restored here - otherwise the quarantine
                  // would outlive its cause and the video would stay black until
                  // the next unrelated visibility change.
                  const bool want_visible = self->visible != FALSE;
                  const bool unquarantined = want_visible && !self->video_surface->visible();
                  if (unquarantined) self->video_surface->SetVisible(true);
                  if (committed || unquarantined) render_video_plane(self, TRUE);
                  if (committed && decision.describe) {
                    g_message(
                        "MPV video plane: HDR on, tone mapping by %s",
                        decision.tone_map_in_player ? "the player" : "the compositor");
                  }
                  self->hdr_tone_mapping = mode;
                  self->applied_target_peak = decision.target_peak_nits;
                  break;
                }
                case Result::kRestored:
                  // mpv is back where it was, so the description already committed
                  // is still true of the pixels and must be left exactly alone.
                  self->video_surface->AbortHdrTransition(token);
                  g_warning(
                      "MPV video plane: mpv refused the %s output colour space and was put back, "
                      "so the surface description is unchanged: %s",
                      decision.describe ? "HDR" : "SDR", mpv_error_string(error));
                  break;
                case Result::kForcedSdr:
                  // mpv could not be put back and is now SDR. Any committed HDR
                  // description describes pixels that no longer exist, so it goes
                  // too - and immediately, paired with a fresh frame.
                  if (self->video_surface->ForceUndescribed()) render_video_plane(self, TRUE);
                  self->applied_target_peak = 0;
                  g_warning(
                      "MPV video plane: mpv's colour space could not be restored and was forced to "
                      "SDR; HDR withdrawn: %s",
                      mpv_error_string(error));
                  break;
                case Result::kUnknown:
                  // Nothing can be said truthfully about these pixels, so nothing is
                  // said and nothing is shown. A later transaction can recover.
                  self->video_surface->ForceUndescribed();
                  self->video_surface->SetVisible(false);
                  self->applied_target_peak = 0;
                  g_warning(
                      "MPV video plane: mpv's output colour space is no longer commandable; the "
                      "plane is hidden rather than shown mislabelled: %s",
                      mpv_error_string(error));
                  break;
              }
              if (done) done(error);
            });
      });
}

// Runs the next queued HDR transaction, or drains the coalesced internal
// re-apply once the queue empties.
static void run_next_hdr_transaction(MpvPlugin* self) {
  if (self->hdr_queue.empty()) {
    self->hdr_transaction_in_flight = false;
    if (self->hdr_reapply_pending) {
      self->hdr_reapply_pending = false;
      request_hdr_reapply(self);
    }
    return;
  }
  auto request = std::make_shared<PendingHdrRequest>(std::move(self->hdr_queue.front()));
  self->hdr_queue.pop_front();
  self->hdr_transaction_in_flight = true;
  // Resolved here rather than at enqueue, so a mode that has since been refused
  // is not applied on this request's back.
  const mpv::HdrToneMapping mode = request->inherit_mode ? self->hdr_tone_mapping_desired : request->mode;
  apply_hdr_state(self, request->allow, mode, [self, request](int error) {
    if (request->done) request->done(error);
    // Only now, with the surface unstaged and mpv settled, may the next one start.
    run_next_hdr_transaction(self);
  });
}

// Queues a transaction that carries whatever tone-mapping mode is in force when
// its turn comes. For everything that is not itself a mode change.
static void submit_hdr_transaction(MpvPlugin* self, bool allow, std::function<void(int)> done) {
  if (self->video_surface == nullptr || self->player == nullptr) {
    if (done) done(MPV_ERROR_UNINITIALIZED);
    return;
  }
  PendingHdrRequest request;
  request.allow = allow;
  request.done = std::move(done);
  self->hdr_queue.push_back(std::move(request));
  if (!self->hdr_transaction_in_flight) run_next_hdr_transaction(self);
}

// Queues the mode change itself, which is the one request whose mode is its own.
static void submit_hdr_mode_transaction(
    MpvPlugin* self, bool allow, mpv::HdrToneMapping mode, std::function<void(int)> done) {
  if (self->video_surface == nullptr || self->player == nullptr) {
    if (done) done(MPV_ERROR_UNINITIALIZED);
    return;
  }
  PendingHdrRequest request;
  request.allow = allow;
  request.inherit_mode = false;
  request.mode = mode;
  request.done = std::move(done);
  self->hdr_queue.push_back(std::move(request));
  if (!self->hdr_transaction_in_flight) run_next_hdr_transaction(self);
}

// The only entry point for internal re-applies: playback restarts and
// preferred-description changes. Nobody is waiting on these, so instead of
// queueing they collapse into a single pending flag and re-read the mode and the
// source when their turn comes.
static void request_hdr_reapply(MpvPlugin* self) {
  if (self->video_surface == nullptr) return;
  if (self->hdr_transaction_in_flight) {
    self->hdr_reapply_pending = true;
    return;
  }
  submit_hdr_transaction(self, self->hdr_wanted != FALSE, nullptr);
}

// Re-applies the current request when the compositor's preferred description
// moves - a monitor change, HDR switched on or off under the app, or the output
// going away entirely.
static void handle_preferred_changed(MpvPlugin* self) {
  if (self->video_surface == nullptr) return;

  // Ask the same gate that would run anyway what the answer is now, and only
  // re-apply when it differs from what is in force. The on/off state alone is not
  // enough: in player mode the peak we tone-map to *is* the display's peak, so
  // moving between two HDR outputs of different brightness changes what must be
  // sent while the boolean stays put.
  const mpv::HdrMetadata source = read_source_hdr_metadata(self);
  mpv::HdrInputs inputs;
  inputs.allowed = self->hdr_wanted != FALSE;
  inputs.client_can_describe = self->video_surface->supports_hdr();
  inputs.output_is_hdr = self->video_surface->output_is_hdr();
  inputs.source_describable = self->video_surface->CanDescribeSource(source);
  inputs.requested = self->hdr_tone_mapping_desired;
  inputs.display_peak_nits = self->video_surface->preferred().max_luminance;
  const mpv::HdrDecision decision = mpv::DecideHdr(inputs, source);

  if (decision.describe == self->video_surface->hdr_active() &&
      decision.target_peak_nits == self->applied_target_peak) {
    return;
  }
  g_message("MPV video plane: preferred description changed; re-evaluating HDR");
  request_hdr_reapply(self);
}

// Attempts the native Wayland video plane. Returns false when it is
// unavailable for any reason (X11, no wl_subcompositor, no usable EGL config),
// in which case the caller falls back to the Flutter texture path.
static gboolean try_start_video_plane(MpvPlugin* self, FlView* view) {
  if (view == nullptr) return FALSE;
  // Escape hatch for driver or compositor trouble in the field, and the A/B
  // switch for measuring the plane against the texture path.
  if (g_strcmp0(g_getenv("PLEZY_DISABLE_VIDEO_PLANE"), "1") == 0) {
    g_message("MPV: native video plane disabled by PLEZY_DISABLE_VIDEO_PLANE");
    return FALSE;
  }
  GtkWidget* widget = GTK_WIDGET(view);
  if (!mpv::WaylandVideoSurface::IsSupported(gtk_widget_get_display(widget))) return FALSE;

  auto surface = std::make_unique<mpv::WaylandVideoSurface>();
  std::string error;
  if (!surface->Create(widget, &error)) {
    g_message("MPV: native video plane unavailable (%s); using the Flutter texture path", error.c_str());
    return FALSE;
  }
  if (!self->player->InitRenderContextForSurface(
          surface->egl_display(), surface->egl_config(), surface->egl_surface(), surface->depth_bits())) {
    surface->Destroy();
    g_message("MPV: video-plane render context failed; using the Flutter texture path");
    return FALSE;
  }

  self->video_surface = std::move(surface);
  self->video_surface->SetFrameCallback([self]() { render_video_plane(self, FALSE); });
  self->video_surface->SetPreferredChangedCallback([self]() { handle_preferred_changed(self); });
  self->player->SetRedrawCallback([self]() { render_video_plane(self, FALSE); });
  return TRUE;
}

struct TextureReadyContext {
  MpvPlugin* plugin;
  guint64 generation;
};

struct TextureReadyResult {
  MpvPlugin* plugin;
  guint64 generation;
  gboolean success;
  gchar* message;
};

static gboolean handle_texture_ready_result(gpointer data) {
  auto* result = static_cast<TextureReadyResult*>(data);
  MpvPlugin* self = result->plugin;
  if (result->generation != self->generation || self->bootstrap_state != VideoBootstrapState::kPending) {
    return G_SOURCE_REMOVE;
  }

  if (result->success) {
    self->bootstrap_state = VideoBootstrapState::kReady;
    self->initialized = TRUE;
    complete_ready_call(self, TRUE, nullptr);
  } else {
    self->bootstrap_state = VideoBootstrapState::kFailed;
    g_free(self->bootstrap_error);
    self->bootstrap_error = g_strdup(result->message ? result->message : "Video initialization failed");
    complete_ready_call(self, FALSE, self->bootstrap_error);
    release_video_resources(self);
  }
  return G_SOURCE_REMOVE;
}

static void destroy_texture_ready_result(gpointer data) {
  auto* result = static_cast<TextureReadyResult*>(data);
  g_object_unref(result->plugin);
  g_free(result->message);
  delete result;
}

static void on_texture_ready(gboolean success, const gchar* message, gpointer user_data) {
  auto* context = static_cast<TextureReadyContext*>(user_data);
  auto* result = new TextureReadyResult{
      MPV_PLUGIN(g_object_ref(context->plugin)),
      context->generation,
      success,
      g_strdup(message),
  };
  g_main_context_invoke_full(
      nullptr, G_PRIORITY_DEFAULT, handle_texture_ready_result, result, destroy_texture_ready_result);
}

static void destroy_texture_ready_context(gpointer data) {
  auto* context = static_cast<TextureReadyContext*>(data);
  g_object_unref(context->plugin);
  delete context;
}

static gboolean handle_ready_timeout(gpointer user_data) {
  MpvPlugin* self = MPV_PLUGIN(user_data);
  self->ready_timeout_source_id = 0;
  if (self->bootstrap_state != VideoBootstrapState::kPending || !self->ready_call) {
    return G_SOURCE_REMOVE;
  }
  self->bootstrap_state = VideoBootstrapState::kFailed;
  g_free(self->bootstrap_error);
  self->bootstrap_error = g_strdup("Video texture did not become ready before the initialization deadline");
  complete_ready_call(self, FALSE, self->bootstrap_error);
  release_video_resources(self);
  return G_SOURCE_REMOVE;
}

static void mpv_plugin_dispose(GObject* object) {
  MpvPlugin* self = MPV_PLUGIN(object);
  complete_ready_call(self, FALSE, "Video initialization was cancelled");
  release_video_resources(self);
  self->bootstrap_state = VideoBootstrapState::kIdle;
  g_clear_pointer(&self->bootstrap_error, g_free);
  g_clear_object(&self->method_channel);
  g_clear_object(&self->event_channel);
  g_clear_object(&self->registrar);
  G_OBJECT_CLASS(mpv_plugin_parent_class)->dispose(object);
}

// GObject instances come from g_type_create_instance, which zeroes the memory and
// runs no C++ constructors, and are released without running destructors. Every
// non-trivial member therefore has to be placement-constructed here and destroyed
// in finalize, in reverse.
//
// A zeroed std::unique_ptr happens to behave like an empty one, which is why the
// two smart pointers survived without this; a zeroed std::deque does not - its
// internal map pointers must be initialised before the first push_back, or it
// dereferences null.
static void mpv_plugin_finalize(GObject* object) {
  MpvPlugin* self = MPV_PLUGIN(object);
  self->hdr_queue.~HdrQueue();
  self->video_surface.~VideoSurfacePtr();
  self->player.~PlayerPtr();
  G_OBJECT_CLASS(mpv_plugin_parent_class)->finalize(object);
}

static void mpv_plugin_class_init(MpvPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = mpv_plugin_dispose;
  G_OBJECT_CLASS(klass)->finalize = mpv_plugin_finalize;
}

static void mpv_plugin_init(MpvPlugin* self) {
  new (&self->player) PlayerPtr();
  new (&self->video_surface) VideoSurfacePtr();
  new (&self->hdr_queue) HdrQueue();
  self->visible = FALSE;
  self->initialized = FALSE;
  self->texture = nullptr;
  self->texture_registered = FALSE;
  self->texture_registrar = nullptr;
  self->audio_only = FALSE;
  self->bootstrap_state = VideoBootstrapState::kIdle;
  self->bootstrap_error = nullptr;
  self->ready_call = nullptr;
  self->generation = 0;
  self->ready_timeout_source_id = 0;
}

MpvPlugin* mpv_plugin_new(FlPluginRegistrar* registrar, const gchar* channel_name, gboolean audio_only) {
  MpvPlugin* self = MPV_PLUGIN(g_object_new(MPV_PLUGIN_TYPE, nullptr));

  self->registrar = FL_PLUGIN_REGISTRAR(g_object_ref(registrar));
  self->audio_only = audio_only;
  // The audio-only core never renders; leaving the texture registrar unset
  // makes the GL/texture path structurally unreachable for it.
  self->texture_registrar = audio_only ? nullptr : fl_plugin_registrar_get_texture_registrar(registrar);
  self->player = std::make_unique<mpv::MpvPlayer>(audio_only);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->method_channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar), channel_name, FL_METHOD_CODEC(codec));

  fl_method_channel_set_method_call_handler(self->method_channel, mpv_plugin_handle_method_call, self, nullptr);

  g_autofree gchar* event_channel_name = g_strconcat(channel_name, "/events", nullptr);
  self->event_channel =
      fl_event_channel_new(fl_plugin_registrar_get_messenger(registrar), event_channel_name, FL_METHOD_CODEC(codec));

  return self;
}

// Static references to keep the plugin instances alive.
static MpvPlugin* g_mpv_plugin = nullptr;
static MpvPlugin* g_mpv_audio_plugin = nullptr;

void mpv_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  g_mpv_plugin = mpv_plugin_new(registrar, "com.plezy/mpv_player", FALSE);
}

void mpv_audio_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  g_mpv_audio_plugin = mpv_plugin_new(registrar, "com.plezy/mpv_audio_player", TRUE);
}

/// Method call handler.
static void mpv_plugin_handle_method_call(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
  (void)channel;
  MpvPlugin* self = MPV_PLUGIN(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;

  if (strcmp(method, "initialize") == 0) {
    if (self->audio_only) {
      // Audio-only music core: no texture, no render context — mpv runs
      // with video disabled entirely (see MpvPlayer). Returns `true`; the
      // Dart side only treats int results as texture IDs.
      if (!self->initialized) {
        if (!self->player || self->player->IsDisposed()) {
          self->player = std::make_unique<mpv::MpvPlayer>(/*audio_only=*/true);
        }
        if (self->player->Initialize()) {
          self->player->SetEventCallback([self](FlValue* event) { send_event(self, event); });
          self->initialized = TRUE;
        }
      }
      if (self->initialized) {
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(TRUE)));
      } else {
        response =
            FL_METHOD_RESPONSE(fl_method_error_response_new("INIT_FAILED", "Failed to initialize MPV player", nullptr));
      }
    } else if (self->video_surface && self->video_surface->valid()) {
      // Native video plane: a bool result tells the Dart side there is no
      // texture, so the Video widget drives geometry via setVideoRect instead.
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(TRUE)));
    } else if (
        self->texture && (self->bootstrap_state == VideoBootstrapState::kPending ||
                          self->bootstrap_state == VideoBootstrapState::kReady)) {
      response =
          FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_int(mpv_texture_get_id(self->texture))));
    } else {
      g_clear_pointer(&self->bootstrap_error, g_free);
      self->bootstrap_state = VideoBootstrapState::kIdle;
      if (!self->player || self->player->IsDisposed()) {
        self->player = std::make_unique<mpv::MpvPlayer>();
      }

      if (!self->player->Initialize()) {
        release_video_resources(self);
        self->bootstrap_state = VideoBootstrapState::kFailed;
        self->bootstrap_error = g_strdup("Failed to initialize MPV player");
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INIT_FAILED", self->bootstrap_error, nullptr));
      } else if (try_start_video_plane(self, fl_plugin_registrar_get_view(self->registrar))) {
        // Native Wayland plane: no Flutter texture, no GPU bootstrap handshake.
        // Video composites below the Flutter surface, so presenting a video
        // frame no longer forces Flutter to redraw the whole window.
        ++self->generation;
        self->bootstrap_state = VideoBootstrapState::kReady;
        self->player->SetEventCallback([self](FlValue* event) { send_event(self, event); });
        self->initialized = TRUE;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(TRUE)));
      } else {
        FlView* view = fl_plugin_registrar_get_view(self->registrar);
        self->texture = mpv_texture_new(self->player.get(), self->texture_registrar, view);
        ++self->generation;
        self->bootstrap_state = VideoBootstrapState::kPending;
        auto* ready_context = new TextureReadyContext{MPV_PLUGIN(g_object_ref(self)), self->generation};
        mpv_texture_set_ready_callback(self->texture, on_texture_ready, ready_context, destroy_texture_ready_context);

        if (!fl_texture_registrar_register_texture(self->texture_registrar, FL_TEXTURE(self->texture))) {
          self->bootstrap_state = VideoBootstrapState::kFailed;
          self->bootstrap_error = g_strdup("Failed to register video texture");
          release_video_resources(self);
          response = FL_METHOD_RESPONSE(fl_method_error_response_new("INIT_FAILED", self->bootstrap_error, nullptr));
        } else {
          self->texture_registered = TRUE;
          MpvTexture* texture = self->texture;
          self->player->SetRedrawCallback([texture]() { mpv_texture_mark_frame_available(texture); });
          self->player->SetEventCallback([self](FlValue* event) { send_event(self, event); });
          mpv_texture_mark_frame_available(self->texture);
          response =
              FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_int(mpv_texture_get_id(self->texture))));
        }
      }
    }
  } else if (strcmp(method, "waitForVideoReady") == 0) {
    if (self->audio_only) {
      response = FL_METHOD_RESPONSE(
          fl_method_error_response_new("INIT_FAILED", "Audio players have no video readiness state", nullptr));
    } else if (self->bootstrap_state == VideoBootstrapState::kReady && self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    } else if (self->bootstrap_state == VideoBootstrapState::kFailed) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "INIT_FAILED", self->bootstrap_error ? self->bootstrap_error : "Video initialization failed", nullptr));
    } else if (self->bootstrap_state != VideoBootstrapState::kPending || !self->texture) {
      response = FL_METHOD_RESPONSE(
          fl_method_error_response_new("INIT_FAILED", "Video initialization is not pending", nullptr));
    } else if (self->ready_call) {
      response = FL_METHOD_RESPONSE(
          fl_method_error_response_new("INIT_IN_PROGRESS", "Video readiness is already being awaited", nullptr));
    } else {
      self->ready_call = FL_METHOD_CALL(g_object_ref(method_call));
      self->ready_timeout_source_id =
          g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 5, handle_ready_timeout, g_object_ref(self), g_object_unref);
      return;
    }
  } else if (strcmp(method, "dispose") == 0) {
    complete_ready_call(self, FALSE, "Video initialization was cancelled");
    release_video_resources(self);
    self->bootstrap_state = VideoBootstrapState::kIdle;
    g_clear_pointer(&self->bootstrap_error, g_free);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "command") == 0) {
    if (!self->player || !self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INITIALIZED", "Player not initialized", nullptr));
    } else {
      FlValue* args_value = fl_value_lookup_string(args, "args");
      if (args_value == nullptr || fl_value_get_type(args_value) != FL_VALUE_TYPE_LIST) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'args' list", nullptr));
      } else {
        std::vector<std::string> command_args;
        size_t len = fl_value_get_length(args_value);
        for (size_t i = 0; i < len; i++) {
          FlValue* item = fl_value_get_list_value(args_value, i);
          if (fl_value_get_type(item) == FL_VALUE_TYPE_STRING) {
            command_args.push_back(fl_value_get_string(item));
          }
        }
        g_object_ref(method_call);
        self->player->CommandAsync(command_args, [method_call](int error) {
          g_autoptr(FlMethodResponse) async_response = nullptr;
          if (error < 0) {
            async_response =
                FL_METHOD_RESPONSE(fl_method_error_response_new("COMMAND_FAILED", "MPV command failed", nullptr));
          } else {
            async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
          }
          fl_method_call_respond(method_call, async_response, nullptr);
          g_object_unref(method_call);
        });
        return;  // Response sent asynchronously
      }
    }
  } else if (strcmp(method, "setProperty") == 0) {
    if (!self->player || !self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          plezy::mpv_common::kSetPropertyNotInitializedCode, "Player not initialized", nullptr));
    } else {
      FlValue* name_value = fl_value_lookup_string(args, "name");
      FlValue* value_value = fl_value_lookup_string(args, "value");

      if (name_value == nullptr || fl_value_get_type(name_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'name'", nullptr));
      } else if (value_value == nullptr || fl_value_get_type(value_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'value'", nullptr));
      } else if (self->video_surface && g_strcmp0(fl_value_get_string(name_value), "hdr-tone-mapping") == 0) {
        // Not an mpv property: it selects which side reduces the source's range,
        // which changes both mpv's target-peak and the luminances the compositor
        // is told. Re-applied immediately so the switch is visible without a
        // seek.
        //
        // Unknown values are rejected rather than folded into the default. This
        // knob's whole purpose is A/B comparison, and silently answering a typo
        // with "compositor, success" would mislabel the very measurement it
        // exists to produce.
        const char* mode = fl_value_get_string(value_value);
        const bool is_player = g_strcmp0(mode, "player") == 0;
        const bool is_compositor = g_strcmp0(mode, "compositor") == 0;
        if (!is_player && !is_compositor) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "INVALID_ARGS", "hdr-tone-mapping must be 'compositor' or 'player'", nullptr));
        } else {
          const mpv::HdrToneMapping requested =
              is_player ? mpv::HdrToneMapping::kPlayer : mpv::HdrToneMapping::kCompositor;
          // A no-op answer is only honest once the mode has actually settled. If a
          // change is queued or running, `desired` holds a value mpv has not yet
          // accepted, and answering a duplicate with immediate success would have
          // Dart persist a mode the original request may still revert. Such a
          // duplicate is queued instead and gets a real outcome; the extra
          // property writes are idempotent.
          if (requested != self->hdr_tone_mapping || self->hdr_transaction_in_flight || !self->hdr_queue.empty()) {
            // `desired` moves now, so an internal re-apply that runs later carries
            // the new mode. `hdr_tone_mapping` itself is committed by the
            // transaction only if mpv accepts the change, which is what keeps
            // native and Dart - which does not persist on failure - in agreement.
            self->hdr_tone_mapping_desired = requested;
            const uint64_t serial = ++self->hdr_mode_request_serial;
            g_object_ref(method_call);
            submit_hdr_mode_transaction(
                self, self->hdr_wanted != FALSE, requested, [self, method_call, serial](int error) {
                  g_autoptr(FlMethodResponse) async_response = nullptr;
                  if (plezy::mpv_common::SetPropertyStatusSucceeded(error)) {
                    async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
                  } else {
                    // Hand the desire back to whatever is actually in force, read
                    // live rather than captured: an intervening request may have
                    // committed since. Skipped if a newer request already claimed
                    // the desire.
                    if (self->hdr_mode_request_serial == serial) {
                      self->hdr_tone_mapping_desired = self->hdr_tone_mapping;
                    }
                    async_response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                        plezy::mpv_common::kSetPropertyFailedCode, mpv_error_string(error), nullptr));
                  }
                  fl_method_call_respond(method_call, async_response, nullptr);
                  g_object_unref(method_call);
                });
            return;
          }
          response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
        }
      } else if (self->video_surface && g_strcmp0(fl_value_get_string(name_value), "hdr-enabled") == 0) {
        // HDR spans both halves of the plane: mpv has to emit PQ / BT.2020, and
        // the compositor has to be told that is what the buffer holds. Neither
        // alone produces HDR, so this cannot go through the plain property path.
        const bool enabled = plezy::mpv_common::ParseEnabledFlag(fl_value_get_string(value_value));
        self->hdr_wanted = enabled ? TRUE : FALSE;
        if (!hdr_available(self) && enabled) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "HDR_UNSUPPORTED", "This compositor, video plane or output cannot carry HDR", nullptr));
        } else {
          g_object_ref(method_call);
          submit_hdr_transaction(self, enabled, [method_call](int error) {
            g_autoptr(FlMethodResponse) async_response = nullptr;
            if (plezy::mpv_common::SetPropertyStatusSucceeded(error)) {
              async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
            } else {
              async_response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                  plezy::mpv_common::kSetPropertyFailedCode, mpv_error_string(error), nullptr));
            }
            fl_method_call_respond(method_call, async_response, nullptr);
            g_object_unref(method_call);
          });
          return;
        }
      } else {
        g_object_ref(method_call);
        self->player->SetPropertyAsync(
            fl_value_get_string(name_value), fl_value_get_string(value_value), [method_call](int error) {
              g_autoptr(FlMethodResponse) async_response = nullptr;
              if (plezy::mpv_common::SetPropertyStatusSucceeded(error)) {
                async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
              } else {
                const char* error_code = plezy::mpv_common::SetPropertyErrorCode(error);
                const std::string description = error == MPV_ERROR_UNINITIALIZED
                                                    ? std::string("Player not initialized")
                                                    : plezy::mpv_common::SetPropertyErrorDescription(error);
                async_response =
                    FL_METHOD_RESPONSE(fl_method_error_response_new(error_code, description.c_str(), nullptr));
              }
              fl_method_call_respond(method_call, async_response, nullptr);
              g_object_unref(method_call);
            });
        return;  // Response sent asynchronously
      }
    }
  } else if (strcmp(method, "setLogLevel") == 0) {
    if (!self->player || !self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INITIALIZED", "Player not initialized", nullptr));
    } else {
      FlValue* level_value = fl_value_lookup_string(args, "level");

      if (level_value == nullptr || fl_value_get_type(level_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'level'", nullptr));
      } else {
        self->player->SetLogLevel(fl_value_get_string(level_value));
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      }
    }
  } else if (strcmp(method, "getProperty") == 0) {
    if (!self->player || !self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INITIALIZED", "Player not initialized", nullptr));
    } else {
      FlValue* name_value = fl_value_lookup_string(args, "name");

      if (name_value == nullptr || fl_value_get_type(name_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'name'", nullptr));
      } else {
        g_object_ref(method_call);
        self->player->GetPropertyAsync(
            fl_value_get_string(name_value), [method_call](int error, const std::string& value) {
              g_autoptr(FlMethodResponse) async_response = nullptr;
              if (error < 0 || value.empty()) {
                async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
              } else {
                async_response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_string(value.c_str())));
              }
              fl_method_call_respond(method_call, async_response, nullptr);
              g_object_unref(method_call);
            });
        return;  // Response sent asynchronously
      }
    }
  } else if (strcmp(method, "observeProperty") == 0) {
    if (!self->player || !self->initialized) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INITIALIZED", "Player not initialized", nullptr));
    } else {
      FlValue* name_value = fl_value_lookup_string(args, "name");
      FlValue* format_value = fl_value_lookup_string(args, "format");
      FlValue* id_value = fl_value_lookup_string(args, "id");

      if (name_value == nullptr || fl_value_get_type(name_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'name'", nullptr));
      } else if (format_value == nullptr || fl_value_get_type(format_value) != FL_VALUE_TYPE_STRING) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'format'", nullptr));
      } else if (id_value == nullptr || fl_value_get_type(id_value) != FL_VALUE_TYPE_INT) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'id'", nullptr));
      } else {
        self->player->ObserveProperty(
            fl_value_get_string(name_value), fl_value_get_string(format_value),
            static_cast<int>(fl_value_get_int(id_value)));
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      }
    }
  } else if (strcmp(method, "setVisible") == 0) {
    FlValue* visible_value = fl_value_lookup_string(args, "visible");

    if (visible_value == nullptr || fl_value_get_type(visible_value) != FL_VALUE_TYPE_BOOL) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing 'visible'", nullptr));
    } else {
      self->visible = fl_value_get_bool(visible_value);

      if (self->video_surface) {
        self->video_surface->SetVisible(self->visible);
        // Becoming visible has to render explicitly: the redraw latch was
        // consumed (or suppressed) while hidden, so no callback is pending.
        if (self->visible) render_video_plane(self, TRUE);
      } else if (self->visible && self->texture) {
        // Trigger a frame render when becoming visible
        mpv_texture_mark_frame_available(self->texture);
      }

      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
  } else if (strcmp(method, "isHDRSupported") == 0) {
    // The output half of the gate is what stops the app offering an HDR toggle
    // on an SDR panel, where enabling it only invites the compositor to tone-map
    // a plane that never needed to be PQ in the first place.
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(hdr_available(self))));
  } else if (strcmp(method, "setVideoRect") == 0) {
    if (!self->video_surface) {
      // Texture mode places video through the Flutter widget tree instead.
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    } else {
      auto read_int = [args](const char* key, int64_t* out) {
        FlValue* value = fl_value_lookup_string(args, key);
        if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_INT) return false;
        *out = fl_value_get_int(value);
        return true;
      };
      int64_t left = 0, top = 0, right = 0, bottom = 0;
      if (!read_int("left", &left) || !read_int("top", &top) || !read_int("right", &right) ||
          !read_int("bottom", &bottom)) {
        response =
            FL_METHOD_RESPONSE(fl_method_error_response_new("INVALID_ARGS", "Missing video rect bounds", nullptr));
      } else {
        FlValue* dpr_value = fl_value_lookup_string(args, "devicePixelRatio");
        double dpr = 1.0;
        if (dpr_value != nullptr && fl_value_get_type(dpr_value) == FL_VALUE_TYPE_FLOAT) {
          dpr = fl_value_get_float(dpr_value);
        }
        // GTK3 only ever reports integer scale factors, and the rect already
        // arrives in physical pixels, so the scale is purely how many buffer
        // pixels make up one surface-local unit.
        int32_t scale = static_cast<int32_t>(dpr + 0.5);
        if (scale < 1) scale = 1;
        self->video_surface->SetRect(
            static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<int32_t>(right - left),
            static_cast<int32_t>(bottom - top), scale);
        // Re-render at the new size straight away; waiting for the next mpv
        // frame would leave a stale buffer stretched across the new rect.
        render_video_plane(self, TRUE);
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      }
    }
  } else if (strcmp(method, "updateFrame") == 0) {
    if (self->visible && self->texture) {
      mpv_texture_mark_frame_available(self->texture);
    }
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "isInitialized") == 0) {
    gboolean initialized = self->player && self->initialized;
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(initialized)));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}
