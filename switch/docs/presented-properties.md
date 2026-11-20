# Presented-property table

Bionic's `__system_property_get` is how Android apps ask "what device am I
on?": build version, SDK level, product model, locale, debug flags. Roblox
will ask, and what we answer steers its code paths, so the answers are a
deliberate device profile - presented, not measured.

## Contract

- The table is `android_presented_properties` in
  `switch/source/android_property_compat.c`. It ships EMPTY until a console
  run's `system_property_read` log lines show which names the guest actually
  queries. Do not invent queries the guest has not made.
- Every row needs a provenance note naming the value's source: the console
  query log that motivated the row plus where the value itself came from
  (AOSP defaults, Roblox's documented requirements, an explicit choice with
  reasoning). INFERRED values are labeled INFERRED.
- Values are capped at 91 bytes plus NUL (bionic `PROP_VALUE_MAX`). A longer
  row is a table bug; the provider truncates defensively and that is a bug
  to fix in the table, not data to ship.
- Names not in the table get bionic's not-found answer (empty value, return
  0, from libc/bionic/system_properties.cpp) and are logged with
  `result=empty` so the next iteration knows what the guest wanted.
- Presented rows log `result=presented`, so a console report shows exactly
  which answers were ours.

## Post-console-run workflow

1. Pull the run's diagnostic log; collect the `system_property_read
   name=... result=empty` lines.
2. For each name, research what value keeps the guest on a supported code
   path, and record the source.
3. Add rows to the table with provenance comments. No provider code changes
   are needed - the lookup is data-driven.
4. Re-run host tests (the table test asserts the production table's shape)
   and rebuild the preview NRO.

## Why presented rather than real

Horizon has no Android property area; the honest choices are "not found" or
a documented presentation. A wrong presented value can silently move the
guest onto a code path we have not bridged, which is why rows arrive only
with evidence and provenance, and why the empty-table default is not-found.
