/* The landing page's motion, and the only script this server serves.
 *
 * It is written out here rather than pulled from a CDN on purpose. This binary is
 * run on somebody's home network, and a page whose animation arrives from
 * somewhere else is a page that stops moving the day that network has no route
 * out - the same reason the stylesheet, the font and the icons are all embedded.
 * Nothing below needs a library: it is an IntersectionObserver, a scroll handler
 * on a frame, and CSS doing the actual moving.
 *
 * Everything here is an enhancement and none of it is load bearing. The document
 * marks itself `.js` before any of this runs, the stylesheet only hides a thing
 * that is about to be revealed under that class, and `prefers-reduced-motion`
 * leaves the page exactly as it is served. A browser that fails to run this file
 * gets the page with everything already in place.
 */
(function () {
  'use strict';

  var root = document.documentElement;
  var still = window.matchMedia('(prefers-reduced-motion: reduce)');

  /* Reduced motion is not "the same page, faster". Nothing below runs, the
     stylesheet's own guards keep the transitions off, and the page is the page. */
  if (still.matches) {
    root.classList.remove('js');
    return;
  }

  /* --------------------------------------------------------------- reveal */

  /* Each piece arrives as its panel does. Once something has arrived it is left
     alone: a section that fades out again every time it is scrolled past reads as
     a fault rather than as an effect. */
  var reveal = function (el) {
    el.classList.add('is-in');
  };

  var items = document.querySelectorAll('[data-anim]');
  if ('IntersectionObserver' in window) {
    var seen = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        reveal(e.target);
        seen.unobserve(e.target);
      });
    }, { rootMargin: '0px 0px -12% 0px', threshold: 0.08 });

    items.forEach(function (el) {
      /* Anything already on screen at load is not something the reader scrolled
         to, so it plays immediately rather than waiting to be scrolled at. */
      var box = el.getBoundingClientRect();
      if (box.top < window.innerHeight * 0.92) reveal(el);
      else seen.observe(el);
    });
  } else {
    items.forEach(reveal);
  }

  /* The stagger is a per-child delay written onto the child, so the order lives
     in the markup rather than in a pile of :nth-child rules. */
  document.querySelectorAll('[data-stagger]').forEach(function (group) {
    var step = parseFloat(group.getAttribute('data-stagger')) || 70;
    var kids = group.children;
    for (var i = 0; i < kids.length; i++) {
      kids[i].style.setProperty('--d', i * step + 'ms');
    }
  });

  /* ------------------------------------------------------------- on scroll */

  var navBar = document.querySelector('.lnav');
  var panels = [].slice.call(document.querySelectorAll('.lpanel'));
  var floats = [].slice.call(document.querySelectorAll('[data-float]'));
  var dots = [];
  var ticking = false;

  /* One rail of dots down the side, because a page that moves a screen at a time
     should say how many screens there are and which one this is. It is built
     here and not in the template: with no script there is no rail to click. */
  if (panels.length > 1) {
    var rail = document.createElement('nav');
    rail.className = 'lrail';
    rail.setAttribute('aria-hidden', 'true');
    panels.forEach(function (panel, i) {
      var dot = document.createElement('a');
      dot.className = 'lrail-dot';
      dot.href = '#' + panel.id;
      dot.tabIndex = -1;
      dot.innerHTML = '<span class="lrail-n">' + String(i + 1).padStart(2, '0') + '</span>';
      rail.appendChild(dot);
      dots.push(dot);
    });
    document.body.appendChild(rail);
  }

  /* Parallax, kept small on purpose. The number on the element is how far it
     drifts against the scroll; anything past a tenth or so stops reading as depth
     and starts reading as the layout coming apart. */
  var drift = function (mid) {
    floats.forEach(function (el) {
      var factor = parseFloat(el.getAttribute('data-float')) || 0.06;
      var box = el.getBoundingClientRect();
      var from = box.top + box.height / 2 - mid;
      el.style.setProperty('--float', (-from * factor).toFixed(2) + 'px');
    });
  };

  var frame = function () {
    ticking = false;
    var mid = window.innerHeight / 2;

    if (navBar) navBar.classList.toggle('is-past', window.scrollY > 24);

    /* The panel with the most of itself on screen is the one being read. */
    var best = 0;
    var bestSeen = -1;
    panels.forEach(function (panel, i) {
      var box = panel.getBoundingClientRect();
      var visible = Math.min(box.bottom, window.innerHeight) - Math.max(box.top, 0);
      if (visible > bestSeen) {
        bestSeen = visible;
        best = i;
      }
    });
    dots.forEach(function (dot, i) {
      dot.classList.toggle('on', i === best);
    });
    drift(mid);
  };

  var onScroll = function () {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(frame);
  };

  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onScroll);
  frame();

  /* Somebody who turns reduced motion on while the page is open gets the page as
     it would have been served to them. */
  var quiet = function () {
    if (!still.matches) return;
    root.classList.remove('js');
    window.removeEventListener('scroll', onScroll);
  };
  if (still.addEventListener) still.addEventListener('change', quiet);
})();
