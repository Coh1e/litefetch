# Qt-free task core

This directory is the UI/engine boundary used by the Win32 product. It does not
include or link any Qt header or library. The Qt implementation is no longer in
`src/`. Qt-era test sources remain in `tests/`; the current build does not
compile them.

## Boundary

- `task_identity.*` — stable lowercase-hex SHA-256 identity over the canonical
  source bytes. The same source keeps the same id across processes and
  database reopenings.
- `text_utf8.*` — strict UTF-8 ⇄ UTF-16 and UTF-8 ⇄ `std::filesystem::path`
  conversion for the Windows boundary. Invalid UTF-8 throws instead of letting
  `std::filesystem::path`'s ANSI narrow constructor retarget a Chinese file
  name; the HTTP engine, task service, SQLite open and CLI all use it.
- `task.*` — persisted states (`queued`, `running`, `completed`, `failed`,
  `cancelled`), task snapshots, and the `Add` / `Start` / `Cancel` commands.
- `task_store.*` — SQLite persistence boundary. Creation writes schema version
  `1` into `native_meta`; task rows live in `native_tasks`. The legacy Qt
  `tasks` table and `user_version` are untouched, so opening the same database
  file does not silently convert old data. Every `insert`/`update` either
  commits synchronously or throws `StorageError`.
- `http_engine.*` — blocking libcurl HTTP(S) transfer. The destination is only
  published after a successful transfer and publication is no-replace:
  `MoveFileExW(..., 0)` refuses an existing target, so two concurrent publishers
  cannot silently overwrite each other. Failure or cancellation removes the
  staging file. Progress runs on the calling thread.
- `task_service.*` — command orchestration. A command reports `ok=true` only
  after its required database commit. `Start` persists `running` before calling
  the engine and persists the engine's actual outcome afterwards. A storage
  failure is returned as `ok=false` and is never reported as a successful
  commit. `Start`/queued-`Cancel` transitions and active-slot registration share
  one lock, so a committed cancel cannot be overwritten by a concurrent start;
  a `cancelled` task requires an explicit future retry command. `Cancel` on a
  running task waits for the owning `Start` to persist the real outcome. An
  optional progress observer lets a UI cache live progress on the transferring
  thread without touching the database; observer exceptions are ignored.
- `cli/main.cpp` — `litefetch-core-cli add|start|cancel|list` for manual
  end-to-end checks and later Win32 integration. Arguments are read as UTF-16
  and converted to UTF-8, so Chinese paths work from a Chinese console; `list`
  opens the database read-only so it can poll while the Win32 task writer is
  active.

## Build and verify

Prerequisites (no global install; CMake fetches the dependencies from source):

```powershell
cmake --preset windows-core
cmake --build out/build/windows-core
ctest --test-dir out/build/windows-core --output-on-failure
```

`windows-core` builds its dependencies from source with CMake FetchContent into
`out/build/<preset>/_deps`, and keeps temporary files under `.deps/tmp`. CTest also sets
`LITEFETCH_TEST_TEMP`/`TEMP`/`TMP` for the core tests to `tmp/core-test-temp`
inside the repository.

Tests exercise the real boundary: a Winsock-controlled HTTP peer serves known
bytes; `Add` and `Start` go through SQLite; the downloaded file is compared
byte-for-byte and the snapshot is checked after reopening the database. Other
scenarios cover HTTP 404, cancellation during a slow transfer, duplicate-source
identity, and a read-only database where neither Start nor Add may report
success.

Manual check with an independent HTTP peer and SHA-256 comparison:

```powershell
node scripts/core-http-fixture.mjs 8080 262144
out/build/windows-core/litefetch-core-cli.exe add --db data/tasks.db --source http://127.0.0.1:8080/file.bin --destination D:\Downloads\file.bin
out/build/windows-core/litefetch-core-cli.exe start --db data/tasks.db --id <task-id>
out/build/windows-core/litefetch-core-cli.exe list --db data/tasks.db
```

## Deliberate limits of this ticket

Pause/resume and crash recovery (#5), HTTPS/auth policy (#6), destination
conflict policy (#18), and the complete Win32 UI (#3+) are not implemented in
this core. `Start` is blocking; the Win32 shell (`src/win32`) dispatches it off
the UI thread through its own worker. The Qt path is no longer part of the
build.
