#ifndef PLEZY_LINUX_MPV_HDR_METADATA_H_
#define PLEZY_LINUX_MPV_HDR_METADATA_H_

#include <cstdint>

// Source HDR10 static metadata, and the rules for turning it into a set of
// colour-management-v1 luminance requests the compositor will accept.
//
// This header is deliberately free of Wayland and GTK: the interesting logic is
// the validation, the penalty for getting it wrong is severe, and neither
// deserves a display server to test.

namespace mpv {

// The source's transfer function, so far as describing the plane cares. Every
// SDR curve collapses to kSdr: the plane is then left undescribed and mpv's
// normal output is already right, so there is nothing to distinguish.
enum class SourceTransfer { kSdr, kPq, kHlg };

// The source's container primaries. Only BT.2020 has a named counterpart worth
// describing for video; everything else is treated as "not a wide gamut" and
// leaves the plane undescribed.
enum class SourcePrimaries { kOther, kBt2020 };

// What the current source actually is, plus its HDR10 static metadata, as
// reported by mpv's video-params. A zero luminance field means the source did
// not carry it.
//
// The colorimetry fields matter as much as the luminances: describing a plane as
// PQ / BT.2020 because a *setting* is on, rather than because the stream is,
// tells the compositor to undo a transform that was never applied.
struct HdrMetadata {
  SourceTransfer transfer = SourceTransfer::kSdr;
  SourcePrimaries primaries = SourcePrimaries::kOther;
  uint32_t max_cll = 0;        // nits, maximum content light level
  uint32_t max_fall = 0;       // nits, maximum frame-average light level
  uint32_t max_luminance = 0;  // nits, mastering display maximum
  double min_luminance = 0.0;  // nits, mastering display minimum
};

// True when the source carries an HDR transfer function, i.e. when there is
// anything to pass through at all.
inline bool SourceIsHdr(const HdrMetadata& metadata) { return metadata.transfer != SourceTransfer::kSdr; }

// Who reduces the source's dynamic range to what the display can show.
//
// kCompositor is passthrough: the source's own metadata is declared and the
// compositor's tone curve does the work. Simplest, adapts to monitor changes
// with no re-render, and is what Kodi does — but its quality is entirely the
// compositor's, and a source that declares no metadata is assumed to reach the
// curve's maximum, which makes the roll-off far harsher than the content needs.
//
// kPlayer tone-maps in mpv to the display's real peak (learned from the
// compositor's preferred description) and then declares *that* peak, leaving the
// compositor an identity transform. This is mpv's own default behaviour and what
// the compositor developers recommend.
enum class HdrToneMapping { kCompositor, kPlayer };

// The primary colour volume maxima the protocol attaches to each named transfer
// function. These are not interchangeable: PQ's EOTF swings to 10000 cd/m²,
// while HLG is a *relative* signal whose absolute luminances are all defined
// against a 1000 cd/m² peak display. Getting this wrong is not cosmetic — an
// HLG stream declaring a 4000-nit MaxCLL with no mastering range passes a
// PQ-shaped check and then trips a fatal invalid_luminance at create().
constexpr uint32_t kPqMaxLuminanceNits = 10000;
constexpr uint32_t kHlgMaxLuminanceNits = 1000;

// The protocol carries the mastering minimum scaled by this to keep four
// decimals of a value that is normally a small fraction of a nit.
constexpr uint32_t kMinLuminanceScale = 10000;

// Both PQ and HLG declare the same primary colour volume *floor*, 0.005 cd/m²,
// already in the protocol's scaled units. Containment is two-sided: a mastering
// range reaching below this leaves the primary colour volume just as surely as
// one reaching above its maximum, and needs the same extended_target_volume
// feature. Sources routinely declare 0.0001 or nothing at all, so this is the
// common case rather than the exotic one.
constexpr uint32_t kPrimaryVolumeMinScaled = 50;

// The implied primary colour volume maximum for a transfer function. This is
// also the range light levels are bounded by when no mastering luminance is
// sent, because the protocol says an unset mastering range takes the primary
// colour volume's own range.
inline uint32_t PrimaryVolumeMaxNits(SourceTransfer transfer) {
  switch (transfer) {
    case SourceTransfer::kHlg:
      return kHlgMaxLuminanceNits;
    case SourceTransfer::kPq:
    case SourceTransfer::kSdr:
      break;
  }
  return kPqMaxLuminanceNits;
}

// What the compositor told us it can accept, which decides how much of the
// source's metadata may legally be forwarded.
struct CompositorLuminanceSupport {
  // feature.set_mastering_display_primaries. Without it, set_mastering_luminance
  // raises unsupported_feature.
  bool mastering = false;
  // feature.extended_target_volume. Without it, the mastering advertisement
  // only promises target volumes *fully contained* within the primary colour
  // volume; exceeding it is implementation-defined and may fail the description.
  bool extended_target_volume = false;
  // Bound wp_color_manager_v1 version. Version 1 additionally requires each
  // light level to sit inside the mastering range; version 2 dropped that.
  uint32_t interface_version = 1;
};

// Which luminance requests to actually emit. A false flag means the field is
// left unset so the compositor applies its own default, which is always safer
// than a value the protocol would reject.
struct HdrLuminancePlan {
  bool send_mastering = false;
  uint32_t mastering_min_scaled = 0;
  uint32_t mastering_max = 0;
  bool send_max_cll = false;
  uint32_t max_cll = 0;
  bool send_max_fall = false;
  uint32_t max_fall = 0;
};

// Converts a mastering minimum in nits to the protocol's scaled units.
inline uint32_t ScaleMinLuminance(double nits) {
  if (!(nits > 0.0)) return 0;
  const double scaled = nits * static_cast<double>(kMinLuminanceScale) + 0.5;
  if (scaled >= 4294967295.0) return 4294967295u;
  return static_cast<uint32_t>(scaled);
}

// True when `value_nits` sits inside the mastering range, which version 1
// spells as strictly greater than min L and less than or equal to max L. The
// comparison against the minimum happens in scaled units and in 64 bits, since
// a corrupt max-luma would otherwise overflow the multiply.
inline bool LuminanceInMasteringRange(uint32_t value_nits, uint32_t min_lum_scaled, uint32_t max_lum_nits) {
  if (value_nits > max_lum_nits) return false;
  return static_cast<uint64_t>(value_nits) * kMinLuminanceScale > min_lum_scaled;
}

// Decides which of set_mastering_luminance / set_max_cll / set_max_fall may be
// sent for `metadata`, given what the compositor advertised.
//
// Every constraint enforced here is a *protocol error* on create(), not a
// failed image description: the compositor disconnects the client, taking the
// whole app down rather than just HDR. Badly authored HDR content does violate
// these — a MaxCLL above the mastering display's own peak is common, and MaxFALL
// above MaxCLL happens — so the stream is never trusted.
inline HdrLuminancePlan PlanHdrLuminance(const HdrMetadata& metadata, const CompositorLuminanceSupport& support) {
  HdrLuminancePlan plan;

  // The ceiling everything is judged against, and it depends on the curve: 10000
  // nits for PQ, 1000 for HLG.
  const uint32_t volume_max = PrimaryVolumeMaxNits(metadata.transfer);

  // Mastering luminance carries two error cases: unsupported_feature unless the
  // compositor advertised set_mastering_display_primaries, and invalid_luminance
  // unless max L is strictly greater than min L.
  //
  // Beyond those, the mastering advertisement only promises target volumes
  // *fully contained* within the primary colour volume, and containment is
  // two-sided. Both ends are therefore clamped into it unless
  // extended_target_volume was advertised:
  //
  //  - The maximum down to the curve's own ceiling. For HLG that is also
  //    semantically right, since its absolute luminances are defined against a
  //    1000-nit display and a larger figure is outside the model. The clamp
  //    doubles as overflow protection for the scaled comparison below.
  //  - The minimum up to the 0.005-nit floor. Sources overwhelmingly declare
  //    0.0001 or nothing at all, both of which sit below it.
  //
  // Clamping rather than dropping matters: the mastering maximum is the
  // compositor's fallback peak when the source carries no MaxCLL, and dropping
  // it there would leave the compositor assuming the curve's full range —
  // exactly the over-compression this whole exercise is about avoiding.
  const uint32_t mastering_ceiling = support.extended_target_volume ? kPqMaxLuminanceNits : volume_max;
  const uint32_t mastering_floor_scaled = support.extended_target_volume ? 0 : kPrimaryVolumeMinScaled;
  uint32_t mastering_max = metadata.max_luminance;
  if (mastering_max > mastering_ceiling) mastering_max = mastering_ceiling;
  uint32_t mastering_min_scaled = ScaleMinLuminance(metadata.min_luminance);
  if (mastering_min_scaled < mastering_floor_scaled) mastering_min_scaled = mastering_floor_scaled;
  if (support.mastering && mastering_max > 0 &&
      static_cast<uint64_t>(mastering_max) * kMinLuminanceScale > mastering_min_scaled) {
    plan.send_mastering = true;
    plan.mastering_min_scaled = mastering_min_scaled;
    plan.mastering_max = mastering_max;
  }

  plan.send_max_cll = metadata.max_cll > 0;
  plan.max_cll = metadata.max_cll;
  plan.send_max_fall = metadata.max_fall > 0;
  plan.max_fall = metadata.max_fall;

  // The range both light levels must sit inside. With no mastering request the
  // primary colour volume applies, which is why volume_max is used and not PQ's
  // ceiling: an HLG stream is bounded at 1000 either way.
  const uint32_t range_max = plan.send_mastering ? plan.mastering_max : volume_max;
  const uint32_t range_min_scaled = plan.send_mastering ? plan.mastering_min_scaled : 0;

  // Version 1 requires both inside that range; version 2 dropped the
  // requirement but the curve still has no code point above its own volume
  // maximum. Drop the offending light level rather than the mastering range:
  // mastering metadata is the more trustworthy of the two, and dropping max_cll
  // leaves the compositor falling back to the mastering maximum, which is the
  // better answer anyway.
  if (support.interface_version < 2) {
    if (plan.send_max_cll && !LuminanceInMasteringRange(plan.max_cll, range_min_scaled, range_max)) {
      plan.send_max_cll = false;
    }
    if (plan.send_max_fall && !LuminanceInMasteringRange(plan.max_fall, range_min_scaled, range_max)) {
      plan.send_max_fall = false;
    }
  } else {
    if (plan.send_max_cll && plan.max_cll > volume_max) plan.send_max_cll = false;
    if (plan.send_max_fall && plan.max_fall > volume_max) plan.send_max_fall = false;
  }

  // Every version requires max_fall <= max_cll, but only while *both* are set,
  // so this has to be judged after the drops above. max_fall is the one to go:
  // it is the less trustworthy field and no compositor tone curve consults it.
  if (plan.send_max_cll && plan.send_max_fall && plan.max_fall > plan.max_cll) {
    plan.send_max_fall = false;
  }
  return plan;
}

// Rewrites the metadata to describe a signal *we* tone-mapped to `peak_nits`,
// rather than the source's original range.
//
// This is the whole point of player-side tone mapping: once mpv has mapped the
// content down to the display's peak, telling the compositor the source's
// original 4000- or 10000-nit range would have it compress a signal that no
// longer contains those levels. The curve and gamut are unchanged — the pixels
// are still PQ or HLG over BT.2020 — but every luminance now describes what we
// produced. The mastering floor is kept: it did not move.
inline HdrMetadata DescribeTonemappedTo(const HdrMetadata& source, uint32_t peak_nits) {
  HdrMetadata described = source;
  if (peak_nits == 0) return described;
  const uint32_t volume_max = PrimaryVolumeMaxNits(source.transfer);
  if (peak_nits > volume_max) peak_nits = volume_max;
  described.max_luminance = peak_nits;
  described.max_cll = peak_nits;
  // MaxFALL must stay at or below MaxCLL, and a frame average equal to the peak
  // would be a claim about the content we have not measured. The source's own
  // figure is kept when it still fits, since it remains the better estimate.
  described.max_fall = (source.max_fall > 0 && source.max_fall <= peak_nits) ? source.max_fall : 0;
  return described;
}

// Everything outside the source that bears on whether the plane carries HDR.
struct HdrInputs {
  bool allowed = false;              // the app's permission (the hdr-enabled setting)
  bool client_can_describe = false;  // 10-bit plane, colour-managed surface, advertised curve
  bool output_is_hdr = false;        // the compositor's preferred transfer function is PQ
  bool source_describable = false;   // this source's curve and gamut are both advertised
  HdrToneMapping requested = HdrToneMapping::kCompositor;
  uint32_t display_peak_nits = 0;  // from the preferred description; 0 means unknown
};

// What to do about it.
struct HdrDecision {
  bool describe = false;            // attach an image description at all
  bool tone_map_in_player = false;  // mpv reduces the range rather than the compositor
  // The peak mpv aims at *and* the peak declared to the compositor — deliberately
  // one number, because the two disagreeing is what makes a compositor remap a
  // signal twice. Zero means target-peak stays on auto.
  uint32_t target_peak_nits = 0;
};

// The single gate. Four independent conditions must hold before a plane is
// described as HDR, and they come from four different places: the user's
// setting, the compositor's advertised capabilities, the output's current state,
// and the file. Any one of them failing means falling back to mpv's ordinary
// tone-mapped SDR output, which is always safe.
//
// Player-side tone mapping additionally needs to know what to map *to*. Without
// a peak from the preferred description there is nothing to aim at, so it
// degrades to passthrough rather than inventing a target.
inline HdrDecision DecideHdr(const HdrInputs& inputs, const HdrMetadata& source) {
  HdrDecision decision;
  decision.describe = inputs.allowed && inputs.client_can_describe && inputs.output_is_hdr &&
                      inputs.source_describable && SourceIsHdr(source);
  if (!decision.describe) return decision;

  if (inputs.requested == HdrToneMapping::kPlayer && inputs.display_peak_nits > 0) {
    // Clamped to the curve's primary colour volume here rather than at the two
    // call sites, so the peak handed to mpv and the peak in the description are
    // the same number by construction.
    const uint32_t volume_max = PrimaryVolumeMaxNits(source.transfer);
    uint32_t peak = inputs.display_peak_nits;
    if (peak > volume_max) peak = volume_max;
    // mpv's target-peak option only accepts 10..10000; below that there is
    // nothing sensible to aim at.
    if (peak >= 10) {
      decision.tone_map_in_player = true;
      decision.target_peak_nits = peak;
    }
  }
  return decision;
}

}  // namespace mpv

#endif  // PLEZY_LINUX_MPV_HDR_METADATA_H_
