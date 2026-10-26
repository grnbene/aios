# Store API Example

A production-style REST API service demonstrating route handling, JSON requests/responses, exact decimal financial math, custom status codes, and pre-cutover testing.

Related guides:
- [Writing Grain applications](../../../docs/grain-app-writing.md)
- [Running and managing Grain applications](../../../docs/grain-apps.md)

---

## Directory structure

```text
examples/apps/api/
├── app.gr         # Service manifest (port 18602, health, settings, secrets)
├── main.gr        # Route definitions with serve_routes()
├── lib/
│   └── store.gr   # Financial calculations with exact decimals
├── check.gr       # Pre-cutover verification suite
└── README.md
```

## Endpoints

| Endpoint | Method | Description | Response Status |
|---|---|---|---|
| `/health` | `GET` | Health check probe (uptime and health state) | `200` |
| `/quote` | `POST` | Calculate invoice subtotal, tax, and total | `200` (success), `400` (bad payload), `405` (wrong method) |
| `/version` | `GET` | App metadata and header inspection | `200` |

### Quote request payload example

```json
{
  "items": [
    {"sku": "WIDGET-A", "price": "19.99", "qty": 2},
    {"sku": "WIDGET-B", "price": "5.50", "qty": 1}
  ]
}
```

Response:

```json
{
  "subtotal": "45.48",
  "tax": "3.64",
  "total": "49.12",
  "item_count": 2
}
```

Prices and taxes are calculated using Grain's exact 128-bit fixed-point `decimal()` type with half-up rounding, preventing floating-point inaccuracies.

---

## Running locally

To run and test the service in the foreground:

```bash
grain app run examples/apps/api
```

Test the endpoints from another shell:

```bash
# Health check
curl -i http://localhost:18602/health

# Price quote
curl -i -X POST http://localhost:18602/quote \
  -H "Content-Type: application/json" \
  -d '{"items": [{"sku": "A", "price": "19.99", "qty": 2}, {"sku": "B", "price": "5.50", "qty": 1}]}'

# Inspect headers & settings
curl -i http://localhost:18602/version -H "Authorization: Bearer secret-token"
```

---

## Validation & Deployment

Verify the manifest and ensure all scripts compile:

```bash
grain app check examples/apps/api
```

Deploy to `grain host`:

```bash
grain app deploy examples/apps/api
```

During deployment, the supervisor runs `check.gr` against a temporary staging port before swapping live sockets. If the health probe or quote verification fails, deployment aborts cleanly and leaves the previous release running.
