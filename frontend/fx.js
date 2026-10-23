/*
 * fx.js - visual effects only (nothing here touches playlist or Spotify logic):
 *   - drifting music notes in the background
 *   - custom cursor: glowing dot + trailing ring, a short trail of notes, a burst on click
 * Disabled automatically for "reduce motion"; cursor effects are skipped on touch screens.
 */
(function () {
  'use strict';

  var reduce = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  var fine = window.matchMedia && matchMedia('(pointer: fine)').matches;
  var GLYPHS = ['♪', '♫', '♬', '♩'];
  var COLORS = ['179,157,255', '124,92,255', '255,122,217', '110,231,255', '255,214,102'];
  var FONT = '"Segoe UI Symbol","Apple Symbols","Noto Sans Symbols 2",serif';

  function rand(a, b) { return a + Math.random() * (b - a); }
  function pick(list) { return list[Math.floor(Math.random() * list.length)]; }

  function makeCanvas(id, z) {
    var c = document.createElement('canvas');
    c.id = id;
    c.setAttribute('aria-hidden', 'true');
    c.style.cssText = 'position:fixed;left:0;top:0;width:100%;height:100%;pointer-events:none;z-index:' + z;
    document.body.appendChild(c);
    return c;
  }

  var bg = makeCanvas('fx-bg', 0);
  var bctx = bg.getContext('2d');
  var top = null, tctx = null;
  var W = 0, H = 0, dpr = 1;

  /* ---------- background notes ---------- */

  var notes = [];

  function newNote(spread) {
    var depth = Math.random();                       // 0 = far (small, faint), 1 = near
    return {
      x: rand(0, W), y: spread ? rand(0, H) : H + rand(20, 120),
      size: 20 + depth * 46, vy: 12 + depth * 30, amp: rand(10, 40), sway: rand(0.4, 1.1),
      phase: rand(0, 6.28), rot: rand(-0.5, 0.5), vr: rand(-0.25, 0.25),
      glyph: pick(GLYPHS), color: pick(COLORS), alpha: 0.14 + depth * 0.30, baseX: 0
    };
  }

  function seedNotes() {
    var count = Math.max(14, Math.min(34, Math.round(W / 48)));
    notes = [];
    for (var i = 0; i < count; i++) { var n = newNote(true); n.baseX = n.x; notes.push(n); }
  }

  function drawNote(ctx, n, x, y) {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(n.rot);
    ctx.font = n.size + 'px ' + FONT;
    ctx.fillStyle = 'rgba(' + n.color + ',' + n.alpha + ')';
    ctx.fillText(n.glyph, 0, 0);
    ctx.restore();
  }

  function drawBackground(t) {
    bctx.clearRect(0, 0, W, H);
    for (var i = 0; i < notes.length; i++) {
      var n = notes[i];
      drawNote(bctx, n, n.baseX + Math.sin(n.phase + t * n.sway) * n.amp, n.y);
    }
  }

  function stepBackground(dt) {
    for (var i = 0; i < notes.length; i++) {
      var n = notes[i];
      n.y -= n.vy * dt;
      n.rot += n.vr * dt;
      if (n.y < -80) { var f = newNote(false); f.baseX = f.x; notes[i] = f; }
    }
  }

  /* ---------- cursor + particles ---------- */

  var parts = [];
  var mx = -100, my = -100, rx = -100, ry = -100, lastX = -100, lastY = -100;
  var dot = null, ring = null, shown = false;

  function spawn(x, y, vx, vy, size, life) {
    if (parts.length > 120) parts.shift();
    parts.push({ x: x, y: y, vx: vx, vy: vy, size: size, life: life, max: life,
      glyph: pick(GLYPHS), color: pick(COLORS), rot: rand(-0.6, 0.6), vr: rand(-1.5, 1.5) });
  }

  function burst(x, y) {
    for (var i = 0; i < 10; i++) {
      var a = (i / 10) * 6.283 + rand(-0.2, 0.2), s = rand(60, 150);
      spawn(x, y, Math.cos(a) * s, Math.sin(a) * s - 30, rand(14, 26), rand(0.7, 1.2));
    }
  }

  function stepParticles(dt) {
    for (var i = parts.length - 1; i >= 0; i--) {
      var p = parts[i];
      p.life -= dt;
      if (p.life <= 0) { parts.splice(i, 1); continue; }
      p.x += p.vx * dt; p.y += p.vy * dt; p.vy -= 25 * dt; p.vx *= 0.985; p.rot += p.vr * dt;
    }
  }

  function drawParticles() {
    tctx.clearRect(0, 0, W, H);
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i], a = Math.max(0, p.life / p.max);
      tctx.save();
      tctx.translate(p.x, p.y);
      tctx.rotate(p.rot);
      tctx.font = p.size + 'px ' + FONT;
      tctx.shadowColor = 'rgba(' + p.color + ',0.9)';
      tctx.shadowBlur = 10;
      tctx.fillStyle = 'rgba(' + p.color + ',' + (a * 0.9) + ')';
      tctx.fillText(p.glyph, 0, 0);
      tctx.restore();
    }
  }

  var HOT = 'button, a, select, input, label, summary, [role="button"], td.title, .slider';

  function initCursor() {
    top = makeCanvas('fx-top', 9998);
    tctx = top.getContext('2d');
    dot = document.createElement('div'); dot.id = 'cur-dot';
    ring = document.createElement('div'); ring.id = 'cur-ring';
    dot.setAttribute('aria-hidden', 'true'); ring.setAttribute('aria-hidden', 'true');
    document.body.appendChild(ring);
    document.body.appendChild(dot);
    document.documentElement.classList.add('fx-cursor');

    document.addEventListener('mousemove', function (e) {
      mx = e.clientX; my = e.clientY;
      if (!shown) { shown = true; rx = mx; ry = my; dot.style.opacity = ring.style.opacity = '1'; }
      dot.style.transform = 'translate3d(' + mx + 'px,' + my + 'px,0)';
      var dx = mx - lastX, dy = my - lastY;
      if (dx * dx + dy * dy > 38 * 38) {                       // a note every ~38 px of movement
        lastX = mx; lastY = my;
        spawn(mx, my, rand(-14, 14), rand(-40, -15), rand(13, 22), rand(0.8, 1.3));
      }
    }, { passive: true });

    document.addEventListener('mouseover', function (e) {
      ring.classList.toggle('hot', !!(e.target.closest && e.target.closest(HOT)));
    });
    document.addEventListener('mousedown', function () { ring.classList.add('down'); });
    document.addEventListener('mouseup', function () { ring.classList.remove('down'); });
    document.addEventListener('click', function (e) { burst(e.clientX, e.clientY); });
    document.documentElement.addEventListener('mouseleave', function () { shown = false; dot.style.opacity = ring.style.opacity = '0'; });
  }

  /* ---------- main loop ---------- */

  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    W = window.innerWidth; H = window.innerHeight;
    [bg, top].forEach(function (c) { if (c) { c.width = W * dpr; c.height = H * dpr; } });
    bctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (tctx) tctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    seedNotes();
    if (reduce) drawBackground(0);                    // still picture, no animation
  }

  var last = 0;
  function frame(ts) {
    var dt = Math.min(0.05, (ts - last) / 1000 || 0.016);
    last = ts;
    stepBackground(dt);
    drawBackground(ts / 1000);
    if (tctx) {
      rx += (mx - rx) * 0.18; ry += (my - ry) * 0.18;
      ring.style.transform = 'translate3d(' + rx + 'px,' + ry + 'px,0)';
      stepParticles(dt);
      drawParticles();
    }
    requestAnimationFrame(frame);
  }

  if (!reduce && fine) initCursor();
  resize();
  window.addEventListener('resize', resize);
  if (!reduce) requestAnimationFrame(frame);
})();
