# Music Playlist Management System

A full-stack playlist manager. The playlist is a **hand-written doubly linked list in C**;
a small embedded HTTP server ([Mongoose](https://github.com/cesanta/mongoose) 7.14) exposes it
as a REST API and also serves a vanilla HTML/CSS/JS front end.

Songs come from **Spotify** (search + real playback with the Web API and Web Playback SDK, Premium
required) or from **local MP3 files**. The C linked list decides the order and what plays on
Next / Previous; the browser plays whatever song the list says is current.

```
 browser (frontend/*.js) ----- Spotify login (PKCE), search, playback SDK ----> Spotify
        |  REST + JSON (no tokens)
        v
 C server (backend/) -- doubly linked list: order, next/prev, repeat, sort, shuffle, save/load
```
**The C backend never sees a Spotify token or secret.** All Spotify calls are made by the page's JavaScript.

```
music-playlist/
  backend/
    playlist.h / playlist.c   the doubly linked list (no HTTP, no JSON) - study this first
    server.c                  HTTP routing + JSON building (uses playlist.h)
    test_playlist.c           standalone tests for the list, checks pointer invariants
    mongoose.c / mongoose.h   third-party embedded web server (single file pair)
    Makefile
  frontend/
    index.html  style.css
    app.js                    playlist UI, player engine, talks to the C API
    spotify.js                Spotify login (PKCE), search, Web Playback SDK
    fx.js                     visual effects only: floating music notes, animated cursor
    config.example.js         copy to config.js and paste your Client ID (config.js is git-ignored)
  music/                      your own .mp3 files (git-ignored), served at /music/<file>.mp3
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

Open **http://127.0.0.1:8080** (use `127.0.0.1`, not `localhost`; `localhost` is redirected for you).
Stop with Ctrl+C (all list memory is freed on exit).

Run it from the `backend/` folder. Otherwise pass the paths:
`./server <frontend_dir> <data_file> <music_dir>` (defaults: `../frontend`, `../data/playlist.txt`, `../music`).

## Spotify setup (one time)

Requirements: a **Spotify Premium** account (the Web Playback SDK refuses free accounts) and a desktop
browser with DRM support (**Chrome or Edge** recommended; Safari/Firefox may work, mobile browsers do not).

1. Go to <https://developer.spotify.com/dashboard> and **Create app**.
2. App name/description: anything. **Redirect URI**: `http://127.0.0.1:8080/callback`
   (exactly this, with `127.0.0.1`, no trailing slash). Click *Add*, then Save.
3. Under **"Which API/SDKs are you planning to use?"** tick **Web API** and **Web Playback SDK**.
4. Open the app's **Settings**, copy the **Client ID**. (Ignore the client secret: this app never uses one.)
5. Copy `frontend/config.example.js` to `frontend/config.js` and paste the Client ID:
   ```js
   window.SPOTIFY_CONFIG = { clientId: "abc123..." };
   ```
   `config.js` is in `.gitignore`.
6. **Development Mode** apps only work for people you list. Open the app -> **User Management** ->
   add the name and **Spotify account email** of everyone who will log in (up to 5, all Premium).
   Users who are not listed get a "403 / not registered" error after login.
7. Start the server, open <http://127.0.0.1:8080>, click **Login with Spotify**, accept the scopes
   (`streaming user-read-email user-read-private user-modify-playback-state`).
   (`user-playback-state` is deliberately not requested: it is not needed, and Spotify answered `invalid_scope`
   for it on a Development Mode app.)

Login uses **Authorization Code with PKCE**; tokens are kept in the browser's `localStorage` and refreshed
automatically a minute before they expire.

### Local MP3 fallback
Drop `.mp3` files into `music/` (name them `Artist - Title.mp3` to get artist and title filled in).
They appear under **Local MP3s** with an **Add** button; the browser reads the real duration. They play with
the HTML5 audio element and work without Spotify. Only `.mp3` files are served from `/music/`.

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
| Save / load | `pl_save`, `pl_load` | O(n) | text file, one tab-separated line per song (`source, title, artist, duration, spotify_uri, image_url, audio_url`; old `title\|artist\|duration` files still load); load builds a temporary list and swaps it in only on success |
| Free | `pl_free` | O(n) | read `next` before freeing each node |

Rules enforced by the list (`make_song` in `playlist.c`):

* title and artist non-empty (max 99 chars, no control characters); duration 1..35999 seconds
* every song has a `source`: `"spotify"` (needs `spotify_uri` = `spotify:track:` + 22 characters, no `audio_url`)
  or `"local"` (no `spotify_uri`; optional `audio_url` of the form `/music/<file>`, no `..`)
* `image_url` is optional and must start with `https://`
* **duplicates:** Spotify songs are unique by `spotify_uri`; local songs are unique by title (case-insensitive)
* insert position must be in `1..count+1`; the first song added to an empty list becomes "now playing"

`Song` nodes carry `spotify_uri[100]`, `image_url[300]`, `source[10]` and `audio_url[300]` next to the original
fields. `pl_add_end_info / pl_add_start_info / pl_insert_at_info` take a `SongInfo` struct; the older
`pl_add_end / pl_add_start / pl_insert_at` remain as wrappers for plain local songs.

## REST API

JSON in and out, CORS enabled (`Access-Control-Allow-Origin: *`). Errors: `{"error":"message"}` with
400 (bad input, duplicate title, position out of range, end of list with repeat off, empty playlist),
404 (unknown song / endpoint) or 500.

Every successful change returns `message`, `songs` (in list order, each with `id, position, title, artist, duration, source, spotify_uri, image_url, audio_url`),
`count`, `totalDuration` (seconds), `currentId` (or `null`), `repeat` and `current`.

| Method & path | Body / query | Result |
|---|---|---|
| `GET /api/songs` | | songs, count, totalDuration, currentId, repeat |
| `GET /api/songs/reverse` | | same, traversed from the tail |
| `GET /api/songs/search?q=text` | `q` required (empty = all) | matching songs + count |
| `POST /api/songs` | `{title, artist, duration}` **or** `{title, artist, minutes, seconds}` (seconds 0-59); optional `spotify_uri`, `image_url`, `source` (`"spotify"`\|`"local"`), `audio_url`; `at`: `"end"` (default) \| `"start"` \| `"position"` with `position` (1-based) | **201** + new `song` |
| `DELETE /api/songs/{id}` | | 200, or 404 |
| `DELETE /api/songs?title=Name` | case-insensitive | delete by title |
| `GET /api/player/current` | | `current` song or `null` |
| `POST /api/player/next` / `prev` | | moves "now playing" and returns it as `current` (with `spotify_uri` / `audio_url`) for the page to play; 400 at the end when repeat is off |
| `POST /api/player/repeat` | | toggles repeat |
| `POST /api/player/play/{id}` | | sets the now-playing song (used by clicking a row) |
| `POST /api/playlist/reverse` | | reverses in place |
| `POST /api/playlist/sort` | `{"by":"title"\|"artist"\|"duration"}` | |
| `POST /api/playlist/shuffle` | | |
| `POST /api/playlist/save` | | writes `data/playlist.txt` |
| `POST /api/playlist/load` | | replaces the list from the file (404 if missing) |
| `GET /api/local-files` | | `{files:[{name, url}]}` for the `.mp3` files in `music/` |
| `GET /callback` | | serves the web app (Spotify's redirect target; the page finishes the PKCE login) |
| `GET /music/<file>.mp3` | | local audio (supports HTTP Range, so seeking works) |

Examples:
```bash
curl localhost:8080/api/songs
curl -X POST localhost:8080/api/songs -d '{"title":"Roar","artist":"Katy Perry","minutes":3,"seconds":43,"at":"position","position":2}'
curl -X DELETE localhost:8080/api/songs/3
curl -X POST localhost:8080/api/playlist/sort -d '{"by":"duration"}'
curl "localhost:8080/api/songs/search?q=queen"
curl -X POST -d '' localhost:8080/api/player/next     # note the -d ''
curl -X POST localhost:8080/api/songs -d '{"title":"Song","artist":"Me","duration":200,"source":"spotify","spotify_uri":"spotify:track:4cOdK2wGLETKBW3PvgPWqT"}'
```
> **curl note:** Mongoose answers `411 Length Required` to a POST with no body *and* no `Content-Length` header.
> Browsers always send the header, so the web UI is unaffected; with curl add `-d ''` to body-less POSTs.

## Front end

Plain HTML/CSS/JS served by the C server, no build step.

* **Playlist** table with album art, source badge, highlighted now-playing row, per-row delete, click a title to
  play it, filter box (uses the API), Reverse, View reversed (read-only tail-to-head view), Sort, Shuffle, Save, Load,
  total songs and duration in mm:ss, toast messages, responsive layout.
* **Now Playing** card with album art, previous / play-pause / next, repeat toggle, seek bar and volume slider.
* **Find songs**: Login with Spotify, search (10 per request, **Load more**), album art + title + artist + duration,
  **Add** to the end / start / a position; local MP3 list.
* **Playback**: Next, Previous and "song finished" all call the C backend (`/api/player/next`, `/api/player/prev`),
  then play the song it returns: Spotify songs via `PUT /v1/me/player/play?device_id=...` with `{"uris":[spotify_uri]}`
  on the Web Playback SDK device, local songs through `<audio>`. Spotify's own next/previous/queue endpoints are never
  called. With repeat on, the list wraps around; with repeat off, playback stops after the last song.
* **Look and feel**: frosted-glass panels over a glowing gradient, drifting music notes in the background, a glowing
  cursor with a trailing ring that grows over buttons, a short trail of notes and a burst on click, an equalizer on the
  playing row, glowing album art while playing, filled seek/volume sliders. Effects are in `fx.js` / `style.css` only.
  They switch off for "reduce motion" and the cursor effects are skipped on touch screens.
* **Errors** are shown as toasts (and a banner for login problems): not logged in, expired session, non-Premium
  account, browser without DRM (use Chrome/Edge), network failure, rate limit (429, with the wait time), and Dashboard
  user-list problems (403). Titles are inserted with `textContent`, so HTML in a title never executes.

## Notes and assumptions

* `Reverse` changes the list; `View reversed` only displays it from the tail.
* Saving is explicit (the Save button / endpoint); the file is loaded on server start. Ids are re-assigned on load.
* Mongoose is compiled with `-std=gnu99 -w` (it needs POSIX extensions); `server.c` and `playlist.c` build with
  `-Wall -Wextra -std=c99` and produce zero warnings.
* The server is single-threaded (Mongoose event loop), so the list needs no locking.
* Titles/artists longer than 99 characters (some Spotify tracks) are shortened by the page before they are sent.
* Delete-by-title removes the first song with that title; Spotify songs may share titles, so prefer delete-by-id.
* `data/playlist.txt` ships empty; use **Save** after building a playlist.

## Testing

* `make test` (in `backend/`): linked list unit tests (`test_playlist.c`), including the Spotify/local rules and
  the save/load round trip. Run under valgrind: no errors, no leaks.
* The REST API was exercised with curl (all endpoints, edge cases) and the UI with Playwright in headless Chromium,
  with Spotify's endpoints and the Web Playback SDK **mocked**, so PKCE, token refresh, search, add, Next/Previous,
  auto-advance, repeat and all error messages are verified, but not against the real Spotify service.

### What you must test yourself with a real Premium login
Checklist:

1. Login with Spotify completes and you return to the app logged in (name shown next to **Log out**).
2. Search returns tracks with art; **Load more** works; adding to end / start / position keeps the order you chose.
3. Click a song: it really plays in the browser. Pause/resume, seek and volume work.
4. Next / Previous move through **your playlist order** (not Spotify's) and update the highlighted row.
5. Let a track finish: the next playlist song starts by itself. With repeat on, the last song wraps to the first;
   with repeat off, playback stops.
6. Reverse / Sort / Shuffle then Next: it follows the new order.
7. Leave the tab open for over an hour: playback and search still work (token refresh).
8. Log in with an account that is *not* in User Management (or a free account): the error message is clear.
9. Reload the page: you stay logged in. Save, restart the server, Load: songs (with art) come back and play.
10. A local MP3 plays, finishes, and hands over to a Spotify song (and the other way round).
11. Chrome/Edge works; in a browser without DRM you see the "use Chrome or Edge" message.
