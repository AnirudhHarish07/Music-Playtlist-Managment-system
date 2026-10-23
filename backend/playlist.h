/*
 * playlist.h - Doubly linked list playlist (data structure module)
 *
 * This module knows nothing about HTTP or JSON.  It can be compiled and
 * tested on its own (see test_playlist.c).
 *
 * Layout of the list (a -> b -> c):
 *
 *        head                          tail
 *         |                             |
 *         v                             v
 *   NULL <- [a] <-> [b] <-> [c] -> NULL
 *            prev/next pointers
 *
 *   - head->prev == NULL and tail->next == NULL
 *   - every node n satisfies  n->next->prev == n  and  n->prev->next == n
 *   - count always equals the number of nodes
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stddef.h>

#define PL_TITLE_MAX  100
#define PL_ARTIST_MAX 100
#define PL_URI_MAX    100
#define PL_IMAGE_MAX  300
#define PL_SOURCE_MAX 10
#define PL_AUDIO_MAX  300

/* One song = one node of the doubly linked list. */
typedef struct Song {
    int  id;
    char title[PL_TITLE_MAX];
    char artist[PL_ARTIST_MAX];
    int  duration;              /* seconds */
    char spotify_uri[PL_URI_MAX];   /* "spotify:track:<id>" ("" for local)   */
    char image_url[PL_IMAGE_MAX];   /* album art URL, may be ""              */
    char source[PL_SOURCE_MAX];     /* "spotify" or "local"                  */
    char audio_url[PL_AUDIO_MAX];   /* "/music/<file>.mp3" for local, or ""  */
    struct Song *prev;
    struct Song *next;
} Song;

/* The list header: everything needed to reach and manage the nodes. */
typedef struct Playlist {
    Song *head;                 /* first song, NULL when empty            */
    Song *tail;                 /* last song,  NULL when empty            */
    Song *current;              /* "now playing" song, NULL if none       */
    int   count;                /* number of songs                        */
    int   nextId;               /* id handed to the next created song     */
    int   repeat;               /* 1 = wrap around at either end          */
} Playlist;

/* Result codes returned by the operations below. */
typedef enum {
    PL_OK = 0,
    PL_ERR_INVALID,             /* bad title/artist/duration/argument     */
    PL_ERR_DUPLICATE,           /* a song with that title already exists  */
    PL_ERR_RANGE,               /* position outside 1..count+1            */
    PL_ERR_NOT_FOUND,           /* no such song                           */
    PL_ERR_EMPTY,               /* operation needs at least one song      */
    PL_ERR_END,                 /* next/prev hit an end with repeat off   */
    PL_ERR_NOMEM,               /* malloc failed                          */
    PL_ERR_IO                   /* file could not be read/written         */
} PlResult;

typedef enum { PL_SORT_TITLE, PL_SORT_ARTIST, PL_SORT_DURATION } PlSortKey;

/* Human readable text for a PlResult (for error messages). */
const char *pl_strerror(PlResult r);

/*
 * Everything needed to create a song.  NULL / "" fields mean "not set".
 * source may be NULL: it is then "spotify" when spotify_uri is given and
 * "local" otherwise.  Rules enforced when a song is created:
 *   spotify: spotify_uri must look like spotify:track:<22 chars>; no audio_url;
 *            duplicates are detected by spotify_uri
 *   local:   no spotify_uri; audio_url (optional) must be /music/<name>;
 *            duplicates are detected by title (case-insensitive)
 *   image_url (optional) must start with https://
 */
typedef struct {
    const char *title;
    const char *artist;
    int         duration;       /* seconds */
    const char *spotify_uri;
    const char *image_url;
    const char *source;
    const char *audio_url;
} SongInfo;

/* Lifecycle ---------------------------------------------------------- */
void pl_init(Playlist *pl);
void pl_free(Playlist *pl);                 /* frees every node, O(n)   */

/* Insertion  (new song id is written to *out_id when out_id != NULL) -- */
PlResult pl_add_end  (Playlist *pl, const char *title, const char *artist,
                      int duration, int *out_id);               /* O(1) */
PlResult pl_add_start(Playlist *pl, const char *title, const char *artist,
                      int duration, int *out_id);               /* O(1) */
PlResult pl_insert_at(Playlist *pl, int pos, const char *title,
                      const char *artist, int duration,
                      int *out_id);                             /* O(n) */

/* Same three insertions with the full set of song fields. */
PlResult pl_add_end_info  (Playlist *pl, const SongInfo *info, int *out_id);
PlResult pl_add_start_info(Playlist *pl, const SongInfo *info, int *out_id);
PlResult pl_insert_at_info(Playlist *pl, int pos, const SongInfo *info,
                           int *out_id);

/* Deletion ----------------------------------------------------------- */
PlResult pl_delete_by_id   (Playlist *pl, int id);              /* O(n) */
PlResult pl_delete_by_title(Playlist *pl, const char *title);   /* O(n) */

/* Lookup / traversal ------------------------------------------------- */
Song *pl_find_by_id   (const Playlist *pl, int id);             /* O(n) */
Song *pl_find_by_title(const Playlist *pl, const char *title);  /* O(n) */
Song *pl_find_by_uri  (const Playlist *pl, const char *uri);    /* O(n) */
int   pl_total_duration(const Playlist *pl);                    /* O(n) */

/* Calls cb(song, ctx) for every song whose title or artist contains q
 * (case-insensitive).  Returns the number of matches.            O(n*m) */
typedef void (*PlVisitFn)(const Song *song, void *ctx);
int pl_search(const Playlist *pl, const char *q, PlVisitFn cb, void *ctx);

/* Player ------------------------------------------------------------- */
PlResult pl_play_next(Playlist *pl);                            /* O(1) */
PlResult pl_play_prev(Playlist *pl);                            /* O(1) */
PlResult pl_play_id  (Playlist *pl, int id);                    /* O(n) */
int      pl_toggle_repeat(Playlist *pl);    /* returns the new setting  */

/* Reordering (nodes are relinked, never copied) ---------------------- */
void pl_reverse(Playlist *pl);                                  /* O(n) */
void pl_sort   (Playlist *pl, PlSortKey key);              /* O(n log n) */
PlResult pl_shuffle(Playlist *pl);                              /* O(n) */

/* Persistence: one tab-separated line per song:
 *   source TAB title TAB artist TAB duration TAB uri TAB image TAB audio
 * (old "title|artist|duration" lines are still understood on load) ----- */
PlResult pl_save(const Playlist *pl, const char *path);
PlResult pl_load(Playlist *pl, const char *path);  /* replaces contents */

#endif /* PLAYLIST_H */
