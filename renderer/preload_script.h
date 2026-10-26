// Copyright 2026 The Umbra Authors. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UMBRA_RENDERER_PRELOAD_SCRIPT_H_
#define UMBRA_RENDERER_PRELOAD_SCRIPT_H_

namespace umbra {

// Runs in an isolated world on top-level http(s) documents. Two jobs:
//
// 1. Speculation rules for same-origin links. "moderate" eagerness means
//    Chromium starts the load when you hover a link for ~200ms on desktop, or
//    on pointerdown / viewport heuristics on Android. With Tor adding a few
//    hundred ms per round trip, starting early is the single biggest
//    perceived-speed win. Prerender fully renders the page so the click swaps
//    it in instantly; prefetch is the cheaper fallback. Same-origin only:
//    cross-site preloads would contact sites you never chose to visit.
//    Links that look like they change state (logout, delete, cart...) are
//    excluded.
//
// 2. Videos without an explicit preload="none" get preload="auto", so the
//    media pipeline starts buffering before you press play.
//
// Placeholders __PRELOAD__ / __VIDEO__ are replaced with true/false at
// injection time.
inline constexpr char kPreloadScript[] = R"JS(
(() => {
  const ENABLE_PRELOAD = __PRELOAD__;
  const ENABLE_VIDEO = __VIDEO__;
  const doc = document;
  if (!doc || !doc.documentElement) return;

  if (ENABLE_PRELOAD &&
      typeof HTMLScriptElement.supports === 'function' &&
      HTMLScriptElement.supports('speculationrules') &&
      !doc.querySelector('script[data-umbra-preload]')) {
    const risky = ['logout', 'log-out', 'signout', 'sign-out', 'delete',
                   'remove', 'unsubscribe', 'cart', 'checkout', 'purchase'];
    const where = {
      and: [
        { href_matches: '/*' },
        { not: { selector_matches: '[rel~="nofollow"], [download], [target="_blank"]' } },
        ...risky.map(w => ({ not: { href_matches: '/*' + w + '*' } })),
        ...risky.map(w => ({ not: { href_matches: { pathname: '/*', search: '*' + w + '*' } } })),
      ],
    };
    const rules = {
      prerender: [{ source: 'document', where, eagerness: 'moderate' }],
      prefetch: [{ source: 'document', where, eagerness: 'moderate' }],
    };
    const s = doc.createElement('script');
    s.type = 'speculationrules';
    s.setAttribute('data-umbra-preload', '');
    s.textContent = JSON.stringify(rules);
    (doc.head || doc.documentElement).appendChild(s);
  }

  if (ENABLE_VIDEO) {
    const boost = (v) => {
      if (v.dataset.umbraBuffered) return;
      v.dataset.umbraBuffered = '1';
      const p = (v.getAttribute('preload') || '').toLowerCase();
      if (p !== 'none') v.preload = 'auto';
    };
    doc.querySelectorAll('video').forEach(boost);
    new MutationObserver((records) => {
      for (const r of records) {
        for (const n of r.addedNodes) {
          if (n.nodeType !== 1) continue;
          if (n.tagName === 'VIDEO') boost(n);
          else if (n.querySelectorAll) n.querySelectorAll('video').forEach(boost);
        }
      }
    }).observe(doc.documentElement, { childList: true, subtree: true });
  }
})();
)JS";

}  // namespace umbra

#endif  // UMBRA_RENDERER_PRELOAD_SCRIPT_H_
