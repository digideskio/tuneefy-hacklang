<?hh // strict

namespace tuneefy\Platform\Platforms;

use tuneefy\Platform\Platform,
    tuneefy\Platform\PlatformResult,
    tuneefy\Platform\WebStreamingPlatformInterface,
    tuneefy\MusicalEntity\MusicalEntity,
    tuneefy\MusicalEntity\Entities\TrackEntity,
    tuneefy\MusicalEntity\Entities\AlbumEntity,
    tuneefy\Utils\Utils;

class SpotifyPlatform extends Platform implements WebStreamingPlatformInterface
{
  
  const string NAME = "Spotify";
  const string TAG = "spotify";
  const string COLOR = "4DA400";

  const string API_ENDPOINT = "https://api.spotify.com/v1/";
  const string API_METHOD = Platform::METHOD_GET;

  protected ImmMap<int,?string> $endpoints = ImmMap {
    Platform::LOOKUP_TRACK  => self::API_ENDPOINT . "tracks/%s",
    Platform::LOOKUP_ALBUM  => self::API_ENDPOINT . "albums/%s",
    Platform::LOOKUP_ARTIST => self::API_ENDPOINT . "artists/%s",
    Platform::SEARCH_TRACK  => self::API_ENDPOINT . "search",
    Platform::SEARCH_ALBUM  => self::API_ENDPOINT . "search",
   // Platform::SEARCH_ARTIST => self::API_ENDPOINT . "search" 
  };
  protected ImmMap<int,?string> $terms = ImmMap {
    Platform::LOOKUP_TRACK  => null,
    Platform::LOOKUP_ALBUM  => null,
    Platform::LOOKUP_ARTIST => null,
    Platform::SEARCH_TRACK  => "q",
    Platform::SEARCH_ALBUM  => "q",
   // Platform::SEARCH_ARTIST => "q" 
  };
  protected ImmMap<int,Map<string,mixed>> $options = ImmMap {
    Platform::LOOKUP_TRACK  => Map {},
    Platform::LOOKUP_ALBUM  => Map {},
    Platform::LOOKUP_ARTIST => Map {},
    Platform::SEARCH_TRACK  => Map { "type" => "track", "limit" => Platform::LIMIT },
    Platform::SEARCH_ALBUM  => Map { "type" => "album", "limit" => Platform::LIMIT },
   // Platform::SEARCH_ARTIST => Map { "type" => "artist", "limit" => Platform::LIMIT } 
  };

  // http://open.spotify.com/track/5jhJur5n4fasblLSCOcrTp
  const string REGEX_SPOTIFY_ALL = "/(?P<type>artist|album|track)(:|\/)(?P<item_id>[a-zA-Z0-9]*)[\/]?$/";
    private ImmMap<string, int> $lookup_type_correspondance = ImmMap {
      'track' => Platform::LOOKUP_TRACK,
      'album' => Platform::LOOKUP_ALBUM,
      'artist' => Platform::LOOKUP_ARTIST,
    };

  // LOCAL files : http://open.spotify.com/local/hang+the+bastard/raw+sorcery/doomed+fucking+doomed/206
  const string REGEX_SPOTIFY_LOCAL = "/local\/(?P<artist_name>".Platform::REGEX_FULLSTRING.")\/(?P<album_name>".Platform::REGEX_FULLSTRING.")\/(?P<track_name>".Platform::REGEX_FULLSTRING.")\/[0-9]+$/";
    

  public function hasPermalink(string $permalink): bool
  {
    return (strpos($permalink, "spotify:") !== false || strpos($permalink, "open.spotify.") !== false || strpos($permalink, "play.spotify.") !== false);
  }

  public function expandPermalink(string $permalink, int $mode): ?PlatformResult
  {

    $musical_entity = null;
    $query_words = Vector {$permalink};

    $match = Map {};

    if (preg_match(self::REGEX_SPOTIFY_ALL, $permalink, $match)) {
      // We have a nicely formatted share url
      
      $object_type = $this->lookup_type_correspondance[$match['type']];
      $response = $this->fetchSync($object_type, $match['item_id']);

      if ($response === null || property_exists($response->data, 'error')) { return null; }
      $entity = $response->data;

      if ($object_type === Platform::LOOKUP_TRACK) {

        $musical_entity = new TrackEntity($entity->name, new AlbumEntity($entity->album->name, $entity->artists[0]->name, $entity->album->images[1]->url)); 
        $musical_entity->addLink(static::TAG, $entity->external_urls->spotify);

        $query_words = Vector {$entity->artists[0]->name, $entity->name};
      
      } else if ($object_type === Platform::LOOKUP_ALBUM) {
      
        $musical_entity = new AlbumEntity($entity->name, $entity->artists[0]->name, $entity->images[1]->url);
        $musical_entity->addLink(static::TAG, $entity->external_urls->spotify);

        $query_words = Vector {$entity->artists[0]->name, $entity->name};
      
      } else if ($object_type === Platform::LOOKUP_ARTIST) {

        $query_words = Vector {$entity->name};
        
      }
        
    } else if (preg_match(self::REGEX_SPOTIFY_LOCAL, $permalink, $match)) {
      // We have a nicely formatted local url, but can only retrieve query words
      $query_words = Vector {$match['artist_name'], $match['track_name']};
    }
    
    // Consolidate results
    $metadata = Map {"query_words" => $query_words};

    if ($musical_entity !== null) {
      $metadata->add(Pair {"platform", $this->getName()});
    }

    return new PlatformResult($metadata, $musical_entity);

  }

  public async function search(int $type, string $query, int $limit, int $mode): Awaitable<?Vector<PlatformResult>>
  {
    $response = await $this->fetch($type, $query);

    if ($response === null) {
      return null;
    }
    $entities = $response->data;

    switch ($type) {
      case Platform::SEARCH_TRACK:
        $results = $entities->tracks->items;
        break;
      case Platform::SEARCH_ALBUM:
        $results = $entities->albums->items;
        break;
    }
    $length = min(count($results), $limit?$limit:Platform::LIMIT);
    
    if ($length === 0) {
      return null;
    }
    $musical_entities = Vector {};
    
    // Tracks bear a popularity score
    // that we're using to rate the results
    $max_track_popularity = 1;
    if ($type === Platform::SEARCH_TRACK) {
      $max_track_popularity = max(intval($results[0]->popularity),1);
    }

    for($i=0; $i<$length; $i++){
    
      $current_item = $results[$i];
     
      if ($type === Platform::SEARCH_TRACK) {
      
        $musical_entity = new TrackEntity($current_item->name, new AlbumEntity($current_item->album->name, $current_item->artists[0]->name, $current_item->album->images[1]->url)); 
        $musical_entity->addLink(static::TAG, $current_item->external_urls->spotify);

        $musical_entities->add(new PlatformResult(Map {"score" => round($current_item->popularity/$max_track_popularity,2)}, $musical_entity));

      } else /*if ($type === Platform::SEARCH_ALBUM)*/ {
            
        // The search/?type=album endpoint only returns a simplified album object, 
        // not including the artist. Either we blank the artist, or we make an extra
        // api call to $current_item->href. This is what EAGER mode is here for.
        $artist = "";
        if ($mode === Platform::MODE_EAGER) {
          // We need to fetch the artist of the album
          $album_response = await $this->fetch(Platform::LOOKUP_ALBUM, $current_item->id);
          if ($album_response !== null && !property_exists($album_response, 'error')) {
            $artist = $album_response->artists[0]->name;
          }
        }

        $musical_entity = new AlbumEntity($current_item->name, $artist, $current_item->images[1]->url); 
        $musical_entity->addLink(static::TAG, $current_item->external_urls->spotify);

        $musical_entities->add(new PlatformResult(Map {"score" => Utils::indexScore($i)}, $musical_entity));

      }
      
    }
    
    return $musical_entities;
  }
}