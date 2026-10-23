/* Music Playlist Manager - talks to the C backend through fetch(). */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };

  var state = { songs: [], count: 0, totalDuration: 0, currentId: null, repeat: false };
  var viewReversed = false;
  var searchQuery = '';
  var searchSeq = 0;               // ignore out-of-date search responses

  // simulated playback
  var playing = false, elapsed = 0, timer = null, lastCurrentId = null;

  /* ---------- helpers ---------- */

  function fmt(sec) {
    var m = Math.floor(sec / 60), s = sec % 60;
    return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
  }

  function toast(msg, isError) {
    var el = document.createElement('div');
    el.className = 'toast' + (isError ? ' error' : '');
    el.textContent = msg;
    $('toasts').appendChild(el);
    setTimeout(function () { el.remove(); }, 3200);
  }

  // Every call returns parsed JSON; HTTP errors throw with the server's message.
  function api(method, path, body) {
    var opts = { method: method, headers: {} };
    if (body !== undefined) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(path, opts).then(function (res) {
      return res.json().catch(function () { return {}; }).then(function (data) {
        if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
        return data;
      });
    });
  }

  function fail(err) { toast(err.message || 'Request failed', true); }

  /* ---------- rendering ---------- */

  function currentSong() {
    for (var i = 0; i < state.songs.length; i++)
      if (state.songs[i].id === state.currentId) return state.songs[i];
    return null;
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
      var t = cell(s.title, 'title');
      t.title = 'Click to play';
      t.addEventListener('click', function () { playId(s.id); });
      cell(s.artist);
      cell(fmt(s.duration));

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
      empty.textContent = searchQuery ? 'No songs match "' + searchQuery + '".' : 'The playlist is empty. Add your first song!';
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

  function renderPlayer() {
    var cur = currentSong();
    $('np-title').textContent = cur ? cur.title : 'Nothing playing';
    $('np-artist').textContent = cur ? cur.artist : '—';
    $('np-length').textContent = cur ? fmt(cur.duration) : '00:00';
    $('btn-repeat').setAttribute('aria-pressed', state.repeat ? 'true' : 'false');
    if (state.currentId !== lastCurrentId) {       // song changed: restart the clock
      lastCurrentId = state.currentId;
      elapsed = 0;
    }
    updateProgress();
    $('btn-play').innerHTML = playing && cur ? '&#9208;' : '&#9654;';
    $('disc').classList.toggle('spinning', playing && !!cur);
  }

  function renderStats() {
    $('stat-count').textContent = state.count;
    $('stat-duration').textContent = fmt(state.totalDuration);
  }

  function render() {
    renderStats();
    renderPlayer();
    if (searchQuery) return;                       // search results render themselves
    if (viewReversed) {
      var rev = state.songs.slice().reverse();     // same data the API returns for /reverse
      renderList(rev);
    } else {
      renderList(state.songs);
    }
  }

  // Take the full-state payload the API returns after any change.
  function applyState(data) {
    if (data.songs) state.songs = data.songs;
    ['count', 'totalDuration', 'currentId', 'repeat'].forEach(function (k) {
      if (data[k] !== undefined) state[k] = data[k];
    });
    if (data.message) toast(data.message);
    render();
    if (searchQuery) runSearch();
  }

  /* ---------- simulated playback ---------- */

  function updateProgress() {
    var cur = currentSong();
    var len = cur ? cur.duration : 0;
    $('progress-bar').style.width = len ? Math.min(100, elapsed / len * 100) + '%' : '0';
    $('np-elapsed').textContent = fmt(Math.floor(elapsed));
  }

  function tick() {
    var cur = currentSong();
    if (!playing || !cur) return;
    elapsed += 0.25;
    if (elapsed >= cur.duration) {                 // song finished: go to the next one
      elapsed = 0;
      api('POST', '/api/player/next').then(applyState).catch(function () {
        playing = false;                           // end of list with repeat off
        renderPlayer();
        toast('Reached the end of the playlist');
      });
      return;
    }
    updateProgress();
  }

  function setPlaying(on) {
    playing = on;
    clearInterval(timer);
    if (on) timer = setInterval(tick, 250);
    renderPlayer();
  }

  /* ---------- actions ---------- */

  function playId(id) {
    api('POST', '/api/player/play/' + id).then(function (d) {
      elapsed = 0;
      applyState(d);
      setPlaying(true);
    }).catch(fail);
  }

  function move(path) {
    api('POST', path).then(function (d) {
      elapsed = 0;
      applyState(d);
    }).catch(fail);
  }

  function runSearch() {
    var mine = ++searchSeq;
    api('GET', '/api/songs/search?q=' + encodeURIComponent(searchQuery)).then(function (d) {
      if (mine !== searchSeq || !searchQuery) return;
      var songs = viewReversed ? d.songs.slice().reverse() : d.songs;
      renderList(songs);
    }).catch(fail);
  }

  function loadAll() {
    var path = '/api/songs';
    return api('GET', path).then(function (d) { applyState(d); }).catch(fail);
  }

  /* ---------- wiring ---------- */

  $('btn-next').addEventListener('click', function () { move('/api/player/next'); });
  $('btn-prev').addEventListener('click', function () { move('/api/player/prev'); });
  $('btn-play').addEventListener('click', function () {
    if (!currentSong()) { toast('Add a song first', true); return; }
    setPlaying(!playing);
  });
  $('btn-repeat').addEventListener('click', function () {
    api('POST', '/api/player/repeat').then(applyState).catch(fail);
  });

  $('btn-reverse').addEventListener('click', function () {
    api('POST', '/api/playlist/reverse').then(applyState).catch(fail);
  });
  $('btn-view-reverse').addEventListener('click', function () {
    viewReversed = !viewReversed;
    // Fetch from the API so the order really comes from the tail->prev traversal.
    api('GET', viewReversed ? '/api/songs/reverse' : '/api/songs').then(function (d) {
      state.songs = viewReversed ? d.songs.slice().reverse() : d.songs; // keep state in list order
      if (searchQuery) { renderStats(); runSearch(); }
      else { renderStats(); renderList(d.songs); }
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

  var searchTimer = null;
  $('search').addEventListener('input', function () {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(function () {
      searchQuery = $('search').value.trim();
      if (searchQuery) runSearch(); else { searchSeq++; render(); }
    }, 250);
  });

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

  loadAll();
})();
