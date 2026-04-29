# Snapshot Parent Resolution Fix Design

**Problem:** The snapshot tree can show a newly created snapshot under the wrong parent because backend tree assembly does not handle unresolved parent lookups explicitly.

**Design:** Add a regression test in `tests/test_snapshot_tree.c` that exercises the incorrect parent-linking scenario, then tighten parent resolution in `src/libvirt_backend.c` so each snapshot is attached only when its parent node is found. If the parent lookup returns a name that is absent from the current node set, handle that case deliberately instead of leaving the node orphaned and letting the tree drift.

**Trade-offs considered:**
- Fix the frontend graph rendering only: hides the symptom, leaves API output wrong.
- Re-sort children after JSON serialization: cosmetic and still relies on bad structure.
- Fix parent resolution in the backend: recommended because it corrects the source of truth for every consumer.

**Verification:** Use the existing `make test` snapshot-tree coverage to prove the regression fails first and stays green after the backend change.
