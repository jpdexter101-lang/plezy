import 'package:flutter_test/flutter_test.dart';

import 'package:plezy/media/media_backend.dart';
import 'package:plezy/media/media_hub.dart';
import 'package:plezy/media/media_item.dart';
import 'package:plezy/media/media_kind.dart';
import 'package:plezy/models/home_section_config.dart';
import 'package:plezy/utils/home_section_builder.dart';

MediaItem item(String id, MediaKind kind, {String? libraryId}) =>
    MediaItem(id: id, backend: MediaBackend.plex, kind: kind, serverId: 'server', libraryId: libraryId);

MediaHub hub(String id, String identifier, List<MediaItem> items) =>
    MediaHub(id: id, identifier: identifier, title: 'Recently Added', type: 'movie', items: items, size: items.length);

void main() {
  test('keeps server rows when no custom layout exists', () {
    final hubs = [
      hub('latest', 'recentlyAddedMovies', [item('m1', MediaKind.movie)]),
    ];
    expect(buildConfiguredHomeSections(sourceHubs: hubs, collections: const [], sections: const []), equals(hubs));
  });

  test('builds a recently added row from selected libraries', () {
    final result = buildConfiguredHomeSections(
      sourceHubs: [
        hub('latest', 'recentlyAddedMovies', [
          item('a', MediaKind.movie, libraryId: 'movies'),
          item('b', MediaKind.movie, libraryId: 'other'),
        ]),
      ],
      collections: const [],
      sections: const [
        HomeSectionConfig(
          id: 'r',
          title: 'My Recent Movies',
          kind: HomeSectionKind.recentlyAdded,
          libraryKeys: ['server:movies'],
        ),
      ],
    );
    expect(result.single.items.map((i) => i.id), ['a']);
  });

  test('shows only selected movie collections and excludes show collections', () {
    final result = buildConfiguredHomeSections(
      sourceHubs: const [],
      collections: [
        item('romcom', MediaKind.collection, libraryId: 'movies'),
        item('buddy', MediaKind.collection, libraryId: 'movies'),
        item('sitcom', MediaKind.collection, libraryId: 'shows'),
      ],
      collectionLibraryKinds: const {'server:movies': MediaKind.movie, 'server:shows': MediaKind.show},
      sections: const [
        HomeSectionConfig(
          id: 'c',
          title: 'Favorite Collections',
          kind: HomeSectionKind.movieCollections,
          libraryKeys: ['server:movies'],
          collectionKeys: ['server:romcom', 'server:buddy'],
        ),
      ],
    );
    expect(result.single.items.map((i) => i.id), ['romcom', 'buddy']);
    expect(result.single.items.every((i) => i.kind == MediaKind.collection), isTrue);
  });
}
