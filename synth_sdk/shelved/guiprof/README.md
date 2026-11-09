# dirtyprof (docs/74)
The measurement prototype for GUI dirty regions (per-tile shape hashes). Results in docs/74. Its hook stubs were
replaced by the guiskip hooks (screens/guiskip.c); to use it again, swap the C calls in the hook.S stubs back.
