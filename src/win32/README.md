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
- "新建下载" native dialog: one HTTP(S) address, explicit save folder,
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

```powershell
ctest --test-dir out/build/windows-core --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/win32-e2e.ps1
```

- `ui-model` — column formatting, measured speed/remaining time, status
  parts, action enablement and URL/file-name parsing (no window needed).
- `ui-worker` — the real worker threads against a controlled HTTP peer:
  add+start completion, cancel, close-during-download shutdown and the
  no-publish/no-staging guarantees.
- `core-unicode-path` — a Chinese destination directory/file and a Chinese
  database path survive the UTF-8 command boundary.
- `scripts/win32-e2e.ps1` — drives the real window through real messages: menu
  command, native dialog controls, toolbar start, clipboard, Explorer, file
  hash, SQLite row and window close during a throttled download. It captures
  screenshots under `.pi/evidence/issue-03/`.

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
  HTTP transfer path that already exists in the core.
- DPI changes are handled through `WM_DPICHANGED` with system metrics/font, but
  this ticket was only exercised at the machine's current scaling (100%).
