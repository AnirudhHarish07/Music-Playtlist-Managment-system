/*
 * spotify.js - everything that talks to Spotify, in the browser only.
 *
 *   - Authorization Code flow with PKCE (no client secret anywhere)
 *   - token storage + refresh before expiry
 *   - Web API calls (search, play, transfer playback) with friendly errors
 *   - Web Playback SDK player
 *
 * The C backend never sees a token.  It only stores spotify_uri strings and
 * decides which song is next/previous; app.js then asks this file to play it.
 */
(function () {
  'use strict';

  var cfg = window.SPOTIFY_CONFIG || {};
  var REDIRECT_URI = cfg.redirectUri || 'http://127.0.0.1:8080/callback';
  var SCOPES = 'streaming user-read-email user-read-private user-playback-state user-modify-playback-state';
  var AUTH_URL = 'https://accounts.spotify.com/authorize';
  var TOKEN_URL = 'https://accounts.spotify.com/api/token';
  var API = 'https://api.spotify.com/v1';
  var TOKEN_KEY = 'sp_token';

  /* ---------- errors with messages meant for the user ---------- */

  var MESSAGES = {
    not_configured: 'Spotify is not configured. Copy frontend/config.example.js to frontend/config.js and set your Client ID.',
    not_logged_in: 'You are not logged in to Spotify. Click "Login with Spotify" first.',
    expired: 'Your Spotify session expired. Please log in again.',
    premium: 'Spotify Premium is required to play music in the browser. Search still works, playback does not.',
    drm: 'This browser cannot play Spotify audio (no DRM / Widevine support). Please use Chrome or Edge.',
    network: 'Network problem: could not reach Spotify. Check your internet connection and try again.',
    forbidden: 'Spotify refused the request (403). In Development Mode your account must be added under "User Management" in the Spotify Dashboard.'
  };

  function SpotifyError(code, message, extra) {
    var e = new Error(message || MESSAGES[code] || 'Spotify error');
    e.name = 'SpotifyError';
    e.code = code;
    if (extra) for (var k in extra) e[k] = extra[k];
    return e;
  }

  /* ---------- token storage ---------- */

  function loadToken() {
    try { return JSON.parse(localStorage.getItem(TOKEN_KEY)); } catch (e) { return null; }
  }
  function saveToken(t) {
    try { localStorage.setItem(TOKEN_KEY, JSON.stringify(t)); } catch (e) { /* storage blocked */ }
    memToken = t;
  }
  function clearToken() {
    try { localStorage.removeItem(TOKEN_KEY); } catch (e) { /* ignore */ }
    memToken = null;
  }
  var memToken = loadToken();

  function isConfigured() {
    return !!cfg.clientId && cfg.clientId !== 'YOUR_SPOTIFY_CLIENT_ID';
  }
  function isLoggedIn() { return !!(memToken && memToken.refresh); }

  /* ---------- PKCE ---------- */

  function randomString(len) {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~';
    var bytes = new Uint8Array(len), out = '';
    crypto.getRandomValues(bytes);
    for (var i = 0; i < len; i++) out += chars[bytes[i] % chars.length];
    return out;
  }

  function base64url(buf) {
    var s = '', bytes = new Uint8Array(buf);
    for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }

  function sha256(text) {
    return crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  }

  // Redirects the browser to Spotify's consent page.
  function login() {
    if (!isConfigured()) return Promise.reject(SpotifyError('not_configured'));
    if (!window.crypto || !crypto.subtle)
      return Promise.reject(SpotifyError('api', 'Login needs a secure context. Open the app at http://127.0.0.1:8080 (not a LAN address).'));
    var verifier = randomString(64), state = randomString(16);
    return sha256(verifier).then(function (hash) {
      try {
        sessionStorage.setItem('sp_verifier', verifier);
        sessionStorage.setItem('sp_state', state);
      } catch (e) { throw SpotifyError('api', 'Browser storage is blocked; cannot log in.'); }
      var q = new URLSearchParams({
        client_id: cfg.clientId, response_type: 'code', redirect_uri: REDIRECT_URI,
        code_challenge_method: 'S256', code_challenge: base64url(hash),
        scope: SCOPES, state: state
      });
      window.location.assign(AUTH_URL + '?' + q.toString());
    });
  }

  function tokenRequest(params) {
    var body = new URLSearchParams(params);
    return fetch(TOKEN_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString()
    }).catch(function () { throw SpotifyError('network'); })
      .then(function (res) {
        return res.json().catch(function () { return {}; }).then(function (d) {
          if (!res.ok) throw SpotifyError('token_http', null, { status: res.status, detail: d.error_description || d.error });
          return d;
        });
      });
  }

  function storeTokenResponse(d, oldRefresh) {
    saveToken({
      access: d.access_token,
      refresh: d.refresh_token || oldRefresh,       // refresh responses may omit a new refresh token
      expires_at: Date.now() + (d.expires_in || 3600) * 1000
    });
    scheduleRefresh();
  }

  // Call once on page load.  On /callback it finishes the login; resolves true if logged in just now.
  function handleCallback() {
    if (window.location.pathname !== '/callback') return Promise.resolve(false);
    var p = new URLSearchParams(window.location.search);
    var code = p.get('code'), err = p.get('error'), state = p.get('state');
    var verifier = null, expected = null;
    try { verifier = sessionStorage.getItem('sp_verifier'); expected = sessionStorage.getItem('sp_state'); } catch (e) { /* ignore */ }
    try { sessionStorage.removeItem('sp_verifier'); sessionStorage.removeItem('sp_state'); } catch (e) { /* ignore */ }
    history.replaceState(null, '', '/');            // never leave the code in the address bar

    if (err) return Promise.reject(SpotifyError('api', err === 'access_denied' ? 'Login was cancelled.' : 'Spotify login failed: ' + err));
    if (!code || !verifier || !state || state !== expected)
      return Promise.reject(SpotifyError('api', 'Login could not be verified. Please try logging in again.'));
    return tokenRequest({
      client_id: cfg.clientId, grant_type: 'authorization_code', code: code,
      redirect_uri: REDIRECT_URI, code_verifier: verifier
    }).then(function (d) { storeTokenResponse(d); return true; })
      .catch(function (e) {
        if (e.code === 'token_http') throw SpotifyError('api', 'Spotify rejected the login (' + (e.detail || e.status) + '). Check the Client ID and the Redirect URI in the Dashboard.');
        throw e;
      });
  }

  var refreshing = null, refreshTimer = null;

  function refreshToken() {
    if (refreshing) return refreshing;
    if (!memToken || !memToken.refresh) return Promise.reject(SpotifyError('not_logged_in'));
    var old = memToken.refresh;
    refreshing = tokenRequest({ grant_type: 'refresh_token', refresh_token: old, client_id: cfg.clientId })
      .then(function (d) { storeTokenResponse(d, old); return memToken.access; })
      .catch(function (e) {
        if (e.code === 'token_http') { clearToken(); throw SpotifyError('expired'); }
        throw e;
      })
      .then(function (v) { refreshing = null; return v; }, function (e) { refreshing = null; throw e; });
    return refreshing;
  }

  // Refresh one minute before the token runs out.
  function scheduleRefresh() {
    clearTimeout(refreshTimer);
    if (!memToken) return;
    var wait = Math.max(5000, memToken.expires_at - Date.now() - 60000);
    refreshTimer = setTimeout(function () { refreshToken().catch(function () { /* surfaced on next use */ }); }, wait);
  }

  // Always returns a valid access token (refreshing if it is about to expire).
  function getToken() {
    if (!isLoggedIn()) return Promise.reject(SpotifyError('not_logged_in'));
    if (memToken.expires_at - Date.now() < 60000) return refreshToken();
    return Promise.resolve(memToken.access);
  }

  function logout() {
    clearTimeout(refreshTimer);
    clearToken();
    if (player) { try { player.disconnect(); } catch (e) { /* ignore */ } }
    player = null; deviceId = null; readyPromise = null;
  }

  /* ---------- Web API ---------- */

  function apiFetch(method, path, body, isRetry) {
    return getToken().then(function (token) {
      var opts = { method: method, headers: { Authorization: 'Bearer ' + token } };
      if (body !== undefined) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
      return fetch(API + path, opts).catch(function () { throw SpotifyError('network'); });
    }).then(function (res) {
      if (res.status === 401 && !isRetry) {          // token rejected: force one refresh, retry once
        if (memToken) memToken.expires_at = 0;
        return refreshToken().then(function () { return apiFetch(method, path, body, true); });
      }
      if (res.status === 204) return null;
      return res.json().catch(function () { return {}; }).then(function (d) {
        if (res.ok) return d;
        var msg = d && d.error && d.error.message, reason = d && d.error && d.error.reason;
        if (res.status === 401) { clearToken(); throw SpotifyError('expired'); }
        if (res.status === 429) {
          var wait = parseInt(res.headers.get('Retry-After'), 10);
          throw SpotifyError('rate_limit', 'Spotify rate limit reached (429). ' +
            (wait ? 'Try again in ' + wait + ' seconds.' : 'Wait a little and try again.'), { retryAfter: wait || null });
        }
        if (res.status === 403) {
          if (reason === 'PREMIUM_REQUIRED' || /premium/i.test(msg || '')) throw SpotifyError('premium');
          throw SpotifyError('forbidden', MESSAGES.forbidden + (msg ? ' (' + msg + ')' : ''), { status: 403 });
        }
        throw SpotifyError('api', 'Spotify error ' + res.status + (msg ? ': ' + msg : ''), { status: res.status, reason: reason });
      });
    });
  }

  function pickImage(images) {
    if (!images || !images.length) return '';
    var sorted = images.slice().sort(function (a, b) { return (a.width || 0) - (b.width || 0); });
    for (var i = 0; i < sorted.length; i++) if ((sorted[i].width || 0) >= 200) return sorted[i].url;
    return sorted[sorted.length - 1].url;
  }

  // One page of results (Spotify allows at most 10 per request here).
  function search(q, offset) {
    var qs = new URLSearchParams({ q: q, type: 'track', limit: '10', offset: String(offset || 0) });
    return apiFetch('GET', '/search?' + qs.toString()).then(function (d) {
      var t = (d && d.tracks) || { items: [] };
      var items = (t.items || []).filter(Boolean).map(function (tr) {
        return {
          uri: tr.uri,
          title: tr.name,
          artist: (tr.artists || []).map(function (a) { return a.name; }).join(', '),
          duration: Math.max(1, Math.round((tr.duration_ms || 0) / 1000)),
          image_url: pickImage(tr.album && tr.album.images)
        };
      });
      var hasMore = t.next ? true : (t.next === undefined && items.length === 10);
      return { tracks: items, hasMore: hasMore, total: t.total };
    });
  }

  function me() { return apiFetch('GET', '/me'); }

  /* ---------- Web Playback SDK ---------- */

  var player = null, deviceId = null, readyPromise = null;
  var handlers = { state: function () {}, ended: function () {}, error: function () {}, ready: function () {} };
  var endHandled = false;
  var initialVolume = 0.8;

  // A finished track shows up as: paused, position 0, and the current track also listed in previous_tracks.
  function isTrackEnd(state) {
    if (!state || !state.paused || state.position !== 0) return false;
    var tw = state.track_window;
    if (!tw || !tw.current_track) return false;
    return (tw.previous_tracks || []).some(function (t) { return t.id === tw.current_track.id; });
  }

  function drmCheck() {
    if (!navigator.requestMediaKeySystemAccess) return Promise.reject(SpotifyError('drm'));
    return navigator.requestMediaKeySystemAccess('com.widevine.alpha', [{
      initDataTypes: ['cenc'],
      audioCapabilities: [{ contentType: 'audio/mp4;codecs="mp4a.40.2"' }]
    }]).catch(function () { throw SpotifyError('drm'); });
  }

  function loadSdkScript() {
    return new Promise(function (resolve, reject) {
      if (window.Spotify) return resolve();
      window.onSpotifyWebPlaybackSDKReady = resolve;
      var s = document.createElement('script');
      s.src = 'https://sdk.scdn.co/spotify-player.js';
      s.async = true;
      s.onerror = function () { reject(SpotifyError('network', 'Could not load the Spotify player script (sdk.scdn.co). Check your connection or ad blocker.')); };
      document.head.appendChild(s);
    });
  }

  // Creates the in-browser player.  Resolves with the device id once Spotify reports it ready.
  function initPlayer(h, volume) {
    if (readyPromise) return readyPromise;
    handlers = Object.assign(handlers, h || {});
    if (typeof volume === 'number') initialVolume = volume;

    readyPromise = drmCheck().then(loadSdkScript).then(function () {
      return new Promise(function (resolve, reject) {
        player = new window.Spotify.Player({
          name: 'Playlist Manager (C linked list)',
          getOAuthToken: function (cb) {
            getToken().then(cb).catch(function (e) { handlers.error(e); });
          },
          volume: initialVolume
        });

        player.addListener('ready', function (d) {
          deviceId = d.device_id;
          // make our browser the active device (does not start anything yet)
          apiFetch('PUT', '/me/player', { device_ids: [deviceId], play: false })
            .catch(function (e) { if (e.code !== 'api') handlers.error(e); });
          handlers.ready(deviceId);
          resolve(deviceId);
        });
        player.addListener('not_ready', function () { deviceId = null; });
        player.addListener('initialization_error', function () { var e = SpotifyError('drm'); handlers.error(e); reject(e); });
        player.addListener('authentication_error', function () {
          // the SDK could not use our token: try one refresh, otherwise ask for a new login
          refreshToken().catch(function (e) { handlers.error(e); });
        });
        player.addListener('account_error', function () { var e = SpotifyError('premium'); handlers.error(e); reject(e); });
        player.addListener('playback_error', function (d) {
          handlers.error(SpotifyError('api', 'Spotify playback error: ' + ((d && d.message) || 'unknown')));
        });
        player.addListener('player_state_changed', function (state) {
          if (!state) return;
          handlers.state({
            position: state.position, duration: state.duration, paused: state.paused,
            uri: state.track_window && state.track_window.current_track && state.track_window.current_track.uri
          });
          if (isTrackEnd(state)) {
            if (!endHandled) { endHandled = true; handlers.ended(); }
          }
        });
        player.connect().then(function (ok) {
          if (!ok) { var e = SpotifyError('api', 'Could not connect the Spotify player. Are you logged in with a Premium account?'); handlers.error(e); reject(e); }
        });
      });
    });
    var attempt = readyPromise;
    attempt.catch(function () {                      // failed: tidy up so the next play attempt can retry
      if (readyPromise === attempt) {
        if (player) { try { player.disconnect(); } catch (e) { /* ignore */ } }
        player = null; deviceId = null; readyPromise = null;
      }
    });
    return attempt;
  }

  function ensureDevice() {
    if (!isLoggedIn()) return Promise.reject(SpotifyError('not_logged_in'));
    if (!readyPromise) return Promise.reject(SpotifyError('api', 'The Spotify player is not started yet.'));
    return Promise.race([
      readyPromise,
      new Promise(function (_, rej) { setTimeout(function () { rej(SpotifyError('api', 'The Spotify player is still starting. Try again in a moment.')); }, 15000); })
    ]);
  }

  // Plays exactly one track URI on the browser device.
  function play(uri) {
    endHandled = false;
    return ensureDevice().then(function (id) {
      var url = '/me/player/play?device_id=' + encodeURIComponent(id);
      return apiFetch('PUT', url, { uris: [uri] }).catch(function (e) {
        if (e.status !== 404) throw e;               // "device not found": transfer once, then retry
        return apiFetch('PUT', '/me/player', { device_ids: [id], play: false })
          .then(function () { return apiFetch('PUT', url, { uris: [uri] }); });
      });
    });
  }

  function guard(fn) { return player ? fn(player) : Promise.resolve(); }
  function pause() { return guard(function (p) { return p.pause(); }); }
  function resume() { return guard(function (p) { return p.resume(); }); }
  function toggle() { return guard(function (p) { return p.togglePlay(); }); }
  function seek(ms) { return guard(function (p) { return p.seek(ms); }); }
  function setVolume(v) { initialVolume = v; return guard(function (p) { return p.setVolume(v); }); }
  function activate() { if (player && player.activateElement) { try { player.activateElement(); } catch (e) { /* ignore */ } } }
  function hasPlayer() { return !!player; }

  window.SP = {
    isConfigured: isConfigured, isLoggedIn: isLoggedIn, login: login, logout: logout,
    handleCallback: handleCallback, scheduleRefresh: scheduleRefresh, getToken: getToken,
    search: search, me: me,
    initPlayer: initPlayer, play: play, pause: pause, resume: resume, toggle: toggle,
    seek: seek, setVolume: setVolume, activate: activate, hasPlayer: hasPlayer,
    isTrackEnd: isTrackEnd, SpotifyError: SpotifyError, MESSAGES: MESSAGES
  };
})();
