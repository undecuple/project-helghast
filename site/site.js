/* Project Helghast site: optional enhancement. The page is complete without this file.
   Same origin, no network calls, no storage, no analytics.
   What it does: shows one section at a time like a front-end menu, reads the arrow keys, Enter, Esc and a game pad,
   and opens every screenshot in an in-page viewer (previous / next, swipe, counter, #shot-N link). */
(function () {
  'use strict';
  var root = document.documentElement;
  root.classList.add('js');

  function ready(fn) {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', fn);
    else fn();
  }

  ready(function () {
    var menu = document.getElementById('menu');
    var panel = document.getElementById('panel');
    var detail = document.getElementById('detail');
    var app = document.getElementById('app');
    if (!menu || !panel || !app) return;
    var links = Array.prototype.slice.call(menu.querySelectorAll('ol a'));
    var secs = Array.prototype.slice.call(panel.querySelectorAll(':scope > .sec'));
    var current = null;
    var narrow = window.matchMedia('(max-width: 860px)');
    var reduce = window.matchMedia('(prefers-reduced-motion: reduce)');

    function idOf(link) { return link.getAttribute('href').slice(1); }

    /* The section a hash points at: the section itself, or the one that contains the target (a #changelog, a #shot-3). */
    function sectionFor(hash) {
      var id = decodeURIComponent((hash || '').replace(/^#/, ''));
      if (!id) return { sec: secs[0], target: null };
      var el = document.getElementById(id);
      if (!el) return { sec: secs[0], target: null };
      var sec = el.closest('.sec');
      return { sec: sec || secs[0], target: el === sec ? null : el };
    }

    function show(sec, target, focusMenu) {
      secs.forEach(function (s) { s.classList.toggle('on', s === sec); });
      links.forEach(function (a) {
        var on = idOf(a) === sec.id;
        a.classList.toggle('on', on);
        if (on) {
          a.setAttribute('aria-current', 'true');
          if (detail) detail.textContent = a.getAttribute('data-detail') || '';
        } else {
          a.removeAttribute('aria-current');
        }
      });
      if (current !== sec) {
        current = sec;
        panel.scrollTop = 0;
        if (narrow.matches && !focusMenu && root.classList.contains('ready')) {
          var top = panel.getBoundingClientRect().top + window.pageYOffset - 8;
          window.scrollTo(0, Math.max(0, top));
        }
      }
      if (target && !/^shot-\d+$/.test(target.id)) {
        target.scrollIntoView({ block: 'start', behavior: reduce.matches ? 'auto' : 'smooth' });
      }
    }

    function fromHash() {
      var r = sectionFor(location.hash);
      show(r.sec, r.target, false);
    }

    function selectIndex(i, focus) {
      if (i < 0) i = links.length - 1;
      if (i >= links.length) i = 0;
      location.replace('#' + idOf(links[i]));   /* the section follows the hash; no history entry per step */
      if (focus) links[i].focus();
    }

    function currentIndex() {
      var i = links.indexOf(document.activeElement);
      if (i >= 0) return i;
      for (i = 0; i < links.length; i++) if (links[i].classList.contains('on')) return i;
      return 0;
    }

    function inMenuOrBody() {
      var a = document.activeElement;
      return !a || a === document.body || menu.contains(a);
    }

    function openPanel() { panel.focus({ preventScroll: true }); }
    function backToMenu() {
      var i = currentIndex();
      links[i].focus();
    }

    /* ---- screenshot viewer ---- */
    var shots = Array.prototype.slice.call(document.querySelectorAll('a.shot'));
    function groupOf(a) {
      var box = a.closest('[data-group]');
      return box ? box.getAttribute('data-group') : 'screens';
    }
    var groups = {};
    shots.forEach(function (a) {
      var g = groupOf(a);
      (groups[g] = groups[g] || []).push(a);
    });
    var viewer = null, vImg, vCap, vCount, vPrev, vNext, vClose, vTouchX = null;
    var vGroup = null, vIndex = -1, vPushed = false, vOpen = false;

    function buildViewer() {
      viewer = document.createElement('div');
      viewer.className = 'viewer';
      viewer.setAttribute('role', 'dialog');
      viewer.setAttribute('aria-modal', 'true');
      viewer.setAttribute('aria-label', 'Screenshot viewer');
      viewer.hidden = true;
      viewer.innerHTML =
        '<div class="v-bar"><span class="v-count" aria-live="polite"></span>' +
        '<button type="button" class="v-close" aria-label="Close the viewer">Close <kbd>Esc</kbd></button></div>' +
        '<div class="v-stage"><button type="button" class="v-prev" aria-label="Previous screenshot">&lsaquo;</button>' +
        '<figure><img alt=""><figcaption></figcaption></figure>' +
        '<button type="button" class="v-next" aria-label="Next screenshot">&rsaquo;</button></div>';
      document.body.appendChild(viewer);
      vImg = viewer.querySelector('img');
      vCap = viewer.querySelector('figcaption');
      vCount = viewer.querySelector('.v-count');
      vPrev = viewer.querySelector('.v-prev');
      vNext = viewer.querySelector('.v-next');
      vClose = viewer.querySelector('.v-close');
      vPrev.addEventListener('click', function () { step(-1); });
      vNext.addEventListener('click', function () { step(1); });
      vClose.addEventListener('click', closeViewer);
      viewer.addEventListener('click', function (e) {
        if (e.target === viewer || e.target.classList.contains('v-stage')) closeViewer();
      });
      viewer.addEventListener('touchstart', function (e) {
        vTouchX = e.touches.length === 1 ? e.touches[0].clientX : null;
      }, { passive: true });
      viewer.addEventListener('touchend', function (e) {
        if (vTouchX === null || !e.changedTouches.length) return;
        var dx = e.changedTouches[0].clientX - vTouchX;
        vTouchX = null;
        if (Math.abs(dx) > 50) step(dx < 0 ? 1 : -1);
      });
    }

    function shotId(a) { var f = a.closest('figure'); return f ? f.id : ''; }

    function paint() {
      var list = groups[vGroup], a = list[vIndex];
      vImg.src = a.getAttribute('href');
      vImg.alt = a.getAttribute('data-caption') || '';
      vCap.textContent = '';
      var b = document.createElement('b');
      b.textContent = a.getAttribute('data-title') || '';
      vCap.appendChild(b);
      vCap.appendChild(document.createTextNode(' ' + (a.getAttribute('data-caption') || '')));
      vCount.textContent = (vIndex + 1) + ' / ' + list.length;
      var one = list.length < 2;
      vPrev.disabled = one;
      vNext.disabled = one;
      /* load the images around this one in the background */
      [-1, 1].forEach(function (d) {
        var n = list[(vIndex + d + list.length) % list.length];
        if (n && n !== a) (new Image()).src = n.getAttribute('href');
      });
    }

    function setHash(id, push) {
      var url = location.pathname + location.search + '#' + id;
      if (push) { history.pushState(null, '', url); vPushed = true; }
      else history.replaceState(null, '', url);
    }

    function openViewer(a, fromHash) {
      if (!viewer) buildViewer();
      var g = groupOf(a);
      vGroup = g;
      vIndex = groups[g].indexOf(a);
      var was = vOpen;
      vOpen = true;
      paint();
      if (!was) {
        viewer.hidden = false;
        app.setAttribute('inert', '');
        app.setAttribute('aria-hidden', 'true');
        document.body.classList.add('v-open');
        vClose.focus();
      }
      var id = shotId(a);
      if (id && !fromHash) setHash(id, !was);   /* opening adds one history entry (Back closes); stepping replaces it */
    }

    function hideViewer() {
      if (!vOpen) return;
      vOpen = false;
      vPushed = false;
      viewer.hidden = true;
      app.removeAttribute('inert');
      app.removeAttribute('aria-hidden');
      document.body.classList.remove('v-open');
      var a = groups[vGroup][vIndex];
      if (a) a.focus({ preventScroll: false });
    }

    function closeViewer() {
      if (!vOpen) return;
      if (vPushed) { vPushed = false; history.back(); }   /* the popstate below hides it */
      else { hideViewer(); setHash('shots', false); }
    }

    function step(d) {
      if (!vOpen) return;
      var list = groups[vGroup];
      vIndex = (vIndex + d + list.length) % list.length;
      paint();
      var id = shotId(list[vIndex]);
      if (id) setHash(id, false);
    }

    shots.forEach(function (a) {
      a.addEventListener('click', function (e) {
        if (e.button !== 0 || e.ctrlKey || e.metaKey || e.shiftKey || e.altKey) return;   /* new tab etc. keep the plain link */
        e.preventDefault();
        openViewer(a, false);
      });
    });

    function viewerFromHash() {
      var m = /^#shot-(\d+)$/.exec(location.hash);
      if (m) {
        var el = document.getElementById('shot-' + m[1]);
        var a = el && el.querySelector('a.shot');
        if (a) { openViewer(a, true); return true; }
      }
      if (vOpen) hideViewer();
      return false;
    }

    function onNav() {
      if (viewerFromHash()) { var r = sectionFor('#shots'); show(r.sec, null, false); return; }
      fromHash();
    }
    window.addEventListener('hashchange', onNav);
    window.addEventListener('popstate', onNav);

    /* ---- keys ---- */
    document.addEventListener('keydown', function (e) {
      if (e.altKey || e.ctrlKey || e.metaKey) return;
      var k = e.key;
      if (vOpen) {
        if (k === 'Escape') { e.preventDefault(); closeViewer(); }
        else if (k === 'ArrowLeft') { e.preventDefault(); step(-1); }
        else if (k === 'ArrowRight') { e.preventDefault(); step(1); }
        else if (k === 'Tab') {   /* focus trap: Tab cycles through the viewer's buttons only */
          var f = [vClose, vPrev, vNext].filter(function (b) { return !b.disabled; });
          var i = f.indexOf(document.activeElement);
          e.preventDefault();
          f[(i + (e.shiftKey ? -1 : 1) + f.length) % f.length].focus();
        }
        return;
      }
      if (narrow.matches && k !== 'Escape') return;   /* on a phone the page scrolls; the keys keep their usual job */
      if (k === 'ArrowDown' && inMenuOrBody()) { e.preventDefault(); selectIndex(currentIndex() + 1, true); }
      else if (k === 'ArrowUp' && inMenuOrBody()) { e.preventDefault(); selectIndex(currentIndex() - 1, true); }
      else if (k === 'Enter' && menu.contains(document.activeElement)) {
        /* the link opens the section (the hash); then the focus moves into it so the arrows scroll it */
        window.setTimeout(openPanel, 0);
      } else if (k === 'Escape' && panel.contains(document.activeElement)) { e.preventDefault(); backToMenu(); }
    });

    /* ---- game pad (optional): d-pad up/down/left/right, A (0) = Enter, B (1) = Esc ---- */
    var pads = 0, last = {}, lastTime = {};
    function pressed(gp, i) { return gp.buttons[i] && gp.buttons[i].pressed; }
    function fire(name, now, repeat) {
      if (last[name] && !(repeat && now - lastTime[name] > 260)) return false;
      last[name] = true; lastTime[name] = now;
      return true;
    }
    function poll() {
      var list = navigator.getGamepads ? navigator.getGamepads() : [];
      var now = Date.now(), gp = null, i;
      for (i = 0; i < list.length; i++) if (list[i]) { gp = list[i]; break; }
      if (gp) {
        var st = {
          up: pressed(gp, 12) || (gp.axes[1] || 0) < -0.6, down: pressed(gp, 13) || (gp.axes[1] || 0) > 0.6,
          left: pressed(gp, 14) || (gp.axes[0] || 0) < -0.6, right: pressed(gp, 15) || (gp.axes[0] || 0) > 0.6,
          a: pressed(gp, 0), b: pressed(gp, 1)
        };
        Object.keys(st).forEach(function (n) {
          if (!st[n]) { last[n] = false; return; }
          if (!fire(n, now, n !== 'a' && n !== 'b')) return;
          if (vOpen) {
            if (n === 'left') step(-1); else if (n === 'right') step(1); else if (n === 'b') closeViewer();
          } else if (n === 'down') selectIndex(currentIndex() + 1, true);
          else if (n === 'up') selectIndex(currentIndex() - 1, true);
          else if (n === 'a') {
            var el = document.activeElement;
            if (el && el !== document.body && el.click) el.click(); else openPanel();
            if (menu.contains(el)) window.setTimeout(openPanel, 0);
          } else if (n === 'b') backToMenu();
        });
      }
      if (pads > 0) window.requestAnimationFrame(poll);
    }
    window.addEventListener('gamepadconnected', function () {
      pads += 1;
      if (pads === 1) window.requestAnimationFrame(poll);
    });
    window.addEventListener('gamepaddisconnected', function () { pads = Math.max(0, pads - 1); });

    /* first paint: the hash decides (a viewer link opens the viewer) */
    if (!viewerFromHash()) fromHash();
    else { var r0 = sectionFor('#shots'); show(r0.sec, null, true); }
    root.classList.add('ready');
  });
})();
