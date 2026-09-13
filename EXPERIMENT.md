# Cache hot-path experiment (not accepted)

Branch experiment/cache-hotpath-20260913 contains the cumulative TLS candidate as
an uncommitted worktree over the original repository HEAD. The pre-existing
layout18 development changes are included; HEAD alone does not identify this code.
Source fingerprints, stage snapshots, binaries, and all benchmark results are in
../datarouter-benchmark/results/hotpath-20260913/.

The remote repeated routed measurements do not establish a stable improvement.
No runtime changes from this experiment are retained in the main working tree.
Do not publish, merge, or update assembly pointers from this worktree.

2026-09-13 rollback: candidate source is now preserved as a signed experimental commit. Not merged. Results archived outside the main worktree at `/home/klp/CGEOS/rollback-archive-20260913/benchmark-results/hotpath-20260913/`.
