import '../media/media_hub.dart';
import '../media/media_item.dart';

/// Returns home hubs with the requested cross-library rows merged.
///
/// Backends are normalized into [MediaHub] before this utility is called, so
/// Plex, Jellyfin, and Emby follow the same grouping rules.
List<MediaHub> mergeHomeHubs(
  List<MediaHub> hubs, {
  bool mergeRecentlyAdded = false,
  bool mergeSameNameCollections = false,
}) {
  if (!mergeRecentlyAdded && !mergeSameNameCollections) return List.of(hubs);

  final groups = <String, _HubGroup>{};
  for (final hub in hubs) {
    final key = _groupKey(
      hub,
      mergeRecentlyAdded: mergeRecentlyAdded,
      mergeSameNameCollections: mergeSameNameCollections,
    );
    if (key == null) continue;
    final group = groups.putIfAbsent(key, () => _HubGroup(hub));
    if (identical(group.first, hub)) continue;
    group.items.addAll(hub.items);
    group.more = group.more || hub.more;
  }

  final output = <MediaHub>[];
  final emitted = <String>{};
  for (final hub in hubs) {
    final key = _groupKey(
      hub,
      mergeRecentlyAdded: mergeRecentlyAdded,
      mergeSameNameCollections: mergeSameNameCollections,
    );
    if (key == null) {
      output.add(hub);
    } else if (emitted.add(key)) {
      final group = groups[key]!;
      final items = _dedupeItems(group.items);
      output.add(
        MediaHub(
          id: group.first.id,
          identifier: group.first.identifier,
          title: group.first.title,
          type: items.map((item) => item.kind.name).toSet().length > 1
              ? 'mixed'
              : group.first.type,
          items: items,
          size: items.length,
          more: group.more,
          serverId: group.first.serverId,
          serverName: group.first.serverName,
        ),
      );
    }
  }
  return output;
}

class _HubGroup {
  _HubGroup(this.first) : items = List.of(first.items), more = first.more;
  final MediaHub first;
  final List<MediaItem> items;
  bool more;
}

String? _groupKey(
  MediaHub hub, {
  required bool mergeRecentlyAdded,
  required bool mergeSameNameCollections,
}) {
  if (mergeRecentlyAdded && _isRecentlyAdded(hub)) return 'recently-added';
  if (mergeSameNameCollections && _isCollectionHub(hub)) {
    final title = _normalized(hub.title);
    if (title.isNotEmpty) return 'collection:$title';
  }
  return null;
}

bool _isRecentlyAdded(MediaHub hub) {
  final values = [
    hub.identifier,
    hub.id,
    hub.title,
  ].whereType<String>().map(_normalized);
  return values.any(
    (value) =>
        value.contains('recentlyadded') ||
        value == 'latest' ||
        value.contains('latestin'),
  );
}

bool _isCollectionHub(MediaHub hub) {
  final values = [
    hub.identifier,
    hub.id,
    hub.type,
  ].whereType<String>().map(_normalized);
  return values.any((value) => value.contains('collection'));
}

String _normalized(String value) =>
    value.toLowerCase().replaceAll(RegExp(r'[^a-z0-9]+'), '');

List<MediaItem> _dedupeItems(List<MediaItem> items) {
  final seen = <String>{};
  return items
      .where((item) => seen.add(item.globalKey))
      .toList(growable: false);
}
