# VF2 project instructions

- Read `tools/acm/HANDOFF.md` when resuming. Within 10 minutes of starting performance work, run the pending experiment if it still applies. If the Mini is busy, run an offline check. If neither is possible, report the blocker and the action needed to clear it.
- State the hypothesis, measurement, and decision criterion in one sentence. Run the smallest useful check with the existing harness. Reviewing logs and preparing commands do not count as running an experiment.
- Verify the recorded device command, binary, and flags before interpreting timings. Record the command, result, and decision on disk.
- Run hardware jobs one at a time. Restore and verify the interactive release afterward. Screen changes with the short Dural replay and qualify repeatable gains with the full replay.
- Re-evaluate priorities with an independent agent every 4 hours, not every hour (user update, 2026-11-20).
- Keep ROM ZIPs, checkpoints, and benchmark artifacts on the PC. On the RAM-booted Mini, stage only the files needed for the current run and remove them afterward. Use `--release-rom` only with a disposable ZIP at its real path.
