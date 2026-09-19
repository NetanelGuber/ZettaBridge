# Contributing to ZettaBridge

Issues naming a 32-bit app you want to run are the most useful thing you can send: the app name
and version, where the APK comes from, and what happens when it starts. A runtime report from the
launcher (long-press the game, "Last run report") turns a vague failure into a fixable one.

## Licence and contributions

ZettaBridge is **source-available, not open source**: see [LICENSE](LICENSE). Two licences apply
cumulatively, PolyForm Noncommercial 1.0.0 and PolyForm Perimeter 1.0.1. You may read, modify and
share it for noncommercial purposes; you may not make money from it, and you may not provide
others a competing product, free or paid. Commercial use needs a separate licence from the
author. Ask; that is what the licence is there for.

**Before a pull request can be merged, its author assigns copyright in the contribution to the
project owner (ZailoxTT, https://github.com/ZailoxTT).** By opening a pull request you state
that:

1. the contribution is your own work, and you have the right to give it;
2. you assign copyright in it to the project owner, who may license it under any terms, including
   commercial ones;
3. you keep the right to use your own contribution elsewhere.

This keeps the project relicensable as a whole. Without it, a single merged patch would freeze
the licence of that file forever, because its author would have to be found and asked every time.

If you would rather not assign copyright, open an issue describing the change instead: a clear
description of the problem and the approach is welcome and will be implemented separately.

## What makes a change easy to accept

- One change per pull request, with the reason in the message: what breaks without it.
- A test that fails before the change and passes after it. `ctest --test-dir build/host` for host
  code, `tools/run_guest_tests.sh` for anything the guest executes.
- No new dependency without saying why the existing ones do not do the job.
- Device-facing changes: say which phone and which app you ran, and what the runtime report said.
