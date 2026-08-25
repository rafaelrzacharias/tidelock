# Workflow — waves, lanes, PRs, reviews, and where perf is graded (tidelock, rev 1)

> **Status:** rev 1, 2026-08-25. **DECIDED.** The process home: how a change travels from lane
> brief to `main`, binding every session — cloud, local, phone-spawned — via `CLAUDE.md`.
> `ROADMAP.md` §2 owns *which* lanes exist and their models; this doc owns *how* a lane ships.
> Written the day the four-leg CI matrix, the target-set ruling and the Pi 4's departure landed;
> supersedes the informal push-to-branch habit of W0–W2.

---

## 1. The unit of work: one lane, one draft PR

- A lane works on branch `w<N>-<lane>` cut from fresh `main`, and **opens a draft PR at lane
  start** — before substantive commits pile up. The PR is not a request for outside review
  (external PRs are disabled; every PR is Rafael's own work, `CLAUDE.md` public-repo protocol);
  it is machinery: CI runs on every push, review findings have an anchor, and the branch
  auto-deletes on merge. When a session's tooling cannot open the PR (integration permissions),
  Rafael opens it from the branch's compare URL — one tap; everything else stays automated.
  The repo's squash/rebase merge toggles remain enabled in settings; the merge-commit rule below
  is convention until Rafael flips them off (a one-tap hardening).
- Inside the lane, `CLAUDE.md`'s operating contract applies unchanged: model gate, slice brief,
  one feature per commit, commit **and** push every turn, docaudit before docs commits.
- **Merge method: merge commit — never squash, never rebase.** One-feature-per-commit history
  is the project's record (`git log` is where history lives, `README.md` doc map), and rebases
  invalidate the other machines' and sessions' checkouts. The merged branch auto-deletes; a
  branch pointer whose commits are all in `main` is clutter, not history.

## 2. Review: adversarial, fresh-context, to a *ship* verdict

- Every lane gets a **fresh-context adversarial review** by a different session on a
  higher-or-equal model (`ROADMAP.md` §2 model policy), its findings posted on the PR, fixed,
  and re-reviewed until the verdict is *ship*. CI must be green on all four `CANON.md` target
  legs **before** the review is requested — reviewer attention is spent on working code.
- **The post-review-edit valve:** small edits that *implement an already-recorded ruling* may
  merge with their lane review deferred to the wave-boundary sweep (§3). The deferral is
  recorded in `TODO.md` at merge time, never assumed — the W1 sweep (2026-08-25) found real
  defects behind exactly this valve, which is why the sweep is mandatory, not ceremonial.

## 3. The wave boundary — three artifacts, not a benchmark

A wave closes with: (1) the ★ lane's done criterion met (`ARCHITECTURE.md` §9); (2) a
**wave-boundary review sweep** covering everything §2's valve deferred, verdict recorded in
`TODO.md`; (3) `ROADMAP.md`'s retro line (its own footer requires it). Benchmarks run only
where a gate doc defines one (`GATE0-BENCH.md`, the `ALLOY.md` test gates, `NETCODE.md` §19
milestones) — a wave is a scheduling unit, not a measurement protocol.

## 4. Where perf is graded (re-ruled 2026-08-25 — owned hardware retired)

- **Conformance** (bit-exact traces, one `build_id`) is the four hosted CI legs, every PR —
  the `CANON.md` target matrix.
- **Perf grading moves to an elected CI leg.** `.github/workflows/perf.yml` measures the G-05
  sweep N times per matrix leg, recording CPU model, medians and variance as artifacts; the
  election of the reference leg is a **ruling made from that data** (request filed in
  `TODO.md`), never before it. The elected leg carries the **regression radar only**: medians
  against a committed baseline, grouped by CPU model — **never a comparison across models**
  (the fleet is heterogeneous, so every measurement records its silicon). The radar bands live
  in `CANON.md` (Perf grading).
- **Absolute grading is suspended at the committed PC rev-2 record**
  (`tests/gate0/results/`, the last fixed-silicon measurement) until the Steam Deck —
  the shipping min-spec machine and the only fixed silicon in the program's future — is
  benched and **re-anchors the `GATE0-BENCH.md` §2 thresholds by ruling**. A shared runner
  never grades an absolute number: an elected *label* is not fixed silicon.
- **Personal machines are playtest instances, nothing more**: CI's `ship` artifact is the
  download for human look-and-feel sessions (`TODO.md` carries the upload step, landing with
  v0's first playable). The physical PCs remain Hovel's *network-soak* hardware
  (`NETCODE.md` §19.4) — that role is about real packets on a real LAN, not perf reference.

## 5. Rulings (closed 2026-08-25 — nothing open)

- **R-1 One lane = one draft PR**, merge commits, auto-delete; opened at lane start (§1).
- **R-2 The perf reference retires from owned hardware to the elected CI leg** (§4); the
  election itself waits on `perf.yml`'s first data set and is filed in `TODO.md`.

*Rev 1 — 2026-08-25; §1/§4 amended same day by the slice's own adversarial review (D4/D6: the
PR-fallback actor named, absolute grading pinned to the PC rev-2 record until the Deck). This
doc shipped as the first §1-governed PR — opened via the App grant, review verdict recorded on
it.*
