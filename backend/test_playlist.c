/* Standalone tests for the linked list (no HTTP involved). */
#include "playlist.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Verify every structural invariant of the doubly linked list. */
static void check(const Playlist *pl)
{
    int n = 0;
    const Song *prev = NULL;
    for (const Song *s = pl->head; s; s = s->next) {
        assert(s->prev == prev);
        prev = s;
        n++;
    }
    assert(prev == pl->tail);
    assert(n == pl->count);
    if (pl->count == 0) assert(!pl->head && !pl->tail && !pl->current);
    else assert(pl->head->prev == NULL && pl->tail->next == NULL);
    if (pl->current) assert(pl_find_by_id(pl, pl->current->id) == pl->current);
    /* walking backwards must visit the same number of nodes */
    n = 0;
    for (const Song *s = pl->tail; s; s = s->prev) n++;
    assert(n == pl->count);
}

static void titles(const Playlist *pl, char *out)
{
    out[0] = 0;
    for (const Song *s = pl->head; s; s = s->next) { strcat(out, s->title); strcat(out, ","); }
}

static void count_cb(const Song *s, void *ctx) { (void)s; (*(int *)ctx)++; }

int main(void)
{
    Playlist pl; char buf[512]; int id = 0, hits;
    pl_init(&pl); check(&pl);

    /* empty-list behaviour */
    assert(pl_play_next(&pl) == PL_ERR_EMPTY);
    assert(pl_play_prev(&pl) == PL_ERR_EMPTY);
    assert(pl_delete_by_id(&pl, 1) == PL_ERR_NOT_FOUND);
    pl_reverse(&pl); pl_sort(&pl, PL_SORT_TITLE); assert(pl_shuffle(&pl) == PL_OK); check(&pl);

    /* adds */
    assert(pl_add_end(&pl, "B", "x", 60, &id) == PL_OK && id == 1);
    assert(pl.current && pl.current->id == 1);
    assert(pl_add_end(&pl, "C", "y", 90, NULL) == PL_OK);
    assert(pl_add_start(&pl, "A", "z", 30, NULL) == PL_OK);
    titles(&pl, buf); assert(!strcmp(buf, "A,B,C,")); check(&pl);

    /* insert: middle, position 1, count+1, out of range */
    assert(pl_insert_at(&pl, 2, "M", "m", 10, NULL) == PL_OK);
    titles(&pl, buf); assert(!strcmp(buf, "A,M,B,C,"));
    assert(pl_insert_at(&pl, 1, "F", "f", 10, NULL) == PL_OK);
    assert(pl_insert_at(&pl, 6, "L", "l", 10, NULL) == PL_OK);
    titles(&pl, buf); assert(!strcmp(buf, "F,A,M,B,C,L,")); check(&pl);
    assert(pl_insert_at(&pl, 0, "Q", "q", 10, NULL) == PL_ERR_RANGE);
    assert(pl_insert_at(&pl, 8, "Q", "q", 10, NULL) == PL_ERR_RANGE);
    assert(pl.count == 6);

    /* validation */
    assert(pl_add_end(&pl, "a", "dup", 10, NULL) == PL_ERR_DUPLICATE);   /* case-insensitive */
    assert(pl_add_end(&pl, "  ", "x", 10, NULL) == PL_ERR_INVALID);
    assert(pl_add_end(&pl, "T", "", 10, NULL) == PL_ERR_INVALID);
    assert(pl_add_end(&pl, "T", "x", 0, NULL) == PL_ERR_INVALID);
    assert(pl_add_end(&pl, "T|x", "x", 5, NULL) == PL_ERR_INVALID);
    assert(pl.count == 6 && pl.nextId == 7);                            /* failures burn no ids */

    /* search */
    hits = 0; assert(pl_search(&pl, "M", count_cb, &hits) == 1 && hits == 1);   /* song M matches in title and artist, counted once */
    hits = 0; pl_search(&pl, "nomatch", count_cb, &hits); assert(hits == 0);

    /* reverse / sort / shuffle keep invariants */
    pl_reverse(&pl); check(&pl);
    titles(&pl, buf); assert(!strcmp(buf, "L,C,B,M,A,F,"));
    pl_sort(&pl, PL_SORT_TITLE); check(&pl);
    titles(&pl, buf); assert(!strcmp(buf, "A,B,C,F,L,M,"));
    pl_sort(&pl, PL_SORT_DURATION); check(&pl);
    assert(pl.head->duration <= pl.head->next->duration);
    pl_sort(&pl, PL_SORT_ARTIST); check(&pl);
    srand((unsigned)time(NULL));
    for (int i = 0; i < 50; i++) { assert(pl_shuffle(&pl) == PL_OK); check(&pl); }
    pl_sort(&pl, PL_SORT_TITLE);

    /* player + repeat */
    pl_play_id(&pl, pl.head->id);
    assert(pl_play_prev(&pl) == PL_ERR_END);
    assert(pl_toggle_repeat(&pl) == 1);
    assert(pl_play_prev(&pl) == PL_OK && pl.current == pl.tail);
    assert(pl_play_next(&pl) == PL_OK && pl.current == pl.head);
    pl_toggle_repeat(&pl);
    pl.current = pl.tail;
    assert(pl_play_next(&pl) == PL_ERR_END);

    /* delete tail while playing it -> current moves to prev; head -> next */
    id = pl.tail->id;
    assert(pl_delete_by_id(&pl, id) == PL_OK); check(&pl);
    assert(pl.current == pl.tail);
    pl.current = pl.head;
    assert(pl_delete_by_title(&pl, pl.head->title) == PL_OK); check(&pl);
    assert(pl.current == pl.head);
    assert(pl_delete_by_title(&pl, "nope") == PL_ERR_NOT_FOUND);
    assert(pl_delete_by_id(&pl, 999) == PL_ERR_NOT_FOUND);

    /* save / load round trip */
    assert(pl_save(&pl, "/tmp/pl_test.txt") == PL_OK);
    titles(&pl, buf);
    { Playlist q; char b2[512]; pl_init(&q);
      assert(pl_load(&q, "/tmp/pl_test.txt") == PL_OK); check(&q);
      titles(&q, b2); assert(!strcmp(buf, b2)); assert(q.count == pl.count);
      assert(pl_load(&q, "/nonexistent/file") == PL_ERR_IO && q.count == pl.count);
      pl_free(&q); }

    /* single song, then delete everything */
    while (pl.head) { assert(pl_delete_by_id(&pl, pl.head->id) == PL_OK); check(&pl); }
    assert(pl.count == 0);
    assert(pl_add_end(&pl, "Solo", "s", 5, NULL) == PL_OK);
    assert(pl_play_next(&pl) == PL_ERR_END);
    pl_toggle_repeat(&pl);
    assert(pl_play_next(&pl) == PL_OK && pl.current == pl.head);
    assert(pl_play_prev(&pl) == PL_OK && pl.current == pl.head);
    pl_reverse(&pl); check(&pl);

    pl_free(&pl); check(&pl);
    puts("all playlist tests passed");
    return 0;
}
