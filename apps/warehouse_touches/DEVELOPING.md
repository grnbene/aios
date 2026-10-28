# Working on Warehouse Touches

How to get the app running on your own machine, with real data, and change it safely. Every step here was run
end to end before it was written down.

Read [README.md](README.md) first: it says what the app does and where it runs. You know the PHP report
(`touches.php`) already; this app gives the same numbers, so the logic will look familiar.

---

## 1. The one rule

**Never point your copy at atl-ga-1's ASQL.** Every deploy runs `deployed.gr`, which replaces the `touch_hour`
Thing in whatever ASQL the app is set to. Pointed at atl, your work-in-progress would replace the one production
uses. Work against your own ASQL (step 3). Changes reach atl by a branch that Sean reviews and deploys.

## 2. Build

You need Linux, CMake 3.20+, GCC 10+ or Clang 13+, and the OpenCL headers (`CL/cl.h`; only needed to compile).

```bash
git clone git@github.com:QBCSystems/aios.git ~/aios
cd ~/aios

# grain: the language, its app host and the `grain app` command
cmake -S grain -B build-grain -DCMAKE_BUILD_TYPE=Release
cmake --build build-grain -j

# aios: needed for ASQL, the database the app reads
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target aios-cli
```

You get `build-grain/grain` and `build/aios`. Put `build-grain` on your `PATH`, or type the full path.

## 3. Your own ASQL, with a copy of the data

Sean makes the copy on atl and gives you the file. It is a snapshot of the whole `versioneight` mirror (about
160 MB), so treat it like the production database it is.

**On atl (Sean, as root):** copy `packaging/asql-export.sh` from the repo to atl, then:

```bash
./asql-export.sh --conf /etc/aios/asql.conf -o /root/asql-v8.tar.zst
# gives /root/asql-v8.tar.zst and /root/asql-v8.tar.zst.sha256 - hand over both, then delete them
```

**On your machine:** make a folder for it, a password for your local engine, and a small config:

```bash
mkdir -p ~/asql-dev/run && cd ~/asql-dev
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > mysql.pass     # your local login password
cat > asql.conf <<EOF
AIOS_ROOT=$HOME/asql-dev/root
SQL_DB=sql_db
MYSQL_PORT=3307
MYSQL_BIND=127.0.0.1
MYSQL_USER=erp
MYSQL_PASS_FILE=$HOME/asql-dev/mysql.pass
EOF

# put the snapshot in place (the .sha256 must sit next to the archive)
~/aios/packaging/asql-import.sh ~/Downloads/asql-v8.tar.zst --conf ~/asql-dev/asql.conf --no-service
```

Start the engine (leave it running in its own terminal):

```bash
AIOS_NO_ACCEL=1 ~/aios/build/aios --root ~/asql-dev/root exec "compute serve port=17311 lanes=4 sql=1 \
  mysql_port=3307 mysql_bind=127.0.0.1 mysql_user=erp mysql_pass_file=$HOME/asql-dev/mysql.pass"
```

It takes 10 to 30 seconds to load. Then it answers like MariaDB on port 3307:

```bash
printf '[client]\nuser=erp\nhost=127.0.0.1\nport=3307\npassword=%s\n' "$(cat ~/asql-dev/mysql.pass)" > ~/asql-dev/client.cnf
mysql --defaults-extra-file=~/asql-dev/client.cnf -e "SHOW THINGS"
```

You should see `touch_hour` with about 9,700 rows. The copy does not follow production: its data stays as it was on
the day of the snapshot. Ask for a new one when you need newer data.

## 4. Run the app

Start an app host (its own terminal; it keeps everything in `~/.grain`):

```bash
grain host
```

Tell it where your ASQL is, then deploy the app from your checkout:

```bash
grain app set warehouse_touches DB_USER=erp DB_PASSWORD_FILE=$HOME/asql-dev/mysql.pass
grain app deploy apps/warehouse_touches
```

You'll see:

```text
warehouse_touches: version 7775878438ae9d4d (first deploy)
  started; healthy
    touch_hour: replaced in place, 9704 hours
  deployed.gr: done
  running
```

Open <http://127.0.0.1:8573/>. The JSON, PDF, Excel and CSV addresses are in the README; for example
<http://127.0.0.1:8573/quickreport?start=2026-10-20&stop=2026-10-26>.

## 5. Making a change

The loop is: edit, check, deploy, reload the page.

```bash
grain app check apps/warehouse_touches      # does every file compile? names the file and line if not
grain app deploy apps/warehouse_touches     # the new version takes over; the old one serves until it does
grain app logs warehouse_touches -f         # print() output, errors, starts and stops
grain app status
```

A version that fails to start, or whose `deployed.gr` fails, never takes over: the old one keeps serving, so a
mistake costs you nothing but the deploy. `grain app rollback warehouse_touches` goes back one version.

`grain app deploy` rebuilds `touch_hour` from your `things/touch_hour.gr` every time, so a change to how touches are
counted shows up straight away.

## 6. Where things are

| You want to change | Look in |
|---|---|
| What is counted, and which hour it falls in | `things/touch_hour.gr` - the Thing. `enroll()` lists the hours, `things()` counts an hour's touches per person, `touched()` says which hours a changed row affects |
| How hours add up into a report (totals, per-day, the distinct sets) | `lib/queries.gr` - `fetch_warehouse_touches`, `merge_rows` |
| What the page, PDF, Excel and CSV look like | `lib/views.gr` |
| Addresses, the date and hour options, the login check | `service.gr` |
| What happens after a deploy | `deployed.gr` |

The comments at the top of each file say what it does. `touch_hour.gr` names the PHP function it matches
(`quickReportData`).

## 7. Learning grain

Grain reads like JavaScript or Python. Start with the code above, and look things up as you go:

- [docs/grain-app-writing.md](../../docs/grain-app-writing.md) - routes, requests and responses, settings, imports,
  the deploy hooks.
- [docs/grain-reference.md](../../docs/grain-reference.md) - the language and every builtin.
- [docs/things.md](../../docs/things.md) - what a Thing is. `touch_hour` is a grain Thing; see
  [asql.md - Grain Things](../../docs/asql.md#grain-things) for the rules its program follows (it may only read
  the tables it lists, and only call a fixed set of builtins).
- `grain run file.gr` runs a single script, handy for trying a function on its own.

## 8. Checking your numbers

The app must agree with the PHP report. After a change to what is counted, compare a few date ranges against
`touches.php` on the same data: the per-person totals should match. If they differ, say why in your change (the
pickups' hour is a known, deliberate difference: the app uses the invoice's input time).

## 9. Sending a change

Work on a branch, keep each change to one thing, and push it for Sean to review. He deploys to atl. Before you
push, run `grain app check apps/warehouse_touches` and read `AGENTS.md` at the top of the repo: it lists the rules
the project works by.

When you are done for the day, stop `grain host` and the engine with Ctrl+C. Both keep their data for next time.
