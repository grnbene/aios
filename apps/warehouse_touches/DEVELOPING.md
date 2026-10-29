# Developing Warehouse Touches

This guide covers setting up a local development environment for the Warehouse Touches app, importing test data into a local ASQL instance, running the app locally, and submitting changes safely.

Before starting, review [README.md](README.md) for an overview of application architecture, routes, and data sources.

---

## 1. Safety and Environment Isolation

**Always target a local ASQL database instance for development, never production (`atl-ga-1`).**

Deploying the application executes `deployed.gr`, which registers or replaces the `touch_hour` Thing in whichever ASQL database the app connects to. Targeting production directly would overwrite the production Thing definition. All development should take place against a local database instance (see Section 3) or by forwarding compute across the fleet (see Section 9), and changes should be deployed to production through standard branch reviews.

## 2. Build Prerequisites & Binaries

Building requires Linux, CMake 3.20+, GCC 10+ or Clang 13+, and OpenCL development headers (`CL/cl.h`, used during compilation).

```bash
git clone git@github.com:QBCSystems/aios.git ~/aios
cd ~/aios

# Grain language runtime, app host, and CLI
cmake -S grain -B build-grain -DCMAKE_BUILD_TYPE=Release
cmake --build build-grain -j

# AIOS core and ASQL database engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target aios-cli
```

This produces `build-grain/grain` and `build/aios`. Add `build-grain` and `build` to your `PATH` or reference them directly by path.

## 3. Local ASQL Setup with Snapshot Data

To develop and verify reports locally, import a snapshot of the warehouse database into a local ASQL instance.

### Obtaining a Snapshot

Snapshots can be exported from an existing ASQL environment using `packaging/asql-export.sh`:

```bash
# If generating an export from an existing server instance:
./packaging/asql-export.sh --conf /etc/aios/asql.conf -o ~/asql-v8.tar.zst
# Produces asql-v8.tar.zst and asql-v8.tar.zst.sha256
```

> [!NOTE]
> Database snapshots contain real business data. Store and handle them securely in accordance with team data handling policies.

### Configuring and Importing

Create a local working directory, configure your local ASQL instance, and set a development password:

```bash
mkdir -p ~/asql-dev/run ~/asql-dev/root && cd ~/asql-dev
umask 077

# Set a local development password
openssl rand -hex 12 > mysql.pass

# Create the ASQL configuration
cat > asql.conf <<EOF
AIOS_ROOT=$HOME/asql-dev/root
SQL_DB=sql_db
MYSQL_PORT=3307
MYSQL_BIND=127.0.0.1
MYSQL_USER=erp
MYSQL_PASS_FILE=$HOME/asql-dev/mysql.pass
EOF

# Import the snapshot (ensure the .sha256 file is in the same directory as the archive)
~/aios/packaging/asql-import.sh ~/Downloads/asql-v8.tar.zst --conf ~/asql-dev/asql.conf --no-service
```

### Starting Local ASQL

Run the engine in a dedicated terminal:

```bash
AIOS_NO_ACCEL=1 ~/aios/build/aios --root ~/asql-dev/root exec "compute serve port=17311 lanes=4 sql=1 \
  mysql_port=3307 mysql_bind=127.0.0.1 mysql_user=erp mysql_pass_file=$HOME/asql-dev/mysql.pass"
```

The database initializes within a few seconds and provides a MySQL/MariaDB-compatible interface on port `3307`. Verify the connection and imported data:

```bash
printf '[client]\nuser=erp\nhost=127.0.0.1\nport=3307\npassword=%s\n' "$(cat ~/asql-dev/mysql.pass)" > ~/asql-dev/client.cnf
mysql --defaults-extra-file=~/asql-dev/client.cnf -e "SHOW THINGS"
```

You should see `touch_hour` listed with existing rows. Note that local snapshot data remains static; re-import a newer snapshot when updated test data is needed.

## 4. Running the Application

1. Start the Grain app host in a separate terminal:

   ```bash
   grain host
   ```

2. Configure the database credentials and deploy the app locally:

   ```bash
   grain app set warehouse_touches DB_USER=erp DB_PASSWORD_FILE=$HOME/asql-dev/mysql.pass
   grain app deploy apps/warehouse_touches
   ```

   Successful deployment output looks like:

   ```text
   warehouse_touches: version <version_id> (first deploy)
     started; healthy
       touch_hour: replaced in place, 9704 hours
     deployed.gr: done
     running
   ```

3. Open the web interface at <http://127.0.0.1:8573/>. Export endpoints (JSON, PDF, Excel, CSV) are documented in [README.md](README.md), for example:
   <http://127.0.0.1:8573/quickreport?start=2026-10-20&stop=2026-10-26>.

## 5. Development Workflow

The typical development loop is: **edit → check → deploy → verify**.

```bash
# Check syntax and compilation across all files in the app. A Thing (things/*.gr) may name only the pure builtins,
# the engine's read/read_raw/write family and what it defines itself; the check refuses anything else with the
# line (e.g. "things/touch_hour.gr:40: 'now' is not available in a Thing"), before it reaches a deploy.
grain app check apps/warehouse_touches

# Deploy the updated version locally
grain app deploy apps/warehouse_touches

# View live application logs and output
grain app logs warehouse_touches -f

# Check status of the running app
grain app status
```

- **Safe reloads:** If a new version fails compilation or fails during `deployed.gr`, the previous version continues serving uninterrupted.
- **Rollback:** Use `grain app rollback warehouse_touches` to revert to the previous release if needed.
- **Immediate updates:** `grain app deploy` automatically recompiles and replaces `touch_hour` from `things/touch_hour.gr`, allowing logic changes to take effect immediately.

## 6. Project Layout

| Purpose | Location | Description |
|---|---|---|
| Touch calculation & hour bucketing | `things/touch_hour.gr` | The Thing definition. `enroll()` identifies relevant hours, `things()` aggregates touches per person per hour, and `touched()` invalidates hours when underlying rows change. |
| Report aggregation | `lib/queries.gr` | Functions (`fetch_warehouse_touches`, `merge_rows`) that query `touch_hour` over date ranges and compute sums and distinct operator sets. |
| Output formats & presentation | `lib/views.gr` | HTML dashboard, PDF, Excel, and CSV templates and renderers. |
| Routing & authentication | `service.gr` | HTTP endpoints, date/hour query parameters, and session/API authorization. |
| Post-deploy hooks | `deployed.gr` | Lifecycle script executed on deployment to register or update the Thing in ASQL. |

Each file includes introductory comments detailing its specific responsibilities and matching legacy PHP routines where applicable.

## 7. Grain Documentation & References

Grain syntax is modern and approachable (resembling TypeScript and Python). Refer to the documentation as needed:

- [Writing Grain Apps](../../docs/grain-app-writing.md) — Routing, request/response lifecycle, configuration, and hooks.
- [Grain Language Reference](../../docs/grain-reference.md) — Language specifications and built-in standard library functions.
- [Things in AIOS](../../docs/things.md) and [ASQL Grain Things](../../docs/asql.md#grain-things) — Architecture and sandbox rules for ASQL Things (table access constraints, pure execution rules).
- Quick experiments: Run standalone Grain scripts with `grain run <script.gr>`.

## 8. Verifying Logic & Calculations

Warehouse touches are critical operational metrics. When modifying touch aggregation logic:

1. Compare outputs across representative date ranges against legacy reports (`touches.php`) using equivalent data.
2. Confirm that per-operator counts match expected totals.
3. Clearly document any intentional differences in calculation methodology (for example, customer pickups bucketing by invoice entry timestamp).

## 9. Joining the Fleet & Deploying

In production, `warehouse_touches` runs on `atl-ga-1`, which hosts the primary ASQL database and maintains the authoritative `touch_hour` Thing. 

Joining the fleet allows a machine to either:
- **Forward requests from a local app host to `atl-ga-1`** (`WAREHOUSE_COMPUTE_ON`), letting you test the web interface and exports without needing a local ASQL database or modifying the production Thing.
- **Deploy and manage releases on remote nodes** across the cluster using AIOS fleet orchestration.

### 9.1 Connecting to the Fleet Hub

Machines in the fleet communicate over encrypted node links brokered by a fleet hub (typically listening on port `7420`):

1. **Obtain the cluster key**: Securely load the fleet cluster key file (for example, `/etc/aios/fleet.key` or `~/.aios/fleet.key`). Ensure permissions are restricted (`chmod 600`).
2. **Join the hub**:
   ```bash
   # Attach to the hub (retry keeps trying until the hub answers)
   aios --root ~/asql-dev/root exec "fleet join <hub-host> 7420 @/path/to/fleet.key retry"
   ```
   Or configure persistent connection via environment variables in systemd or your shell:
   ```bash
   export AIOS_FLEET_HUB="<hub-host>:7420"
   export AIOS_FLEET_KEY="<cluster-key>"
   ```
3. **Verify membership**:
   ```bash
   aios exec "fleet members"
   aios exec "fleet status"
   ```
   Note the node ID (32-hex ID or 12-char prefix) for the target machine (e.g., `atl-ga-1`).

### 9.2 Running with Forwarded Compute (`WAREHOUSE_COMPUTE_ON`)

To test the application locally against production data without copying snapshots or running local ASQL:

1. Set `WAREHOUSE_COMPUTE_ON` to `atl-ga-1`'s member ID (or prefix):
   ```bash
   grain app set warehouse_touches WAREHOUSE_COMPUTE_ON=<atl-member-id>
   ```
2. Deploy the app locally:
   ```bash
   grain app deploy apps/warehouse_touches
   ```
3. When `WAREHOUSE_COMPUTE_ON` is set:
   - All requests (dashboard, quickreport, PDF/Excel exports, and live event streams) are forwarded across the fleet mesh to `atl-ga-1` and computed there.
   - Nothing is computed locally, and local deployment does not alter the ASQL Thing.

### 9.3 Deploying to Remote Fleet Nodes

Authorized releases can be deployed to remote fleet members directly from your workspace using `app deploy ... on <node>`:

1. **Check application compilation before deploying**:
   ```bash
   grain app check apps/warehouse_touches
   ```
2. **Deploy over the fleet mesh**:
   ```bash
   # Deploy to a specific fleet member (e.g., atl-ga-1)
   aios exec "app deploy apps/warehouse_touches on <member-id>"

   # Or deploy across all fleet members running the app
   aios exec "app deploy apps/warehouse_touches on ALL"
   ```
   The host packages the app, streams it across the encrypted fleet fabric, spins up the new release on a staging port, validates health, swaps traffic with zero dropped requests, and executes `deployed.gr`.

3. **Manage configuration and secrets on remote nodes**:
   ```bash
   # Configure environment settings on the target node
   aios exec "app set warehouse_touches DB_USER=erp DB_PASSWORD_FILE=/opt/aios/mysql.pass on <member-id>"

   # Set sealed secrets securely via stdin
   cat /opt/aios/mysql.pass | aios exec "app secret warehouse_touches DB_PASSWORD on <member-id>"
   ```

4. **Monitor remote services**:
   ```bash
   # Check service status across the fleet
   aios exec "app status on ALL"

   # Follow live service logs on a remote member
   aios exec "app logs warehouse_touches -f on <member-id>"
   ```

5. **Rollback if necessary**:
   If an issue occurs after deployment, revert instantly to the previous release:
   ```bash
   aios exec "app rollback warehouse_touches on <member-id>"
   ```

---

## 10. Submitting Changes

1. Create a dedicated topic branch for your changes.
2. Verify code quality and compilation:
   ```bash
   grain app check apps/warehouse_touches
   ```
3. Ensure adherence to repository standards outlined in `AGENTS.md`.
4. Open a pull request with details on what was changed, testing steps performed, and any metric discrepancies observed. Once reviewed and approved, deploy to production fleet nodes following Section 9.

To stop local services when finished, stop `grain host` and the ASQL process with `Ctrl+C`. State is preserved across restarts.
