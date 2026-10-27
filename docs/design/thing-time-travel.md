# Time travel for Things: `SHOW THING <name> AS OF '<time>'`

## The question it answers

"Why did order 123 show Picked at 2:15 yesterday?" Today a row Thing keeps the last 32 changes per key in memory,
as display text (`line_count=1 | qty=5 | picked=0`). That is enough to see what happened to a key recently, but
not to ask what the whole Thing looked like at a moment, and a busy key loses yesterday within the hour.

## What is kept

Each row Thing (the SQL kinds: one row per key, or partitioned sums) appends every change to a log file beside its
state file, `<things dir>/<name>.asof`:

    c  <wall_ns>  <key>  <cause>  <n>  <value>...     the key's values after a change, and what caused it
    g  <wall_ns>  <key>  <cause>                      the key stopped being a Thing (its rows went away)
    b  <wall_ns>  <key>  <n>  <value>...              the state when the log began, or at its cut-off (see below)

Values are stored in the order `value_labels` gives (the key first). The log is on disk only: nothing is added to
the engine's memory per change, which matters on a mirror already short of it.

A Thing created after this change starts its log with its creation. A Thing that already existed starts it when the
engine first loads it, with a `b` line per key: history before that moment is not known, and is not invented.

## How much is kept

Two bounds, whichever is reached first:

- **Time:** 30 days by default (`AIOS_THING_KEEP_DAYS`), or per Thing with `SET THING <name> KEEP <n> DAYS`.
  `KEEP 0 DAYS` turns the log off and deletes it, for a Thing too busy to be worth it.
- **Size:** 256 MB per Thing by default (`AIOS_THING_KEEP_MB`). A busy Thing reaches this before its days.

When a log passes either bound it is rewritten: the state at the new cut-off as `b` lines, then every change after
it. For the size bound the cut-off moves forward until what is left is half the cap, so a rewrite happens once per
half-cap of new changes, not on every append. The cut-off is the earliest time `AS OF` can answer; a time before it
is refused with that time named.

The cost per change is one appended line, under the engine's Thing lock (as the state file's own writes are). The
rewrite reads the whole log once; at 256 MB that is a second or two, once per 128 MB of changes.

## The statement

    SHOW THING <name> AS OF '<YYYY-MM-DD HH:MM:SS>' [KEYS ('k', ...)]

The Thing as it stood at the end of that second (local time, as every Thing time is shown): one row per key that
existed then, its values, `changed_at` (when that value was set) and `cause` (the change that set it, the same text
as `SHOW THING HISTORY`). `KEYS` narrows it to some keys. A time after now answers the current state.

## Not in this step

- Grain Things, claims and resources: their state is not one row of values per key. They keep their own history.
- A per-row explanation of a value (which source rows make it up). That is a separate design.
- Time travel over plain tables: ASQL's store has its own `AS OF`; this is the Thing's view only.
