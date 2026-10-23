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
    assert(pl_add_end(&pl, "T|x", "x", 5, NULL) == PL_OK);               /* '|' is allowed now */
    assert(pl_delete_by_title(&pl, "T|x") == PL_OK);
    pl.nextId--;                                                        /* keep id arithmetic below simple */
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

    /* ---- Spotify / local fields ---- */
    {
        const char *U1 = "spotify:track:4cOdK2wGLETKBW3PvgPWqT";
        const char *U2 = "spotify:track:7qiZfU4dY1lWllzX7mPBI3";
        SongInfo s = { "Sp One", "Artist", 200, U1, "https://i.scdn.co/image/abc", NULL, NULL };
        Playlist q; int sid = 0; pl_init(&q);

        assert(pl_add_end_info(&q, &s, &sid) == PL_OK);
        assert(!strcmp(q.head->source, "spotify"));                      /* inferred from the URI */
        assert(!strcmp(q.head->spotify_uri, U1) && !strcmp(q.head->image_url, s.image_url));
        assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_DUPLICATE);       /* same URI */
        s.title = "Sp One"; s.spotify_uri = U2;
        assert(pl_add_start_info(&q, &s, NULL) == PL_OK);                /* same title, other URI: fine */
        assert(q.count == 2);

        s.spotify_uri = "spotify:track:short";   assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);
        s.spotify_uri = "spotify:album:4cOdK2wGLETKBW3PvgPWqT";
                                                  assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);
        s.spotify_uri = "spotify:track:4cOdK2wGLETKBW3PvgPW!T"; assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);
        s.spotify_uri = "";                       s.source = "spotify";
                                                  assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);  /* spotify needs a URI */
        s.source = "tape"; s.spotify_uri = U2;    assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);
        s.source = NULL;   s.image_url = "http://insecure/x.jpg";
                                                  assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);
        s.image_url = NULL; s.audio_url = "/music/a.mp3";
                                                  assert(pl_add_end_info(&q, &s, NULL) == PL_ERR_INVALID);  /* spotify + audio */
        assert(q.count == 2);

        { SongInfo l = { "Local Tune", "Me", 90, NULL, NULL, NULL, "/music/Local%20Tune.mp3" };
          assert(pl_add_end_info(&q, &l, NULL) == PL_OK && !strcmp(q.tail->source, "local"));
          l.title = "local tune";   assert(pl_add_end_info(&q, &l, NULL) == PL_ERR_DUPLICATE);   /* local: by title */
          l.title = "Other"; l.audio_url = "/etc/passwd";   assert(pl_add_end_info(&q, &l, NULL) == PL_ERR_INVALID);
          l.audio_url = "/music/../secret.mp3";             assert(pl_add_end_info(&q, &l, NULL) == PL_ERR_INVALID);
          l.audio_url = NULL; l.spotify_uri = U2; l.source = "local";
                                                            assert(pl_add_end_info(&q, &l, NULL) == PL_ERR_INVALID);
          l.spotify_uri = NULL; l.title = "Sp One";          /* a local song may share a Spotify song's title */
          assert(pl_add_end_info(&q, &l, NULL) == PL_OK); }
        assert(q.count == 4); check(&q);

        /* save/load keeps every field, including '|' in a title */
        { SongInfo p = { "Pipe | Title", "A | B", 61, U1 + 0, NULL, NULL, NULL };
          Playlist r; char b1[512], b2[512]; pl_init(&r);
          p.spotify_uri = "spotify:track:1111111111111111111111";
          assert(pl_add_end_info(&q, &p, NULL) == PL_OK);
          assert(pl_save(&q, "/tmp/pl_test2.txt") == PL_OK);
          assert(pl_load(&r, "/tmp/pl_test2.txt") == PL_OK); check(&r);
          assert(r.count == q.count);
          for (const Song *a = q.head, *b = r.head; a; a = a->next, b = b->next) {
              assert(!strcmp(a->title, b->title) && !strcmp(a->artist, b->artist));
              assert(a->duration == b->duration && !strcmp(a->spotify_uri, b->spotify_uri));
              assert(!strcmp(a->image_url, b->image_url) && !strcmp(a->source, b->source));
              assert(!strcmp(a->audio_url, b->audio_url));
          }
          titles(&q, b1); titles(&r, b2); assert(!strcmp(b1, b2));
          pl_free(&r); }

        /* legacy pipe format + garbage lines */
        { FILE *f = fopen("/tmp/pl_test3.txt", "w"); Playlist r; pl_init(&r);
          fputs("Old Song|Old Artist|120\n# comment\n\nbroken line\nBad|x|abc\nlocal\tonly\tthree\n", f);
          fclose(f);
          assert(pl_load(&r, "/tmp/pl_test3.txt") == PL_OK && r.count == 1);
          assert(!strcmp(r.head->source, "local") && r.head->duration == 120); check(&r);
          pl_free(&r); }
        pl_free(&q); check(&q);
    }

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
