# Branch merge verification — 2026-10-04

All 15 feature branch names fetched from `origin` are ancestors of local `master`.
There are 14 distinct feature tips and 14 merge commits, each created with
`git merge --no-ff`. `upstream-sync` and
`visually-distinguish-selected-slides-in-text` both point to `39e90c5`; after
merging the latter, merging the former reports “Already up to date.”

The starting commit was `f9eefda` (Hype 0.4.3). The verified final application
code is at `a1291cc`. Changes are local; no push or branch deletion was performed.

## Checks after each merge

Each distinct merge was rebuilt and passed the full Qt suite before moving to
the next branch. The checks use actual QML windows, keyboard/mouse events,
rendered pixels, and application output files. CLI/export checks were rerun
for their changes, and the full Python suite ran for the later merges.
Conflicting test scenarios were retained and given compatible starting states.

| Branch | Functionality checked |
| --- | --- |
| `fix-building` | Build with bundled Qt 6.9.3; reject system Qt 6.4.2; test runner uses the same toolchain. |
| `slide-movement-shortcuts` | Move a selection five slides or to either boundary; keep ranges together; undo; sidebar and Overview. |
| `fix-home-end` | Native line Home/End in text editors; retain slide-focused navigation and movement shortcuts. |
| `nonactive-panel-tint` | Subdued selected thumbnails while editing; restore active styling; editor border pixels survive resize. |
| `theme-fixing-cinnamon` | Bundled palettes without Omarchy; installed overrides; editor palette follows presentation; themes survive undo/redo. |
| `fix-blurry-slides` | Rapid image navigation; a deliberately visible late thumbnail cannot cover the sharp preview, verified in captured pixels. |
| `presentation-start-hotkey` | Shift+F5 starts at slide one and returns there during an active presentation. |
| `mint-release` | Build/extract the actual 0.4.3 `.deb`; relocated launcher; eight fonts; 22 palettes; two-slide rendering; PDF/PPTX; GUI startup. |
| `slide-navigation-from-text` | Ctrl+Up/Down and Ctrl+PageUp/PageDown navigate without reordering; native word movement; keypad modifiers; visible slide boundaries. |
| `fix-slide-selection-from-frontmatter` | Entering metadata selects slide one and reveals it in the sidebar. |
| `fix-remember-cursor-position` | Tab preserves unchanged-selection caret; changing slides places it on slide text; metadata positions are retained. |
| `fix-remember-cursor-between-text-modes` | Exact caret mapping between Visual and Markdown, edited text, omitted padding, metadata positions, toolbar switches and focus. |
| `optimize-slide-changing-speed` | Selection-only notifications; stable font/source controls; editing, undo/redo and saving; measured keyboard/frame latency. |
| `visually-distinguish-selected-slides-in-text` | Unicode/CRLF source ranges; outline pixels; partial ranges; Start/End jumps preserve selection/caret; resize; native text selection stays independent. |

## Fixes made during integration

- `bin/test` now selects bundled Qt or `HYPE_QMAKE`, checks the minimum version,
  and cleans objects when the toolchain changes.
- PowerPoint checks validate JPEG signatures and frame dimensions instead of
  interpreting JPEG bytes as PNG headers. The PDF resolution fixture uses
  one-pixel luminance stripes so chroma subsampling does not obscure its 4K
  resolution check.
- The CLI theme listing check accepts bundled palettes alongside installed ones.
- Source scrolling combines whole-slide navigation, caret retention and mode
  mapping without confusing navigation deltas with cursor positions.
- The public QML `setMode` method keeps its one-argument Qt interface. A separate
  helper handles caret-preserving switches.
- Palette notifications, editor focus borders, bundled resource entries and
  selection-only notifications are preserved together. Source range properties
  notify on `selectionChanged`.
- The viewport aligns to the complete source boundary while the caret remains
  on the first text character. This fixes incorrect offscreen jump controls for
  an otherwise fully visible selected slide.
- Shortcut documentation retains both slide reordering and text navigation.

These fixes are described in the fix commits and merge-resolution commit bodies.

## Final results

Environment: Linux Mint 22.3, Qt 6.9.3, GNU source-highlight, FFmpeg.

- Normal QML runtime, full Qt suite: **68 passed, 0 failed, 5 optional checks skipped**.
- Full Python suite with LibreOffice compatibility enabled: **28 passed, 0 failed, 0 skipped**.
- Desktop portal test in an isolated D-Bus session: **passed**.
- Video last-frame pixels and replay with XCB/OpenGL on the real display: **passed**.
- Rebuilt final `.deb`, extracted and smoke-tested: **passed**.
- Branch ancestry and `git diff --check`: **passed**.

The default Qt skips are portal, video, the new navigation benchmark, and two
older optional performance checks. Portal, video and the new benchmark were
run separately. The older trial-based performance checks were not run.

One baseline run crashed in Qt's QML garbage collector; baseline and per-merge
GUI suites were subsequently run with `QV4_FORCE_INTERPRETER=1`. The final full
suite also passed with that variable unset. No runtime workaround was added
to the application.

### Navigation measurements

The same 80-slide fixture, offscreen software backend and interpreter were used
before and after, with 120 native Markdown cursor transitions per run.

| Metric (median) | Before optimization (`b4fabc8`) | Final integrated code (`a1291cc`) |
| --- | ---: | ---: |
| Key handling | 190.657 ms | 3.366 ms |
| Next frame | 197.477 ms | 10.392 ms |

Final key/frame samples all stayed below 16.67 ms in this run. These are local
measurements, not a guarantee for every desktop or presentation.

Successful logs and selection screenshots are saved in `build/verification/`.
The final package is `build/deb/hype_0.4.3-1_amd64.deb`.

Typical reproduction commands, with Qt 6.9+ and source-highlight available:

```sh
./bin/build
QT_QUICK_BACKEND=software ./bin/test
QT_QUICK_BACKEND=software HYPE_OFFICE_TESTS=1 python3 -m unittest discover -s tests -p 'test_*.py'
QT_QUICK_BACKEND=software QV4_FORCE_INTERPRETER=1 HYPE_NAV_BENCHMARK=1 ./bin/test benchmarkNavigation
HYPE_PORTAL_TESTS=1 dbus-run-session -- ./bin/test portalFileDialogs
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=rhi HYPE_GUI_TESTS=1 build/tests/hype-tests videoHoldsLastFrameAndReplays
./bin/build-deb
```
