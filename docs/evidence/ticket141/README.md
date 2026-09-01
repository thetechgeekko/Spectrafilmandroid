# Ticket #141 device-evidence contract

`tools/ticket141_mask_memory.ps1` writes the current run's durable **text-only** evidence to
`docs/evidence/ticket141/current/`. Large APKs and pulled installed copies remain under the selected
`build/evidence` directory and are intentionally not tracked here.

A run is authoritative only when all of these conditions hold:

- `qualification_status.txt` says `status=COMPLETE`;
- `provenance.txt` records the Git commit/tree, source-manifest digest, exact target/test APK hashes,
  device identity, and cell parameters;
- `source_manifest.sha256` hashes every ticket-owned source/test input and the relevant Android/Gradle
  build inputs;
- `artifact_manifest.sha256` matches the supplied APK pair, and the script has also pulled the two
  installed base APKs and compared their hashes;
- both memory-cell files contain a result, the forced-denial cell passes, and the source manifest plus
  repository `HEAD` remain unchanged from before installation through the end of sampling; and
- thermal and battery snapshots are present for review.

`status=INCOMPLETE`, a missing file, or a source/artifact digest mismatch makes the run
non-authoritative. A historical run cannot qualify a later source tree merely because its device and
dimensions are the same.

The 12.5 MP and 50 MP cells qualify bounded-memory behavior and deterministic repeat output only. They
do not establish the separate 1–2 second export-performance objective.
