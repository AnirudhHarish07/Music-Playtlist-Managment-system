/*
 * Music Playlist Manager - UI.
 *
 * Order, Next and Previous ALWAYS come from the C linked list through the
 * REST API (/api/player/next, /api/player/prev).  The API answers with the
 * song that is now current; this file then plays it:
 *   - source "spotify": through the Spotify Web Playback SDK (spotify.js)
 *   - source "local":   through an HTML5 <audio> element (/music/<file>.mp3)
 * Spotify's own next/previous/queue endpoints are never used.
 */
(function () {
  'use strict';

  // Spotify only accepts the redirect URI http://127.0.0.1:8080/callback, and
  // browser storage is per-origin, so always run on 127.0.0.1.
  if (location.hostname === 'localhost') {
    location.replace(location.href.replace('//localhost', '//127.0.0.1'));
    return;
  }

  var $ = function (id) { return document.getElementById(id); };
  var audio = $('audio');
  var MSG = SP.MESSAGES;

  var state = { songs: [], count: 0, totalDuration: 0, currentId: null, repeat: false };
  var viewReversed = false;
  var searchQuery = '';
  var searchSeq = 0;               // ignore out-of-date playlist-filter responses

  /* ---------- helpers ---------- */

  function fmt(sec) {
    sec = Math.max(0, Math.floor(sec || 0));
    var m = Math.floor(sec / 60), s = sec % 60;
    return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
  }

  var lastToast = { msg: '', at: 0 };
  function toast(msg, isError) {
    var now = Date.now();
    if (msg === lastToast.msg && now - lastToast.at < 2000) return;   // swallow duplicates
    lastToast = { msg: msg, at: now };
    var el = document.createElement('div');
    el.className = 'toast' + (isError ? ' error' : '');
    el.textContent = msg;
    $('toasts').appendChild(el);
    setTimeout(function () { el.remove(); }, isError ? 6000 : 3200);
  }

  function showBanner(msg) {
    var b = $('sp-banner');
    b.textContent = '';
    var t = document.createElement('span');
    t.textContent = msg;
    var x = document.createElement('button');
    x.className = 'small';
    x.textContent = '×';
    x.setAttribute('aria-label', 'Dismiss');
    x.addEventListener('click', function () { b.hidden = true; });
    b.appendChild(t);
    b.appendChild(x);
    b.hidden = false;
  }

  // Every call returns parsed JSON; HTTP errors throw with the server's message.
  function api(method, path, body) {
    var opts = { method: method, headers: {} };
    if (body !== undefined) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(path, opts).catch(function () {
      throw new Error('Cannot reach the playlist server. Is it running on port 8080?');
    }).then(function (res) {
      return res.json().catch(function () { return {}; }).then(function (data) {
        if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
        return data;
      });
    });
  }

  var BANNER_CODES = ['drm', 'premium', 'expired', 'not_logged_in', 'not_configured', 'forbidden'];
  function fail(err) {
    var msg = (err && err.message) || 'Request failed';
    toast(msg, true);
    if (err && BANNER_CODES.indexOf(err.code) >= 0) showBanner(msg);
    if (err && err.code === 'expired') renderAuth();
  }

  function trunc(s, n) { s = String(s || ''); return s.length > n ? s.slice(0, n - 1) + '…' : s; }

  /* ---------- playback engine (Spotify or local audio) ---------- */

  // songId: the playlist song being played; kind: "spotify" | "local" | null
  var engine = { songId: null, kind: null, uri: null, playing: false, pos: 0, dur: 0, stamp: 0 };
  var seeking = false;

  function currentSong() {
    for (var i = 0; i < state.songs.length; i++)
      if (state.songs[i].id === state.currentId) return state.songs[i];
    return null;
  }

  function enginePos() {
    if (!engine.kind) return 0;
    if (engine.kind === 'local') return audio.currentTime || 0;
    return engine.pos + (engine.playing ? (performance.now() - engine.stamp) / 1000 : 0);
  }

  function volume() { return parseInt($('volume').value, 10) / 100; }

  function stopEngine() {
    audio.pause();
    if (engine.kind === 'spotify') SP.pause().catch(function () {});
    engine = { songId: null, kind: null, uri: null, playing: false, pos: 0, dur: 0, stamp: 0 };
    renderTransport();
  }

  function startSpotifyPlayer() {
    return SP.initPlayer({
      state: onSpotifyState,
      ended: onTrackEnded,
      error: fail
    }, volume());
  }

  function onSpotifyState(s) {
    if (engine.kind !== 'spotify' || s.uri !== engine.uri) return;
    engine.pos = s.position / 1000;
    engine.dur = s.duration / 1000;
    engine.playing = !s.paused;
    engine.stamp = performance.now();
    renderTransport();
  }

  // Both players call this when the song finishes: ask the C list for the next one.
  function onTrackEnded() {
    if (engine.endFired) return;                   // the SDK event and the timer below may both report the end
    engine.endFired = true;
    engine.playing = false;
    advance('/api/player/next', true);
  }

  // Safety net for Spotify: if the clock runs well past the track length and no
  // "ended" state arrived from the SDK, treat the track as finished.
  function checkSpotifyEnd() {
    if (engine.kind === 'spotify' && engine.playing && !engine.endFired &&
        engine.dur > 0 && enginePos() >= engine.dur + 1.5) onTrackEnded();
  }

  // Start playing a song object (the one the C backend says is current).
  function playSong(song) {
    if (!song) { stopEngine(); return Promise.resolve(); }

    if (song.source === 'spotify') {
      if (!SP.isConfigured()) { fail(SP.SpotifyError('not_configured')); stopEngine(); return Promise.resolve(); }
      if (!SP.isLoggedIn())   { fail(SP.SpotifyError('not_logged_in')); stopEngine(); return Promise.resolve(); }
      audio.pause();
      engine = { songId: song.id, kind: 'spotify', uri: song.spotify_uri, playing: true, pos: 0, dur: song.duration, stamp: performance.now() };
      renderTransport();
      return startSpotifyPlayer().then(function () { return SP.play(song.spotify_uri); }).catch(function (e) {
        engine.playing = false;
        renderTransport();
        fail(e);
      });
    }

    // local song
    if (engine.kind === 'spotify') SP.pause().catch(function () {});
    if (!song.audio_url) {
      engine = { songId: null, kind: null, uri: null, playing: false, pos: 0, dur: 0, stamp: 0 };
      audio.pause();
      renderTransport();
      toast('"' + song.title + '" has no audio file. Add MP3s to the music/ folder or search Spotify.', true);
      return Promise.resolve();
    }
    engine = { songId: song.id, kind: 'local', uri: null, playing: true, pos: 0, dur: song.duration, stamp: 0 };
    audio.src = song.audio_url;
    audio.volume = volume();
    renderTransport();
    return audio.play().catch(function () {
      engine.playing = false;
      renderTransport();
      toast('Could not play "' + song.title + '". Is the file still in the music/ folder?', true);
    });
  }

  // Next / Previous: the C linked list decides, then we play what it returns.
  function advance(path, auto) {
    return api('POST', path).then(function (d) {
      applyState(d, true);
      return playSong(d.current || currentSong());
    }).catch(function (err) {
      if (auto) {
        engine.playing = false;
        renderTransport();
        toast('Reached the end of the playlist');
      } else {
        fail(err);
      }
    });
  }

  function playId(id) {
    api('POST', '/api/player/play/' + id).then(function (d) {
      applyState(d, true);
      return playSong(d.current || currentSong());
    }).catch(fail);
  }

  audio.addEventListener('ended', function () { if (engine.kind === 'local') onTrackEnded(); });
  audio.addEventListener('play', function () { if (engine.kind === 'local') { engine.playing = true; renderTransport(); } });
  audio.addEventListener('pause', function () { if (engine.kind === 'local' && !audio.ended) { engine.playing = false; renderTransport(); } });
  audio.addEventListener('loadedmetadata', function () { if (engine.kind === 'local' && isFinite(audio.duration)) engine.dur = audio.duration; });

  /* ---------- rendering ---------- */

  function thumb(url, cls) {
    var el;
    if (url) {
      el = document.createElement('img');
      el.src = url;
      el.alt = '';
      el.loading = 'lazy';
      el.referrerPolicy = 'no-referrer';
    } else {
      el = document.createElement('span');
      el.textContent = '♫';
    }
    el.className = cls + (url ? '' : ' ph');
    return el;
  }

  function renderRows(songs) {
    var body = $('song-rows');
    body.textContent = '';
    songs.forEach(function (s) {
      var tr = document.createElement('tr');
      if (s.id === state.currentId) tr.className = 'playing';

      function cell(text, cls) {
        var td = document.createElement('td');
        td.textContent = text;
        if (cls) td.className = cls;
        tr.appendChild(td);
        return td;
      }
      cell(s.position);

      var t = cell('', 'title');
      var wrap = document.createElement('div');
      wrap.className = 'title-wrap';
      wrap.appendChild(thumb(s.image_url, 'thumb'));
      var txt = document.createElement('span');
      txt.className = 'title-text';
      txt.textContent = s.title;
      wrap.appendChild(txt);
      var badge = document.createElement('span');
      badge.className = 'badge ' + s.source;
      badge.textContent = s.source === 'spotify' ? 'Spotify' : (s.audio_url ? 'MP3' : 'no audio');
      wrap.appendChild(badge);
      t.appendChild(wrap);
      t.title = 'Click to play';
      t.addEventListener('click', function () { playId(s.id); });

      cell(s.artist, 'artist');
      cell(fmt(s.duration), 'time');

      var act = cell('', 'actions');
      var del = document.createElement('button');
      del.className = 'del';
      del.textContent = 'Delete';
      del.setAttribute('aria-label', 'Delete ' + s.title);
      del.addEventListener('click', function () {
        api('DELETE', '/api/songs/' + s.id).then(applyState).catch(fail);
      });
      act.appendChild(del);
      body.appendChild(tr);
    });
  }

  function renderList(songs) {
    renderRows(songs);
    var empty = $('empty');
    if (songs.length === 0) {
      empty.hidden = false;
      empty.textContent = searchQuery ? 'No songs match "' + searchQuery + '".' : 'The playlist is empty. Search Spotify to add songs!';
    } else {
      empty.hidden = true;
    }
    var note = $('view-note');
    var msgs = [];
    if (searchQuery) msgs.push('Showing ' + songs.length + ' result(s) for "' + searchQuery + '"');
    if (viewReversed) msgs.push('Viewing in reverse order (tail to head)');
    note.hidden = msgs.length === 0;
    note.textContent = msgs.join(' — ');
    $('btn-view-reverse').setAttribute('aria-pressed', viewReversed ? 'true' : 'false');
  }

  // Now-playing card + seek bar + play button; also called by the 250 ms ticker.
  function renderTransport() {
    var cur = currentSong();
    var active = !!(cur && engine.kind && engine.songId === cur.id);
    var dur = active ? (engine.dur || cur.duration) : (cur ? cur.duration : 0);
    var pos = active ? Math.min(enginePos(), dur) : 0;

    $('np-title').textContent = cur ? cur.title : 'Nothing playing';
    $('np-artist').textContent = cur ? cur.artist : '—';
    var img = $('art-img');
    if (cur && cur.image_url) {
      if (img.getAttribute('src') !== cur.image_url) img.src = cur.image_url;
      img.hidden = false;
      $('art-ph').hidden = true;
    } else {
      img.removeAttribute('src');
      img.hidden = true;
      $('art-ph').hidden = false;
    }
    $('art').classList.toggle('spinning', active && engine.playing);
    $('np-length').textContent = fmt(dur);
    $('btn-repeat').setAttribute('aria-pressed', state.repeat ? 'true' : 'false');
    $('btn-repeat').textContent = '\u21bb Repeat: ' + (state.repeat ? 'On' : 'Off');
    $('btn-play').innerHTML = active && engine.playing ? '&#9208;' : '&#9654;';

    var seek = $('seek');
    seek.disabled = !active;
    seek.max = Math.max(1, Math.round(dur));
    if (!seeking) {
      seek.value = Math.round(pos);
      $('np-elapsed').textContent = fmt(pos);
    }
  }

  function renderStats() {
    $('stat-count').textContent = state.count;
    $('stat-duration').textContent = fmt(state.totalDuration);
  }

  function render() {
    renderStats();
    renderTransport();
    renderResults();
    renderLocal();
    if (searchQuery) return;                       // filter results render themselves
    renderList(viewReversed ? state.songs.slice().reverse() : state.songs);
  }

  // Take the full-state payload the API returns after any change.
  function applyState(data, quiet) {
    if (data.songs) state.songs = data.songs;
    ['count', 'totalDuration', 'currentId', 'repeat'].forEach(function (k) {
      if (data[k] !== undefined) state[k] = data[k];
    });
    if (data.message && !quiet) toast(data.message);
    // the song that was playing got deleted: stop its audio
    if (engine.songId !== null && !state.songs.some(function (s) { return s.id === engine.songId; })) stopEngine();
    render();
    if (searchQuery) runFilter();
  }

  setInterval(function () { renderTransport(); checkSpotifyEnd(); }, 250);

  /* ---------- playlist actions ---------- */

  function runFilter() {
    var mine = ++searchSeq;
    api('GET', '/api/songs/search?q=' + encodeURIComponent(searchQuery)).then(function (d) {
      if (mine !== searchSeq || !searchQuery) return;
      renderList(viewReversed ? d.songs.slice().reverse() : d.songs);
    }).catch(fail);
  }

  function loadAll() {
    return api('GET', '/api/songs').then(function (d) { applyState(d); }).catch(fail);
  }

  $('btn-next').addEventListener('click', function () { SP.activate(); advance('/api/player/next', false); });
  $('btn-prev').addEventListener('click', function () { SP.activate(); advance('/api/player/prev', false); });

  $('btn-play').addEventListener('click', function () {
    var cur = currentSong();
    if (!cur) { toast('Add a song first', true); return; }
    SP.activate();
    if (engine.kind && engine.songId === cur.id) {
      if (engine.kind === 'local') {
        if (audio.paused) audio.play().catch(function () { toast('Could not play the audio file', true); });
        else audio.pause();
      } else {
        SP.toggle().catch(fail);
      }
    } else {
      playSong(cur);
    }
  });

  $('btn-repeat').addEventListener('click', function () {
    api('POST', '/api/player/repeat').then(applyState).catch(fail);
  });

  $('seek').addEventListener('input', function () {
    seeking = true;
    $('np-elapsed').textContent = fmt($('seek').value);
  });
  $('seek').addEventListener('change', function () {
    var sec = parseInt($('seek').value, 10);
    seeking = false;
    if (engine.kind === 'local') {
      audio.currentTime = sec;
    } else if (engine.kind === 'spotify') {
      engine.pos = sec;
      engine.stamp = performance.now();
      SP.seek(sec * 1000).catch(fail);
    }
  });

  try { var saved = localStorage.getItem('volume'); if (saved !== null) $('volume').value = saved; } catch (e) { /* storage blocked */ }
  audio.volume = volume();
  $('volume').addEventListener('input', function () {
    audio.volume = volume();
    SP.setVolume(volume()).catch(function () {});
    try { localStorage.setItem('volume', $('volume').value); } catch (e) { /* ignore */ }
  });

  $('btn-reverse').addEventListener('click', function () {
    api('POST', '/api/playlist/reverse').then(applyState).catch(fail);
  });
  $('btn-view-reverse').addEventListener('click', function () {
    viewReversed = !viewReversed;
    // Fetch from the API so the order really comes from the tail->prev traversal.
    api('GET', viewReversed ? '/api/songs/reverse' : '/api/songs').then(function (d) {
      state.songs = viewReversed ? d.songs.slice().reverse() : d.songs; // keep state in list order
      renderStats();
      if (searchQuery) runFilter(); else renderList(d.songs);
    }).catch(fail);
  });
  $('btn-sort').addEventListener('click', function () {
    api('POST', '/api/playlist/sort', { by: $('sort-by').value }).then(applyState).catch(fail);
  });
  $('btn-shuffle').addEventListener('click', function () {
    api('POST', '/api/playlist/shuffle').then(applyState).catch(fail);
  });
  $('btn-save').addEventListener('click', function () {
    api('POST', '/api/playlist/save').then(function (d) { toast(d.message); }).catch(fail);
  });
  $('btn-load').addEventListener('click', function () {
    api('POST', '/api/playlist/load').then(applyState).catch(fail);
  });

  var filterTimer = null;
  $('search').addEventListener('input', function () {
    clearTimeout(filterTimer);
    filterTimer = setTimeout(function () {
      searchQuery = $('search').value.trim();
      if (searchQuery) runFilter(); else { searchSeq++; render(); }
    }, 250);
  });

  /* ---------- manual add form ---------- */

  $('f-at').addEventListener('change', function () {
    $('pos-label').hidden = $('f-at').value !== 'position';
  });

  $('add-form').addEventListener('submit', function (e) {
    e.preventDefault();
    var min = parseInt($('f-min').value, 10), sec = parseInt($('f-sec').value, 10);
    if (isNaN(min) || isNaN(sec) || min < 0) return toast('Enter valid minutes and seconds', true);
    if (sec < 0 || sec > 59) return toast('Seconds must be between 0 and 59', true);
    if (min === 0 && sec === 0) return toast('Duration must be longer than 0 seconds', true);
    var body = {
      title: $('f-title').value, artist: $('f-artist').value,
      minutes: min, seconds: sec, at: $('f-at').value
    };
    if (body.at === 'position') {
      body.position = parseInt($('f-pos').value, 10);
      if (isNaN(body.position)) return toast('Enter a position', true);
    }
    api('POST', '/api/songs', body).then(function (d) {
      $('f-title').value = '';
      $('f-artist').value = '';
      $('f-title').focus();
      applyState(d);
    }).catch(fail);
  });

  /* ---------- Find songs: shared "add to" options ---------- */

  $('ins-at').addEventListener('change', function () {
    $('ins-pos-label').hidden = $('ins-at').value !== 'position';
  });

  // Adds the insert mode (and position) to a request body; null when the position is invalid.
  function withInsertMode(body) {
    body.at = $('ins-at').value;
    if (body.at === 'position') {
      body.position = parseInt($('ins-pos').value, 10);
      if (isNaN(body.position)) { toast('Enter a position', true); return null; }
    }
    return body;
  }

  function resultRow(img, title, artist, dur, added, onAdd) {
    var li = document.createElement('li');
    li.appendChild(thumb(img, 'thumb big'));
    var info = document.createElement('div');
    info.className = 'info';
    var a = document.createElement('div'); a.className = 'r-title'; a.textContent = title;
    var b = document.createElement('div'); b.className = 'r-artist'; b.textContent = artist;
    info.appendChild(a); info.appendChild(b);
    li.appendChild(info);
    if (dur) {
      var d = document.createElement('span'); d.className = 'r-dur'; d.textContent = fmt(dur);
      li.appendChild(d);
    }
    var btn = document.createElement('button');
    btn.className = 'small';
    btn.textContent = added ? 'Added' : 'Add';
    btn.disabled = added;
    btn.addEventListener('click', function () { onAdd(btn); });
    li.appendChild(btn);
    return li;
  }

  /* ---------- Spotify search ---------- */

  var sp = { q: '', offset: 0, tracks: [], hasMore: false, busy: false, searched: false };

  function inPlaylist(field, value) {
    return state.songs.some(function (s) { return s[field] === value; });
  }

  function renderResults() {
    var ul = $('sp-results');
    ul.textContent = '';
    sp.tracks.forEach(function (t) {
      ul.appendChild(resultRow(t.image_url, t.title, t.artist, t.duration, inPlaylist('spotify_uri', t.uri),
        function (btn) { addSpotifyTrack(t, btn); }));
    });
    if (sp.searched && sp.tracks.length === 0) {
      var li = document.createElement('li');
      li.className = 'none';
      li.textContent = 'No tracks found.';
      ul.appendChild(li);
    }
    $('sp-more').hidden = !sp.hasMore;
    $('sp-more').disabled = sp.busy;
  }

  function runSpotifySearch(more) {
    if (!SP.isConfigured()) return fail(SP.SpotifyError('not_configured'));
    if (!SP.isLoggedIn()) return fail(SP.SpotifyError('not_logged_in'));
    var q = more ? sp.q : $('sp-q').value.trim();
    if (!q) return toast('Type something to search for', true);
    if (!more) sp = { q: q, offset: 0, tracks: [], hasMore: false, busy: false, searched: false };
    sp.busy = true;
    renderResults();
    SP.search(sp.q, sp.offset).then(function (r) {
      sp.tracks = sp.tracks.concat(r.tracks);
      sp.offset += r.tracks.length;
      sp.hasMore = r.hasMore && r.tracks.length > 0;
      sp.searched = true;
    }).catch(fail).then(function () {
      sp.busy = false;
      renderResults();
    });
  }

  $('sp-form').addEventListener('submit', function (e) { e.preventDefault(); runSpotifySearch(false); });
  $('sp-more').addEventListener('click', function () { runSpotifySearch(true); });

  function addSpotifyTrack(t, btn) {
    var body = withInsertMode({
      title: trunc(t.title, 99), artist: trunc(t.artist, 99), duration: t.duration,
      spotify_uri: t.uri, image_url: t.image_url.length < 300 ? t.image_url : '', source: 'spotify'
    });
    if (!body) return;
    btn.disabled = true;
    api('POST', '/api/songs', body).then(applyState).catch(function (e) { btn.disabled = false; fail(e); });
  }

  /* ---------- Local MP3 files (music/ folder) ---------- */

  var localFiles = [];

  function loadLocal() {
    return api('GET', '/api/local-files').then(function (d) {
      localFiles = (d.files || []).slice().sort(function (a, b) { return a.name.localeCompare(b.name); });
      renderLocal();
    }).catch(function () { /* optional feature: stay quiet */ });
  }

  function renderLocal() {
    var ul = $('local-list');
    ul.textContent = '';
    localFiles.forEach(function (f) {
      var base = f.name.replace(/\.mp3$/i, '');
      ul.appendChild(resultRow('', base, 'Local file', 0, inPlaylist('audio_url', f.url),
        function (btn) { addLocalFile(f, btn); }));
    });
    $('local-empty').hidden = localFiles.length > 0;
  }

  function audioDuration(url) {
    return new Promise(function (resolve, reject) {
      var a = new Audio();
      a.preload = 'metadata';
      a.onloadedmetadata = function () {
        if (isFinite(a.duration) && a.duration > 0) resolve(Math.max(1, Math.round(a.duration)));
        else reject(new Error('Could not read the length of that MP3.'));
      };
      a.onerror = function () { reject(new Error('Could not read that MP3 file.')); };
      a.src = url;
    });
  }

  function addLocalFile(f, btn) {
    var base = f.name.replace(/\.mp3$/i, ''), title = base, artist = 'Local file';
    var dash = base.indexOf(' - ');                 // "Artist - Title.mp3"
    if (dash > 0) { artist = base.slice(0, dash); title = base.slice(dash + 3); }
    btn.disabled = true;
    audioDuration(f.url).then(function (duration) {
      var body = withInsertMode({
        title: trunc(title, 99), artist: trunc(artist, 99), duration: duration,
        source: 'local', audio_url: f.url
      });
      if (!body) { btn.disabled = false; return; }
      return api('POST', '/api/songs', body).then(applyState);
    }).catch(function (e) { btn.disabled = false; fail(e); });
  }

  $('local-refresh').addEventListener('click', function () { loadLocal().then(function () { toast('Music folder rescanned'); }); });

  /* ---------- Spotify login ---------- */

  function renderAuth() {
    var configured = SP.isConfigured(), logged = SP.isLoggedIn();
    $('sp-login').hidden = logged;
    $('sp-login').disabled = !configured;
    $('sp-logout').hidden = !logged;
    if (!logged) { $('sp-user').hidden = true; }
  }

  function loadMe() {
    SP.me().then(function (m) {
      $('sp-user').textContent = m.display_name || m.id || 'Spotify user';
      $('sp-user').hidden = false;
      if (m.product && m.product !== 'premium') fail(SP.SpotifyError('premium'));
    }).catch(fail);
  }

  $('sp-login').addEventListener('click', function () { SP.login().catch(fail); });
  $('sp-logout').addEventListener('click', function () {
    stopEngine();
    SP.logout();
    sp = { q: '', offset: 0, tracks: [], hasMore: false, busy: false, searched: false };
    renderAuth();
    renderResults();
    toast('Logged out of Spotify');
  });

  /* ---------- start ---------- */

  renderAuth();
  loadAll();
  loadLocal();
  if (!SP.isConfigured()) showBanner(MSG.not_configured);

  SP.handleCallback().then(function (justLoggedIn) {
    renderAuth();
    if (SP.isLoggedIn()) {
      SP.scheduleRefresh();
      loadMe();
      startSpotifyPlayer().catch(fail);       // duplicate toasts are swallowed
      if (justLoggedIn) toast('Logged in to Spotify');
    }
  }).catch(function (e) { renderAuth(); fail(e); });
})();
