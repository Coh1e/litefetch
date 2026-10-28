# Win32 product window

The native application shell for LiteFetch: OS-drawn caption, native menu,
compact toolbar, Details ListView and the four-section status bar. It uses
Win32/Comctl32 only and the Qt-free `src/core` boundary; the legacy Qt code
elsewhere in `src/` stays available for comparison and is not part of this
target.

## What this ticket implements (#3)

- System title bar, six native menus (文件/编辑/查看/任务/工具/帮助), seven
  toolbar commands with the Figma command icons, six default columns
  (名称/大小/进度/下载速度/剩余时间/状态) and a four-part status bar.
- List geometry follows the Figma main board: the five fixed column pitches
  are 342/104/82/126/108 px (the design's 331/93/71/115/97 text slots plus the
  11 px gap between slots). At or above the design ListView client width
  (998 px at 96 DPI, i.e. the 1000 px window client minus the native 2 px
  ListView border) those pitches stay fixed and 状态 takes the remaining
  pixels, like `flex-[1_0_0]`. Below that width all six columns shrink
  proportionally, so 状态 never collapses to 0/1 px and there is no cliff at
  the threshold. Widths scale with DPI and are recomputed on resize;
  `ui-window` asserts that the live ListView shows exactly the widths
  `columnWidths()` computes for its own client width and DPI.
- "新建下载" native dialog: one HTTP address (the engine accepts HTTPS, but
  this ticket only exercises HTTP; HTTPS is #6), explicit save folder,
  `浏览...`, "添加后立即开始". The file name is derived from the URL and the
  destination path is passed to the core as UTF-8.
- A real new task goes through `TaskService` on a worker thread: `Add` (and
  optional `Start`) in the new dialog, `Start` for queued/retried tasks and
  `Cancel` for queued/running tasks. The list shows the engine's real bytes,
  measured speed, remaining time and persisted state; nothing is simulated.
- 打开文件夹 opens Explorer at the real destination (or selects the finished
  file), 复制下载地址 writes the task source to the clipboard, F5/查看→刷新
  reloads from SQLite.
- Closing the window cancels queued/running tasks, waits for the blocking
  `Start` calls to persist their outcome and joins every worker thread before
  the process exits. A cancelled transfer never publishes a partial file.
- Pause, delete, options, batch add, resume and task properties are **not**
  implemented in this ticket; their menu/toolbar entries stay disabled and the
  toolbar tooltip plus 帮助→本版本能力说明 state why.

## Build and run

Prerequisites are the project-local core dependencies (see
`src/core/README.md`; no global install):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap-core-deps.ps1
cmake --preset windows-core
cmake --build out/build/windows-core
out/build/windows-core/litefetch-win32.exe
```

Run with the vcpkg runtime DLLs on `PATH` (or copy them next to the executable):

```powershell
$env:PATH = "$PWD/.deps/installed-core/x64-windows/debug/bin;$env:PATH"
out/build/windows-core/litefetch-win32.exe --db "$PWD/tmp/tasks.db" --download-dir "$PWD/tmp/downloads"
```

Without switches the product uses `%LOCALAPPDATA%\LiteFetch\tasks.db` and the
user's Downloads known folder. `--db` and `--download-dir` exist for development
and acceptance runs and write only to the given project-local paths.

## Tests

Acceptance for a ticket is one command. It never launches a second process:

```powershell
test-local.cmd
```

- `ui-window` — creates the real main window in-process and drives it with the
  same commands a user would: search overlay, filters, status parts, toolbar
  toggle, the live column geometry against `columnWidths()`, toolbar enablement
  for a queued and a completed task, copy link into the clipboard, a real HTTP
  download started from the list, delete-record-keeps-file, the open-folder
  decision, column drag and persistence across a reopen, the UI Automation
  surface, and a clean close (exit code 0). Dialogs are dismissed with a
  callback timer, so nothing races a second process. With
  `LITEFETCH_EVIDENCE_DIR` set (ctest sets it to `.pi/evidence/ui-window`) it
  also writes PNG screenshots of the window through `PrintWindow`.
- `ui-model` — column formatting, measured speed/remaining time, status
  parts, action enablement, URL/file-name parsing, the startup command line and
  the Explorer target of `打开文件夹` (no window needed).
- `ui-worker` — the real worker threads against a controlled HTTP peer:
  add+start completion, cancel, close-during-download shutdown and the
  no-publish/no-staging guarantees.
- `core-unicode-path` — a Chinese destination directory/file and a Chinese
  database path survive the UTF-8 command boundary.
- `scripts/win32-smoke.ps1` — not part of ticket acceptance; the release check
  (#28) launches the built exe once and closes it, failing with evidence instead
  of hanging. Why it avoids the shell, and why a double-click still shows
  "unknown publisher" until #27 signs the binary, is in the script header. See
  [ADR-0001](../../docs/adr/0001-acceptance-without-a-second-process.md).

## Icons

`src/win32/assets/figma/*.svg` are the Figma command glyphs; Windows needs
`.ico` resources, so `scripts/build-win32-icons.mjs` rasterizes that small SVG
subset (fill/stroke paths, masks) into
`src/win32/assets/icons/*.ico` at 16/20/24/32/48 px. The generated icons are
committed; rerun the script only when the design assets change (Node is already
used by the test fixtures; no new dependency).

## Known limits of this ticket

- Pause/resume and crash recovery are #5. A persisted `running` row without a
  worker thread is displayed as `已中断`; it is not silently resumed.
- Delete and batch/list operations are #4; the commands stay disabled.
- The list is single-selection here. Multi-select and the remaining keyboard
  surface belong to #4/#26.
- HTTPS, authentication and proxy policy are #6/#17; this ticket exercises the
  HTTP transfer path that already exists in the core. HTTPS is not accepted as
  verified by this ticket.
- The new-download dialog keeps the design's field order and 72 px URL field
  but follows native control metrics: the window is 727×324 px (visible about
  711×308) versus the Figma board's 570×301 px, i.e. +24.7% width and +2.3%
  height. This comes from a 406-DLU resource with Segoe UI 9 pt; it is not a
  DPI error or custom drawing, and no field is clipped. The URL edit no longer
  shows the vertical scrollbar the design does not draw. Whether the Figma
  pixel size must be matched exactly is a human decision; this ticket keeps
  native controls (see #29).
- DPI changes are handled through `WM_DPICHANGED` with system metrics/font and
  column geometry has unit cases for 96/120/144/168 DPI, but this ticket was
  only exercised in a real window at the machine's current scaling (100%).
