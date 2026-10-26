# Background worker example

A minimal background worker service demonstrating non-HTTP application loops in Grain.

Related guides:
- [Running and managing Grain applications](../../../docs/grain-apps.md)
- [Writing Grain applications](../../../docs/grain-app-writing.md)

---

## Application structure

```text
examples/apps/worker/
├── app.gr             # Manifest with kind: "worker"
├── main.gr            # Long-running loop with sleep()
├── lib/
│   └── queue.gr       # Modular helper code
└── README.md
```

## Manifest (`app.gr`)

A worker omits `port` and specifies `kind: "worker"`:

```grain
let app = {
  "name": "worker",
  "description": "background worker loop processing jobs and writing heartbeats",
  "main": "main.gr",
  "kind": "worker",
  "settings": {
    "INTERVAL_MS": "1000",
    "BATCH_SIZE": "5"
  },
  "secrets": [
    "WORKER_TOKEN"
  ]
}
```

## Running locally

Test the worker directly in your terminal:

```bash
grain app run examples/apps/worker
```

The worker prints its log to stdout. Press `Ctrl+C` to stop it cleanly: `sleep()` unblocks immediately upon host shutdown.

## Deploying to a host

Check syntax and compile all files:

```bash
grain app check examples/apps/worker
```

Deploy to the local host daemon:

```bash
grain app deploy examples/apps/worker
```

Inspect running status and stream live logs:

```bash
grain app status worker
grain app logs worker -f
```

Set or update configuration variables without modifying code:

```bash
grain app set worker INTERVAL_MS=2000 BATCH_SIZE=10
grain app restart worker
```
