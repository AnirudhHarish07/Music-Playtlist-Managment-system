/*
 * playlist.c - all doubly linked list operations.
 *
 * Conventions used in the pointer diagrams below:
 *     [x]  a node          <->  the prev/next pair between two nodes
 *     NULL marks the end of the list on either side
 */
#include "playlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MAX_DURATION 35999   /* just under 10 hours, in seconds */

/* ====================================================================
 * Small helpers
 * ==================================================================== */

const char *pl_strerror(PlResult r)
{
    switch (r) {
    case PL_OK:           return "ok";
    case PL_ERR_INVALID:  return "invalid input";
    case PL_ERR_DUPLICATE:return "a song with that title already exists";
    case PL_ERR_RANGE:    return "position out of range";
    case PL_ERR_NOT_FOUND:return "song not found";
    case PL_ERR_EMPTY:    return "playlist is empty";
    case PL_ERR_END:      return "reached the end of the playlist (repeat is off)";
    case PL_ERR_NOMEM:    return "out of memory";
    case PL_ERR_IO:       return "file error";
    }
    return "unknown error";
}

/* Case-insensitive comparison (strcasecmp is not part of C99). */
static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Case-insensitive "does hay contain needle?" (empty needle matches). */
static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return 1;
    }
    return 0;
}

/*
 * Copy src into dst (size cap), trimming surrounding whitespace.
 * Fails when the text is empty, too long, or contains control characters
 * or '|' (the separator of the save file format).
 */
static int clean_text(char *dst, size_t cap, const char *src)
{
    size_t len;
    if (!src) return 0;
    while (isspace((unsigned char)*src)) src++;
    len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    if (len == 0 || len >= cap) return 0;
    for (size_t i = 0; i < len; i++)
        if (iscntrl((unsigned char)src[i]) || src[i] == '|') return 0;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

/*
 * Validate the input and allocate a fully initialised, UNLINKED node.
 * The node gets prev = next = NULL; the caller links it into the list.
 * The id counter is only advanced by the caller after linking succeeds.
 */
static PlResult make_song(Playlist *pl, const char *title, const char *artist,
                          int duration, Song **out)
{
    Song *s;
    char t[PL_TITLE_MAX], a[PL_ARTIST_MAX];

    if (!clean_text(t, sizeof t, title) || !clean_text(a, sizeof a, artist))
        return PL_ERR_INVALID;
    if (duration < 1 || duration > PL_MAX_DURATION)
        return PL_ERR_INVALID;
    if (pl_find_by_title(pl, t) != NULL)
        return PL_ERR_DUPLICATE;

    s = malloc(sizeof *s);
    if (!s) return PL_ERR_NOMEM;
    s->id = pl->nextId;
    strcpy(s->title, t);
    strcpy(s->artist, a);
    s->duration = duration;
    s->prev = s->next = NULL;
    *out = s;
    return PL_OK;
}

/* Bookkeeping shared by every insertion once the node is linked. */
static void after_link(Playlist *pl, Song *s, int *out_id)
{
    pl->nextId++;
    pl->count++;
    if (!pl->current) pl->current = s;      /* first song starts as "now playing" */
    if (out_id) *out_id = s->id;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

void pl_init(Playlist *pl)
{
    pl->head = pl->tail = pl->current = NULL;
    pl->count  = 0;
    pl->nextId = 1;
    pl->repeat = 0;
}

/* Walk the list once, saving next BEFORE freeing the node. */
void pl_free(Playlist *pl)
{
    Song *s = pl->head;
    while (s) {
        Song *next = s->next;
        free(s);
        s = next;
    }
    pl->head = pl->tail = pl->current = NULL;
    pl->count = 0;
}

/* ====================================================================
 * Insertion
 * ==================================================================== */

/*
 * Add at the END (O(1) thanks to the tail pointer).
 *
 * Before:   head -> [a] <-> [b] <- tail
 * After:    head -> [a] <-> [b] <-> [new] <- tail
 *
 * Pointer changes:
 *   new->prev  = old tail
 *   old tail->next = new
 *   tail       = new
 * Empty list: head and tail both become new.
 */
PlResult pl_add_end(Playlist *pl, const char *title, const char *artist,
                    int duration, int *out_id)
{
    Song *s;
    PlResult r = make_song(pl, title, artist, duration, &s);
    if (r != PL_OK) return r;

    if (pl->tail) {
        s->prev = pl->tail;         /* new points back to the old last node */
        pl->tail->next = s;         /* old last node points forward to new  */
        pl->tail = s;               /* new is now the last node             */
    } else {
        pl->head = pl->tail = s;    /* first node: it is both ends          */
    }
    after_link(pl, s, out_id);
    return PL_OK;
}

/*
 * Add at the BEGINNING (O(1)).
 *
 * Before:   head -> [a] <-> [b] <- tail
 * After:    head -> [new] <-> [a] <-> [b] <- tail
 *
 * Pointer changes:
 *   new->next = old head
 *   old head->prev = new
 *   head      = new
 */
PlResult pl_add_start(Playlist *pl, const char *title, const char *artist,
                      int duration, int *out_id)
{
    Song *s;
    PlResult r = make_song(pl, title, artist, duration, &s);
    if (r != PL_OK) return r;

    if (pl->head) {
        s->next = pl->head;         /* new points forward to the old first  */
        pl->head->prev = s;         /* old first points back to new         */
        pl->head = s;
    } else {
        pl->head = pl->tail = s;
    }
    after_link(pl, s, out_id);
    return PL_OK;
}

/*
 * Insert so that the new song ends up at 1-based position `pos`.
 * Valid range is 1 .. count+1 (count+1 means "append").  O(n) to walk.
 *
 * General case (1 < pos <= count): `at` is the node currently at pos.
 *
 * Before:   [p] <-> [at]            where p = at->prev
 * After:    [p] <-> [new] <-> [at]
 *
 * Four pointers change (order matters, set new's pointers first so we
 * never lose the neighbours):
 *   1. new->prev = p
 *   2. new->next = at
 *   3. p->next   = new
 *   4. at->prev  = new
 */
PlResult pl_insert_at(Playlist *pl, int pos, const char *title,
                      const char *artist, int duration, int *out_id)
{
    Song *s, *at;
    PlResult r;

    if (pos < 1 || pos > pl->count + 1) return PL_ERR_RANGE;
    if (pos == 1)             return pl_add_start(pl, title, artist, duration, out_id);
    if (pos == pl->count + 1) return pl_add_end  (pl, title, artist, duration, out_id);

    r = make_song(pl, title, artist, duration, &s);
    if (r != PL_OK) return r;

    at = pl->head;                          /* walk to the node at `pos` */
    for (int i = 1; i < pos; i++) at = at->next;

    s->prev = at->prev;                     /* 1 */
    s->next = at;                           /* 2 */
    at->prev->next = s;                     /* 3 (at->prev exists: pos > 1) */
    at->prev = s;                           /* 4 */

    after_link(pl, s, out_id);
    return PL_OK;
}

/* ====================================================================
 * Lookup
 * ==================================================================== */

Song *pl_find_by_id(const Playlist *pl, int id)
{
    for (Song *s = pl->head; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

Song *pl_find_by_title(const Playlist *pl, const char *title)
{
    for (Song *s = pl->head; s; s = s->next)
        if (ci_cmp(s->title, title) == 0) return s;
    return NULL;
}

int pl_total_duration(const Playlist *pl)
{
    int total = 0;
    for (const Song *s = pl->head; s; s = s->next) total += s->duration;
    return total;
}

int pl_search(const Playlist *pl, const char *q, PlVisitFn cb, void *ctx)
{
    int n = 0;
    for (const Song *s = pl->head; s; s = s->next) {
        if (ci_contains(s->title, q) || ci_contains(s->artist, q)) {
            cb(s, ctx);
            n++;
        }
    }
    return n;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

/*
 * Unlink `s` from the list and free it.
 *
 * Before:   [p] <-> [s] <-> [n]          (p and/or n may be NULL)
 * After:    [p] <-> [n]
 *
 * Pointer changes:
 *   - if s has a predecessor:  p->next = n        else head = n  (s was head)
 *   - if s has a successor:    n->prev = p        else tail = p  (s was tail)
 * A single-song list works too: both branches fall to head = tail = NULL.
 *
 * If s is the song now playing, "current" moves to its successor, or to
 * its predecessor when s was the tail (NULL if the list becomes empty).
 */
static void unlink_and_free(Playlist *pl, Song *s)
{
    if (s->prev) s->prev->next = s->next;
    else         pl->head      = s->next;

    if (s->next) s->next->prev = s->prev;
    else         pl->tail      = s->prev;

    if (pl->current == s)
        pl->current = s->next ? s->next : s->prev;

    free(s);
    pl->count--;
}

PlResult pl_delete_by_id(Playlist *pl, int id)
{
    Song *s = pl_find_by_id(pl, id);
    if (!s) return PL_ERR_NOT_FOUND;
    unlink_and_free(pl, s);
    return PL_OK;
}

PlResult pl_delete_by_title(Playlist *pl, const char *title)
{
    Song *s;
    if (!title) return PL_ERR_INVALID;
    s = pl_find_by_title(pl, title);
    if (!s) return PL_ERR_NOT_FOUND;
    unlink_and_free(pl, s);
    return PL_OK;
}

/* ====================================================================
 * Player
 * ==================================================================== */

/* Move "now playing" forward.  Wraps to head only when repeat is on. */
PlResult pl_play_next(Playlist *pl)
{
    if (!pl->head) return PL_ERR_EMPTY;
    if (!pl->current)            pl->current = pl->head;
    else if (pl->current->next)  pl->current = pl->current->next;
    else if (pl->repeat)         pl->current = pl->head;     /* wrap around */
    else                         return PL_ERR_END;
    return PL_OK;
}

/* Move "now playing" backward.  Wraps to tail only when repeat is on. */
PlResult pl_play_prev(Playlist *pl)
{
    if (!pl->head) return PL_ERR_EMPTY;
    if (!pl->current)            pl->current = pl->head;
    else if (pl->current->prev)  pl->current = pl->current->prev;
    else if (pl->repeat)         pl->current = pl->tail;     /* wrap around */
    else                         return PL_ERR_END;
    return PL_OK;
}

PlResult pl_play_id(Playlist *pl, int id)
{
    Song *s = pl_find_by_id(pl, id);
    if (!s) return PL_ERR_NOT_FOUND;
    pl->current = s;
    return PL_OK;
}

int pl_toggle_repeat(Playlist *pl)
{
    pl->repeat = !pl->repeat;
    return pl->repeat;
}

/* ====================================================================
 * Reordering
 * ==================================================================== */

/*
 * Reverse in place by swapping the two pointers of every node.
 *
 * Before:   head -> [a] <-> [b] <-> [c] <- tail
 *
 * For each node we swap prev and next:
 *     a: prev=NULL,next=b  ->  prev=b,   next=NULL
 *     b: prev=a,   next=c  ->  prev=c,   next=a
 *     c: prev=b,   next=NULL -> prev=NULL, next=b
 * Finally swap head and tail:
 *     head -> [c] <-> [b] <-> [a] <- tail
 *
 * After swapping, the old `next` is stored in `prev`, so the walk
 * continues through s->prev.  No node is allocated or freed.
 */
void pl_reverse(Playlist *pl)
{
    Song *s = pl->head;
    while (s) {
        Song *old_next = s->next;
        s->next = s->prev;
        s->prev = old_next;
        s = old_next;               /* continue to the original next node */
    }
    Song *tmp = pl->head;
    pl->head = pl->tail;
    pl->tail = tmp;
}

/* Comparison of two songs by the chosen key; ties keep original order. */
static int song_cmp(const Song *a, const Song *b, PlSortKey key)
{
    switch (key) {
    case PL_SORT_TITLE:    return ci_cmp(a->title,  b->title);
    case PL_SORT_ARTIST:   return ci_cmp(a->artist, b->artist);
    case PL_SORT_DURATION: return a->duration - b->duration;
    }
    return 0;
}

/* Merge two sorted singly-chained runs (following ->next only). Stable. */
static Song *merge_runs(Song *a, Song *b, PlSortKey key)
{
    Song dummy, *tail = &dummy;
    while (a && b) {
        if (song_cmp(b, a, key) < 0) { tail->next = b; b = b->next; }
        else                         { tail->next = a; a = a->next; }
        tail = tail->next;
    }
    tail->next = a ? a : b;
    return dummy.next;
}

/* Merge sort on the ->next chain.  Splits with slow/fast pointers. */
static Song *merge_sort(Song *h, PlSortKey key)
{
    Song *slow, *fast, *second;
    if (!h || !h->next) return h;

    slow = h; fast = h->next;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    second = slow->next;
    slow->next = NULL;              /* cut the list into two halves */

    return merge_runs(merge_sort(h, key), merge_sort(second, key), key);
}

/*
 * Sort by relinking.  Merge sort only touches ->next, so afterwards one
 * pass repairs every ->prev pointer and finds the new tail.
 */
void pl_sort(Playlist *pl, PlSortKey key)
{
    Song *prev = NULL;
    pl->head = merge_sort(pl->head, key);
    for (Song *s = pl->head; s; s = s->next) {
        s->prev = prev;
        prev = s;
    }
    pl->tail = prev;
}

/*
 * Fisher-Yates shuffle.  A linked list has no random access, so the node
 * POINTERS are copied into a temporary array, shuffled, then used to
 * relink the same nodes.  The array is freed before returning; songs are
 * never stored in it permanently.
 *
 *   for i = n-1 down to 1:  j = random in [0, i];  swap(a[i], a[j])
 */
PlResult pl_shuffle(Playlist *pl)
{
    Song **a;
    int n = pl->count, i = 0;

    if (n < 2) return PL_OK;
    a = malloc((size_t)n * sizeof *a);
    if (!a) return PL_ERR_NOMEM;

    for (Song *s = pl->head; s; s = s->next) a[i++] = s;

    for (i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Song *t = a[i]; a[i] = a[j]; a[j] = t;
    }

    for (i = 0; i < n; i++) {       /* relink in the shuffled order */
        a[i]->prev = (i > 0)     ? a[i - 1] : NULL;
        a[i]->next = (i < n - 1) ? a[i + 1] : NULL;
    }
    pl->head = a[0];
    pl->tail = a[n - 1];
    free(a);
    return PL_OK;
}

/* ====================================================================
 * Persistence
 * ==================================================================== */

PlResult pl_save(const Playlist *pl, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return PL_ERR_IO;
    for (const Song *s = pl->head; s; s = s->next)
        fprintf(f, "%s|%s|%d\n", s->title, s->artist, s->duration);
    return fclose(f) == 0 ? PL_OK : PL_ERR_IO;
}

/*
 * Load into a temporary playlist first; only when the whole file has been
 * read is the old content freed and replaced.  Malformed lines and
 * duplicate titles are skipped.  Ids are reassigned, repeat is kept.
 */
PlResult pl_load(Playlist *pl, const char *path)
{
    Playlist tmp;
    char line[2 * PL_TITLE_MAX + 64];
    FILE *f = fopen(path, "r");
    if (!f) return PL_ERR_IO;

    pl_init(&tmp);
    while (fgets(line, sizeof line, f)) {
        char *sep1, *sep2, *end;
        long dur;
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            int c;                              /* over-long line: skip it */
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        sep1 = strchr(line, '|');
        sep2 = sep1 ? strchr(sep1 + 1, '|') : NULL;
        if (!sep1 || !sep2) continue;
        *sep1 = '\0';
        *sep2 = '\0';
        dur = strtol(sep2 + 1, &end, 10);
        if (*end != '\0' || end == sep2 + 1) continue;
        if (dur < 1 || dur > PL_MAX_DURATION) continue;

        if (pl_add_end(&tmp, line, sep1 + 1, (int)dur, NULL) == PL_ERR_NOMEM) {
            pl_free(&tmp);
            fclose(f);
            return PL_ERR_NOMEM;
        }
    }
    fclose(f);

    tmp.repeat = pl->repeat;
    tmp.current = tmp.head;
    pl_free(pl);
    *pl = tmp;
    return PL_OK;
}
