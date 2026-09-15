---
name: patch
description: Diagnose a bug and implement the fix without verifying it, then push the branch for the user to test on the Windows dev machine. Use when the user invokes /patch, or asks for a fix they have said they will test themselves. Not for milestones, features, or anything needing docs/MILESTONES.md.
---

# patch

Fast path for a bug the user will test. They have the dev machine; this
container does not. Diagnose, fix, push, stop.

## What this mode changes

CLAUDE.md's `## Before you finish a milestone` checklist does not apply.
Do not build, do not run the headless tests, do not run `tools/linux-deps.sh`,
do not `apt-get` a toolchain. An unverified patch is the deliverable, not a
degraded version of one.

The usual rules still hold: the conventions in CLAUDE.md, the dependency
allowlist, and `docs/ARCHITECTURE.md` gets updated when the fix changes
something architectural.

## The work

1. **Find the cause.** Read until the mechanism is understood — not until a
   plausible suspect appears. If the theory does not explain every part of
   the reported symptom, including the parts that seem incidental, it is
   probably the wrong theory. Asymmetries are evidence: one side broken and
   the other fine usually means two code paths that were meant to agree.

2. **Prefer removing the divergence over patching its effect.** Two
   definitions of one thing that drifted apart is a recurring shape in this
   codebase. Collapsing them to one is a better fix than compensating for
   the gap, and usually a smaller diff.

3. **Check what is free.** Static reasoning costs nothing and catches typos:
   geometry invariants, table arithmetic, whether a refactor preserves the
   values it claims to preserve. A twenty-line script proving the new data
   matches the old is worth writing. A toolchain bootstrap is not. Use the
   scratchpad directory, and do not commit the scratch work.

4. **Commit and push** to the session's designated branch. Commit message
   says what was wrong and why the fix addresses it. Do not open a PR unless
   asked.

## The final message

Three things, tersely:

- **What changed**, by file, one line each.
- **What was and was not checked.** Name the thing that would be caught by
  a build and was therefore not caught. "Not compiled, not run" is fine and
  complete; do not apologise for it or pad it into a disclaimer.
- **What it should look like if the diagnosis was right**, and what a
  persisting symptom would mean instead. The person testing this was not in
  the diagnosis and needs to know what they are looking at.

No summary of the investigation. If the reasoning is worth keeping, it
belongs in a comment next to the code or in `docs/ARCHITECTURE.md`, where it
will still be there in six months.
