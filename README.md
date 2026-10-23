# Music Playlist Management System

A full-stack playlist manager. The playlist is a **hand-written doubly linked list in C**;
a small embedded HTTP server ([Mongoose](https://github.com/cesanta/mongoose) 7.14) exposes it
as a REST API and also serves a vanilla HTML/CSS/JS front end.

```
music-playlist/
  backend/
    playlist.h / playlist.c   the doubly linked list (no HTTP, no JSON) - study this first
    server.c                  HTTP routing + JSON building (uses playlist.h)
    test_playlist.c           standalone tests for the list, checks pointer invariants
    mongoose.c / mongoose.h   third-party embedded web server (single file pair)
    Makefile
  frontend/  index.html  style.css  app.js
  data/playlist.txt           saved playlist, loaded when the server starts
```

## Build and run

Requirements: GCC (any 4.x+ / MinGW-w64), `make` optional. No other dependencies.

**Linux / macOS**
```bash
cd backend
make                # or: gcc -Wall -Wextra -std=c99 -O2 -c server.c playlist.c && gcc -std=gnu99 -O2 -w -c mongoose.c && gcc -o server server.o playlist.o mongoose.o
./server
```

**Windows (MinGW, cmd or PowerShell)**
```bat
cd backend
mingw32-make
server.exe
```
Without make:
```bat
gcc -Wall -Wextra -std=c99 -O2 -c server.c playlist.c
gcc -std=gnu99 -O2 -w -c mongoose.c
gcc -o server.exe server.o playlist.o mongoose.o -lws2_32
server.exe
```

Open **http://localhost:8080**. Stop with Ctrl+C (all list memory is freed on exit).

Run it from the `backend/` folder. Otherwise pass the paths:
`./server <frontend_dir> <data_file>` (defaults: `../frontend`, `../data/playlist.txt`).

`make test` builds and runs the linked list tests on their own. To check for leaks:
`gcc -g -std=c99 -o t test_playlist.c playlist.c && valgrind --leak-check=full ./t`.

## Linked list design

```c
typedef struct Song { int id; char title[100]; char artist[100]; int duration;
                      struct Song *prev, *next; } Song;
typedef struct Playlist { Song *head, *tail, *current; int count, nextId, repeat; } Playlist;
```
Invariants (verified by `test_playlist.c` after every operation): `head->prev == NULL`,
`tail->next == NULL`, `n->next->prev == n`, and `count` equals the number of nodes.

| Operation | Function | Time | What happens to the pointers |
|---|---|---|---|
| Add at end | `pl_add_end` | O(1) | new->prev = tail; tail->next = new; tail = new |
| Add at start | `pl_add_start` | O(1) | new->next = head; head->prev = new; head = new |
| Insert at position *p* (1..count+1) | `pl_insert_at` | O(p) | walk to the node at *p*, then relink its predecessor and itself to the new node (4 pointers); *p*=1 / count+1 reuse start / end |
| Delete by id / title | `pl_delete_by_id`, `pl_delete_by_title` | O(n) | predecessor->next = successor (or head moves), successor->prev = predecessor (or tail moves); `current` moves to the next song, else the previous one |
| Display / reverse display | iterate `next` from head / `prev` from tail | O(n) | no changes |
| Total duration | `pl_total_duration` | O(n) | none |
| Search (title or artist, case-insensitive, partial) | `pl_search` | O(n·m) | none |
| Next / previous | `pl_play_next`, `pl_play_prev` | O(1) | `current` moves one node; with repeat on it wraps to head / tail |
| Reverse in place | `pl_reverse` | O(n) | swap `prev`/`next` of every node, then swap `head`/`tail` |
| Sort by title/artist/duration | `pl_sort` | O(n log n) | stable merge sort over the `next` chain, then one pass repairs `prev` and `tail` |
| Shuffle | `pl_shuffle` | O(n) | Fisher-Yates over a temporary array of node *pointers*, then the same nodes are relinked (songs are never stored in an array) |
| Save / load | `pl_save`, `pl_load` | O(n) | text file, one `title\|artist\|duration` per line; load builds a temporary list and swaps it in only on success |
| Free | `pl_free` | O(n) | read `next` before freeing each node |

Rules enforced by the list: titles are unique (case-insensitive), title and artist are non-empty
(max 99 chars, no `|` or control characters), duration is 1..35999 seconds, insert position must be in
`1..count+1`. The first song added to an empty list becomes "now playing".

## REST API

JSON in and out, CORS enabled (`Access-Control-Allow-Origin: *`). Errors: `{"error":"message"}` with
400 (bad input, duplicate title, position out of range, end of list with repeat off, empty playlist),
404 (unknown song / endpoint) or 500.

Every successful change returns `message`, `songs` (in list order, each with `id, position, title, artist, duration`),
`count`, `totalDuration` (seconds), `currentId` (or `null`), `repeat` and `current`.

| Method & path | Body / query | Result |
|---|---|---|
| `GET /api/songs` | | songs, count, totalDuration, currentId, repeat |
| `GET /api/songs/reverse` | | same, traversed from the tail |
| `GET /api/songs/search?q=text` | `q` required (empty = all) | matching songs + count |
| `POST /api/songs` | `{title, artist, duration}` **or** `{title, artist, minutes, seconds}` (seconds 0-59); `at`: `"end"` (default) \| `"start"` \| `"position"` with `position` (1-based) | **201** + new `song` |
| `DELETE /api/songs/{id}` | | 200, or 404 |
| `DELETE /api/songs?title=Name` | case-insensitive | delete by title |
| `GET /api/player/current` | | `current` song or `null` |
| `POST /api/player/next` / `prev` | | moves "now playing"; 400 at the end when repeat is off |
| `POST /api/player/repeat` | | toggles repeat |
| `POST /api/player/play/{id}` | | sets the now-playing song (used by clicking a row) |
| `POST /api/playlist/reverse` | | reverses in place |
| `POST /api/playlist/sort` | `{"by":"title"\|"artist"\|"duration"}` | |
| `POST /api/playlist/shuffle` | | |
| `POST /api/playlist/save` | | writes `data/playlist.txt` |
| `POST /api/playlist/load` | | replaces the list from the file (404 if missing) |

Examples:
```bash
curl localhost:8080/api/songs
curl -X POST localhost:8080/api/songs -d '{"title":"Roar","artist":"Katy Perry","minutes":3,"seconds":43,"at":"position","position":2}'
curl -X DELETE localhost:8080/api/songs/3
curl -X POST localhost:8080/api/playlist/sort -d '{"by":"duration"}'
curl "localhost:8080/api/songs/search?q=queen"
curl -X POST -d '' localhost:8080/api/player/next     # note the -d ''
```
> **curl note:** Mongoose answers `411 Length Required` to a POST with no body *and* no `Content-Length` header.
> Browsers always send the header, so the web UI is unaffected; with curl add `-d ''` to body-less POSTs.

## Front end

Plain HTML/CSS/JS served by the C server. Playlist table with a highlighted now-playing row, Now Playing card
(previous / play-pause / next / repeat, simulated progress bar and spinning disc, auto-advance when a song ends),
add form (end / start / position), per-row delete, click a title to play it, live search via the API, Reverse,
View reversed (read-only tail-to-head view from `GET /api/songs/reverse`), Sort dropdown, Shuffle, Save, Load,
total songs and duration in mm:ss, toast messages, and a responsive layout. Titles are inserted with `textContent`, so HTML in a title is never executed.

## Notes and assumptions

* `Reverse` changes the list; `View reversed` only displays it from the tail.
* Saving is explicit (the Save button / endpoint); the file is loaded on server start. Ids are re-assigned on load.
* Mongoose is compiled with `-std=gnu99 -w` (it needs POSIX extensions); `server.c` and `playlist.c` build with
  `-Wall -Wextra -std=c99` and produce zero warnings.
* The server is single-threaded (Mongoose event loop), so the list needs no locking.
