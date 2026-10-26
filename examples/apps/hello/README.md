# Hello world example app

A minimal Grain web app with routes, settings, encrypted secrets, helper imports, and a pre-deploy check.

Related guides:
- [Running and managing Grain applications](../../../docs/grain-apps.md)
- [Writing Grain applications](../../../docs/grain-app-writing.md)
- Other examples: [REST API service](../api/README.md) and [Background worker](../worker/README.md)

---

## Files

| File | What it does |
|---|---|
| `app.gr` | Manifest: sets app name, port 18601, default greeting, and secret names |
| `main.gr` | Service entrypoint: registers `/` and `/signed` |
| `lib/words.gr` | Helper module imported by `main.gr` |
| `check.gr` | Pre-cutover check: verifies responses before swapping ports |

---

## Quickstart

Start the host daemon if it isn't already running:

```bash
grain host &
```

Check that the files compile, then deploy:

```bash
grain app check examples/apps/hello
grain app deploy examples/apps/hello
```

Hit the endpoint:

```bash
curl http://localhost:18601/
```

```json
{"greeting":"hello from hello 784e4bbb55fc517c","up_s":1}
```

Change a setting and add a secret:

```bash
# Update an environment variable
grain app set hello GREETING=howdy

# Inject a secret via stdin
echo -n 's3cret' | grain app secret hello SIGNATURE

# Restart to load the new config
grain app restart hello

# Test the signed route
curl http://localhost:18601/signed
```

---

## Running directly without the host

If you're just tweaking code locally and don't want the supervisor:

```bash
grain app run examples/apps/hello
```

It keeps its working files in a `.grain/` folder inside the app folder; delete that folder when you are done.

---

## Cleaning up

```bash
grain app remove hello
```
