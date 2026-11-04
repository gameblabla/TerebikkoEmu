# Web player sources

The browser build is plain JavaScript assembled into two standalone HTML files. Edit the
sources here, not the generated files in `web/`.

| Path | Purpose |
| --- | --- |
| `js/decoder-worker.js` | Off-thread 8 kHz / pulse decoder. Inlined as a `text/plain` script so the page stays standalone. |
| `js/engine.js` | Shared audio analysis, event timeline and decoder-worker plumbing. |
| `js/controller.js` | DOM controller, game state, input, save states and minigame. |
| `styles/app.css` | Page styles. |
| `templates/page.html` | Outer HTML document with `{{MARKERS}}`. |
| `templates/body.html` | Shared page layout. |
| `templates/screen.html`, `aside.html`, `settings-modal.html`, `state-modal.html` | Shared UI partials. |
| `templates/debug-config.html` | Debug-only event/channel panel. |
| `build.js` | Node assembler (no shell scripts or external dependencies). |

Build from the repository root:

```
make -f Makefile.web            # release + debug
make -f Makefile.web release    # web/index.html
make -f Makefile.web debug      # web/debug.html
make -f Makefile.web check      # syntax-check the JavaScript sources
```

If `pickupCoin.json` exists at the repository root, its contents are inlined as
`window.TEREBIKKO_PICKUP_COIN_PRESET`; otherwise the controller falls back to `null`.
