/*
 * server.c - HTTP routing and JSON handling (Mongoose).
 *
 * All song data lives in the doubly linked list from playlist.c; this file
 * only translates HTTP requests into list operations and list contents
 * into JSON.  Static files (the web UI) are served from the frontend dir.
 *
 * Usage:  server [frontend_dir] [data_file]
 *         defaults: ../frontend  ../data/playlist.txt   (run from backend/)
 */
#ifndef _WIN32
#define _XOPEN_SOURCE 700   /* realpath() */
#endif
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mongoose.h"
#include "playlist.h"

#define LISTEN_URL "http://0.0.0.0:8080"

static Playlist g_pl;                       /* the one and only playlist */
static const char *g_frontend = "../frontend";
static const char *g_datafile = "../data/playlist.txt";
static volatile int g_running = 1;
static char g_frontend_abs[4096];

static const char *CORS_JSON =
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n";

/* ====================================================================
 * Tiny growable string buffer used to build JSON by hand
 * ==================================================================== */

typedef struct { char *p; size_t len, cap; int oom; } Buf;

static void buf_put(Buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *np;
        while (cap < b->len + n + 1) cap *= 2;
        np = realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_fmt(Buf *b, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_str(b, tmp);
}

/* Append s as a quoted, escaped JSON string. */
static void buf_json_str(Buf *b, const char *s)
{
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; buf_put(b, e, 2); }
        else if (c < 0x20)         { buf_fmt(b, "\\u%04x", c); }
        else                       { buf_put(b, (const char *)&c, 1); }
    }
    buf_put(b, "\"", 1);
}

/* ====================================================================
 * JSON serialisation of list contents
 * ==================================================================== */

static void song_json(Buf *b, const Song *s, int position)
{
    buf_fmt(b, "{\"id\":%d,\"position\":%d,\"title\":", s->id, position);
    buf_json_str(b, s->title);
    buf_str(b, ",\"artist\":");
    buf_json_str(b, s->artist);
    buf_fmt(b, ",\"duration\":%d}", s->duration);
}

/* "songs":[...] in forward or backward order (reverse walks tail->prev). */
static void songs_array_json(Buf *b, int backwards)
{
    buf_str(b, "\"songs\":[");
    if (backwards) {
        int pos = g_pl.count;
        for (const Song *s = g_pl.tail; s; s = s->prev, pos--) {
            song_json(b, s, pos);
            if (s->prev) buf_str(b, ",");
        }
    } else {
        int pos = 1;
        for (const Song *s = g_pl.head; s; s = s->next, pos++) {
            song_json(b, s, pos);
            if (s->next) buf_str(b, ",");
        }
    }
    buf_str(b, "]");
}

/* Summary fields shared by most responses. */
static void state_fields_json(Buf *b)
{
    buf_fmt(b, "\"count\":%d,\"totalDuration\":%d,\"currentId\":",
            g_pl.count, pl_total_duration(&g_pl));
    if (g_pl.current) buf_fmt(b, "%d", g_pl.current->id);
    else              buf_str(b, "null");
    buf_fmt(b, ",\"repeat\":%s", g_pl.repeat ? "true" : "false");
}

static int position_of(const Song *target)
{
    int pos = 1;
    for (const Song *s = g_pl.head; s; s = s->next, pos++)
        if (s == target) return pos;
    return 0;
}

static void current_json(Buf *b)
{
    buf_str(b, "\"current\":");
    if (g_pl.current) song_json(b, g_pl.current, position_of(g_pl.current));
    else              buf_str(b, "null");
}

/* ====================================================================
 * Replies
 * ==================================================================== */

static void reply_buf(struct mg_connection *c, int status, Buf *b)
{
    if (b->oom || !b->p) {
        mg_http_reply(c, 500, CORS_JSON, "{\"error\":\"out of memory\"}");
    } else {
        mg_http_reply(c, status, CORS_JSON, "%.*s", (int)b->len, b->p);
    }
    free(b->p);
}

static void reply_error(struct mg_connection *c, int status, const char *msg)
{
    Buf b = {0};
    buf_str(&b, "{\"error\":");
    buf_json_str(&b, msg);
    buf_str(&b, "}");
    reply_buf(c, status, &b);
}

static int status_for(PlResult r)
{
    switch (r) {
    case PL_ERR_NOT_FOUND: return 404;
    case PL_ERR_NOMEM:
    case PL_ERR_IO:        return 500;
    default:               return 400;   /* invalid, duplicate, range, empty, end */
    }
}

static void reply_pl_error(struct mg_connection *c, PlResult r)
{
    reply_error(c, status_for(r), pl_strerror(r));
}

/* {"message":..., full state (songs + summary + current)} */
static void reply_state(struct mg_connection *c, int status, const char *msg,
                        const Song *created)
{
    Buf b = {0};
    buf_str(&b, "{\"message\":");
    buf_json_str(&b, msg);
    if (created) {
        buf_str(&b, ",\"song\":");
        song_json(&b, created, position_of(created));
    }
    buf_str(&b, ",");
    songs_array_json(&b, 0);
    buf_str(&b, ",");
    state_fields_json(&b);
    buf_str(&b, ",");
    current_json(&b);
    buf_str(&b, "}");
    reply_buf(c, status, &b);
}

/* ====================================================================
 * Request helpers
 * ==================================================================== */

static int route(const struct mg_http_message *hm, const char *method,
                 const char *pattern)
{
    return mg_match(hm->method, mg_str(method), NULL) &&
           mg_match(hm->uri, mg_str(pattern), NULL);
}

/* Parse the trailing "/<digits>" of a URI; returns 1 and sets *id on success. */
static int id_from_uri(const struct mg_str uri, const char *prefix, int *id)
{
    size_t n = strlen(prefix);
    long v = 0;
    if (uri.len <= n || memcmp(uri.buf, prefix, n) != 0) return 0;
    for (size_t i = n; i < uri.len; i++) {
        if (uri.buf[i] < '0' || uri.buf[i] > '9') return 0;
        v = v * 10 + (uri.buf[i] - '0');
        if (v > 1000000000L) return 0;
    }
    *id = (int)v;
    return 1;
}

/* Read an integer field that may be absent.  Returns 1 present, 0 absent,
 * -1 present but not a whole number. */
static int json_int(struct mg_str json, const char *path, long *out)
{
    double d;
    if (!mg_json_get_num(json, path, &d)) return 0;
    if (d != (double)(long)d || d < -1e9 || d > 1e9) return -1;
    *out = (long)d;
    return 1;
}

/* ====================================================================
 * Handlers
 * ==================================================================== */

static void handle_add(struct mg_connection *c, struct mg_http_message *hm)
{
    char *title = mg_json_get_str(hm->body, "$.title");
    char *artist = mg_json_get_str(hm->body, "$.artist");
    char *at = mg_json_get_str(hm->body, "$.at");
    long duration = 0, minutes = 0, seconds = 0, position = 0;
    int has_dur = json_int(hm->body, "$.duration", &duration);
    int has_min = json_int(hm->body, "$.minutes", &minutes);
    int has_sec = json_int(hm->body, "$.seconds", &seconds);
    int has_pos = json_int(hm->body, "$.position", &position);
    const char *err = NULL;
    PlResult r = PL_OK;
    int new_id = 0;

    if (!title || !artist)                          err = "title and artist are required";
    else if (has_dur < 0 || has_min < 0 || has_sec < 0 || has_pos < 0)
                                                    err = "duration, minutes, seconds and position must be whole numbers";
    else if (has_dur == 0 && has_min == 0 && has_sec == 0)
                                                    err = "duration is required (seconds, or minutes + seconds)";
    else if (has_dur == 0) {                        /* minutes + seconds form */
        if (seconds < 0 || seconds > 59)            err = "seconds must be between 0 and 59";
        else if (minutes < 0)                       err = "minutes must not be negative";
        else                                        duration = minutes * 60 + seconds;
    }
    if (!err && (duration < 1 || duration > 35999))
        err = "duration must be between 1 and 35999 seconds";
    if (!err && at && strcmp(at, "end") && strcmp(at, "start") && strcmp(at, "position"))
        err = "at must be \"end\", \"start\" or \"position\"";

    if (err) {
        reply_error(c, 400, err);
    } else {
        /* "position" without "at" implies at:"position" */
        const char *mode = at ? at : (has_pos == 1 ? "position" : "end");
        if (!strcmp(mode, "start"))
            r = pl_add_start(&g_pl, title, artist, (int)duration, &new_id);
        else if (!strcmp(mode, "position")) {
            if (has_pos != 1) { reply_error(c, 400, "position is required when at is \"position\""); goto done; }
            r = pl_insert_at(&g_pl, (int)position, title, artist, (int)duration, &new_id);
        } else
            r = pl_add_end(&g_pl, title, artist, (int)duration, &new_id);

        if (r == PL_OK) reply_state(c, 201, "song added", pl_find_by_id(&g_pl, new_id));
        else            reply_pl_error(c, r);
    }
done:
    free(title); free(artist); free(at);
}

static void handle_delete(struct mg_connection *c, struct mg_http_message *hm)
{
    int id;
    char title[PL_TITLE_MAX + 8];
    PlResult r;

    if (id_from_uri(hm->uri, "/api/songs/", &id)) {
        r = pl_delete_by_id(&g_pl, id);
    } else if (mg_match(hm->uri, mg_str("/api/songs"), NULL) &&
               mg_http_get_var(&hm->query, "title", title, sizeof title) > 0) {
        r = pl_delete_by_title(&g_pl, title);       /* DELETE /api/songs?title=... */
    } else {
        reply_error(c, 400, "use DELETE /api/songs/{id} or /api/songs?title=...");
        return;
    }
    if (r == PL_OK) reply_state(c, 200, "song deleted", NULL);
    else            reply_pl_error(c, r);
}

static void search_cb(const Song *s, void *ctx)
{
    Buf *b = ctx;
    if (b->len > 0 && b->p[b->len - 1] != '[') buf_str(b, ",");
    song_json(b, s, position_of(s));
}

static void handle_search(struct mg_connection *c, struct mg_http_message *hm)
{
    char q[256];
    Buf b = {0};
    int n;
    if (mg_http_get_var(&hm->query, "q", q, sizeof q) < 0) {
        reply_error(c, 400, "missing query parameter q");
        return;
    }
    buf_str(&b, "{\"query\":");
    buf_json_str(&b, q);
    buf_str(&b, ",\"songs\":[");
    n = pl_search(&g_pl, q, search_cb, &b);
    buf_fmt(&b, "],\"count\":%d,\"currentId\":", n);
    if (g_pl.current) buf_fmt(&b, "%d", g_pl.current->id); else buf_str(&b, "null");
    buf_str(&b, "}");
    reply_buf(c, 200, &b);
}

static void handle_list(struct mg_connection *c, int backwards)
{
    Buf b = {0};
    buf_str(&b, "{");
    songs_array_json(&b, backwards);
    buf_str(&b, ",");
    state_fields_json(&b);
    buf_fmt(&b, ",\"order\":\"%s\"}", backwards ? "reverse" : "forward");
    reply_buf(c, 200, &b);
}

static void handle_current(struct mg_connection *c)
{
    Buf b = {0};
    buf_str(&b, "{");
    current_json(&b);
    buf_str(&b, ",");
    state_fields_json(&b);
    buf_str(&b, "}");
    reply_buf(c, 200, &b);
}

static void handle_move(struct mg_connection *c, int forward)
{
    PlResult r = forward ? pl_play_next(&g_pl) : pl_play_prev(&g_pl);
    if (r == PL_OK) reply_state(c, 200, forward ? "playing next" : "playing previous", NULL);
    else            reply_pl_error(c, r);
}

static void handle_sort(struct mg_connection *c, struct mg_http_message *hm)
{
    char *by = mg_json_get_str(hm->body, "$.by");
    PlSortKey key;
    if (!by) { reply_error(c, 400, "body must be {\"by\":\"title\"|\"artist\"|\"duration\"}"); return; }
    if      (!strcmp(by, "title"))    key = PL_SORT_TITLE;
    else if (!strcmp(by, "artist"))   key = PL_SORT_ARTIST;
    else if (!strcmp(by, "duration")) key = PL_SORT_DURATION;
    else { free(by); reply_error(c, 400, "by must be title, artist or duration"); return; }
    pl_sort(&g_pl, key);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "sorted by %s", by);
        reply_state(c, 200, msg, NULL);
    }
    free(by);
}

/* ====================================================================
 * Event handler / router
 * ==================================================================== */

static void on_event(struct mg_connection *c, int ev, void *ev_data)
{
    struct mg_http_message *hm;
    if (ev != MG_EV_HTTP_MSG) return;
    hm = (struct mg_http_message *) ev_data;

    if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {        /* CORS preflight */
        mg_http_reply(c, 204, CORS_JSON, "");
    } else if (route(hm, "GET", "/api/songs"))           handle_list(c, 0);
    else if (route(hm, "GET",  "/api/songs/reverse"))    handle_list(c, 1);
    else if (route(hm, "GET",  "/api/songs/search"))     handle_search(c, hm);
    else if (route(hm, "POST", "/api/songs"))            handle_add(c, hm);
    else if (route(hm, "DELETE", "/api/songs") ||
             route(hm, "DELETE", "/api/songs/*"))        handle_delete(c, hm);
    else if (route(hm, "GET",  "/api/player/current"))   handle_current(c);
    else if (route(hm, "POST", "/api/player/next"))      handle_move(c, 1);
    else if (route(hm, "POST", "/api/player/prev"))      handle_move(c, 0);
    else if (route(hm, "POST", "/api/player/repeat")) {
        int on = pl_toggle_repeat(&g_pl);
        reply_state(c, 200, on ? "repeat on" : "repeat off", NULL);
    } else if (mg_match(hm->method, mg_str("POST"), NULL) &&
               mg_match(hm->uri, mg_str("/api/player/play/*"), NULL)) {
        int id;
        if (!id_from_uri(hm->uri, "/api/player/play/", &id)) reply_error(c, 400, "invalid song id");
        else {
            PlResult r = pl_play_id(&g_pl, id);
            if (r == PL_OK) reply_state(c, 200, "now playing", NULL);
            else            reply_pl_error(c, r);
        }
    } else if (route(hm, "POST", "/api/playlist/reverse")) {
        pl_reverse(&g_pl);
        reply_state(c, 200, "playlist reversed", NULL);
    } else if (route(hm, "POST", "/api/playlist/sort"))    handle_sort(c, hm);
    else if (route(hm, "POST", "/api/playlist/shuffle")) {
        PlResult r = pl_shuffle(&g_pl);
        if (r == PL_OK) reply_state(c, 200, "playlist shuffled", NULL);
        else            reply_pl_error(c, r);
    } else if (route(hm, "POST", "/api/playlist/save")) {
        PlResult r = pl_save(&g_pl, g_datafile);
        if (r == PL_OK) reply_state(c, 200, "playlist saved", NULL);
        else            reply_error(c, 500, "could not write the data file");
    } else if (route(hm, "POST", "/api/playlist/load")) {
        PlResult r = pl_load(&g_pl, g_datafile);
        if (r == PL_OK) reply_state(c, 200, "playlist loaded", NULL);
        else if (r == PL_ERR_IO) reply_error(c, 404, "data file not found");
        else reply_pl_error(c, r);
    } else if (mg_match(hm->uri, mg_str("/api/*"), NULL)) {
        reply_error(c, 404, "unknown API endpoint or method");
    } else {
        struct mg_http_serve_opts opts;
        memset(&opts, 0, sizeof opts);
        opts.root_dir = g_frontend_abs;
        mg_http_serve_dir(c, hm, &opts);
    }
}

/*
 * Mongoose refuses a web root that contains "..", so turn a relative path
 * such as ../frontend into an absolute one.  Returns 0 on failure.
 */
static int resolve_dir(const char *dir, char *out, size_t cap)
{
#ifdef _WIN32
    return _fullpath(out, dir, cap) != NULL;
#else
    char tmp[PATH_MAX];
    if (!realpath(dir, tmp) || strlen(tmp) >= cap) return 0;
    strcpy(out, tmp);
    return 1;
#endif
}

static void on_signal(int sig) { (void)sig; g_running = 0; }

int main(int argc, char **argv)
{
    struct mg_mgr mgr;
    PlResult r;

    if (argc > 1) g_frontend = argv[1];
    if (argc > 2) g_datafile = argv[2];

    if (!resolve_dir(g_frontend, g_frontend_abs, sizeof g_frontend_abs)) {
        fprintf(stderr, "Frontend folder not found: %s\n"
                        "Run from the backend/ folder or pass the path: server <frontend_dir> <data_file>\n",
                g_frontend);
        return 1;
    }

    srand((unsigned)time(NULL));
    pl_init(&g_pl);
    r = pl_load(&g_pl, g_datafile);
    if (r == PL_OK) printf("Loaded %d song(s) from %s\n", g_pl.count, g_datafile);
    else            printf("No saved playlist at %s (starting empty)\n", g_datafile);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    mg_mgr_init(&mgr);
    if (!mg_http_listen(&mgr, LISTEN_URL, on_event, NULL)) {
        fprintf(stderr, "Cannot listen on %s (is the port in use?)\n", LISTEN_URL);
        mg_mgr_free(&mgr);
        pl_free(&g_pl);
        return 1;
    }
    printf("Music Playlist server running: http://localhost:8080  (Ctrl+C to stop)\n");
    while (g_running) mg_mgr_poll(&mgr, 200);

    printf("\nShutting down...\n");
    mg_mgr_free(&mgr);
    pl_free(&g_pl);                         /* release every node */
    return 0;
}
