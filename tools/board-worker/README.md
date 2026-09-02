# board-worker — the hands elsewhere

The device never does the work. A spoken job becomes one work item on the
JarvisMCP coordination board (`delegate_task`); this loop, on any machine
with a coding agent installed, claims the item, runs it, and writes the
result back; the device announces it on its next poll (every 90 s while
awake) and `delegated_tasks` lists it by voice.

```
device ──delegate_task──▶ board ◀──claim / complete / block── worker.py
   ▲                        │
   └──── board_poll (90 s) ─┘        "The datasheet hunt is done: …"
```

## Run it

```bash
export JARVISMCP_URL="https://<gateway>/execute"   # the execute route
export JARVISMCP_TOKEN="<bearer for that gateway>"  # never in the repo
export WORKER_RUNTIME=claude                        # or codex
python3 tools/board-worker/worker.py
```

Optional: `BOARD_PROJECT` (default `jarvisnano-desk`, must match the device's
`project_id` in `POST /api/tools/config`), `WORKER_HOST`, `WORKER_MAX_SECONDS`
(900), `WORKER_IDLE_SECONDS` (20).

The agent runs as `claude -p "<task>" --output-format json` or
`codex exec --sandbox workspace-write "<task>"` in the worker's working
directory, so start the loop inside the checkout the jobs should touch, and
give that agent the JarvisMCP MCP server if the jobs need it.

## Keep it alive

macOS (launchd), `~/Library/LaunchAgents/com.jarvisnano.board-worker.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>Label</key><string>com.jarvisnano.board-worker</string>
  <key>ProgramArguments</key>
  <array><string>/usr/bin/python3</string><string>/path/to/tools/board-worker/worker.py</string></array>
  <key>WorkingDirectory</key><string>/path/to/checkout</string>
  <key>EnvironmentVariables</key><dict>
    <key>JARVISMCP_URL</key><string>https://gateway.example/execute</string>
    <key>JARVISMCP_TOKEN</key><string>see-keychain</string>
    <key>WORKER_RUNTIME</key><string>claude</string>
  </dict>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/tmp/board-worker.log</string>
</dict></plist>
```

Linux (systemd): a `Type=simple` unit with the same three variables in an
`EnvironmentFile=` outside the repo and `Restart=always`.

## What it will not do

- Run two jobs at once. One machine, one lease.
- Retry a blocked item. The device says it is blocked and why; a person
  decides.
- Keep state. Kill it any time between jobs; a lease it held expires on the
  board and `recoverWorkItem` is the documented way back.
- Hide credentials in the repo. If a token appears in a commit,
  `scripts/check-secrets.sh` fails the build.
