# Workflow — waves, lanes, PRs, reviews, and where perf is graded (tidelock, rev 1)

> **Status:** rev 1, 2026-08-25. **DECIDED.** The process home: how a change travels from lane
> brief to `main`, binding every session — cloud, local, phone-spawned — via `CLAUDE.md`.
> `ROADMAP.md` §2 owns *which* lanes exist and their models; this doc owns *how* a lane ships.
> Written the day the four-leg CI matrix, the target-set ruling and the Pi 4's departure landed;
> supersedes the informal push-to-branch habit of W0–W2.

---

## 1. The unit of work: one lane, one draft PR

- A lane works on branch `w<N>-<lane>` cut from fresh `main`, and **opens a PR at lane
  start — ready, never draft (ruled 2026-08-26)** — before substantive commits pile up. The PR
  is not a request for outside review (external PRs are disabled; every PR is Rafael's own work,
  `CLAUDE.md` public-repo protocol); it is machinery: CI runs on every push, review findings
  have an anchor, and the branch auto-deletes on merge. Ready-not-draft because the draft flag
  gated nothing (§1's merge preconditions do the gating) while lane sessions cannot flip
  draft→ready and drafts cannot merge — the W2 ecs pilot had to merge through a ready successor
  PR, splitting one lane's record across two PRs; this ruling retires that pattern. When a
  session's tooling cannot open the PR (integration permissions), Rafael opens it from the
  branch's compare URL — one tap; everything else stays automated. The repo's squash/rebase
  merge toggles remain enabled in settings; the merge-commit rule below is convention until
  Rafael flips them off (a one-tap hardening).
- Inside the lane, `CLAUDE.md`'s operating contract applies unchanged: model gate, slice brief,
  one feature per commit, commit **and** push every turn, docaudit before docs commits.
- **Before its first review round, a lane MAY rewrite its own branch** (amend/force-push) —
  the sanctioned cure for a per-commit gate miss (e.g. a missing `[docs:none]`, which
  `commit_docs`'s per-commit design lets no later commit waive) — because pre-review the branch
  has no other consumer by convention. **Once review has begun, history is frozen**: fixes are
  new commits, and `CLAUDE.md`'s no-force-overwrite rule applies in full. (Ruled 2026-08-26;
  the W2 ecs pilot hit exactly this collision and resolved it this way.)
- **A lane's final report is its FILED record (ruled 2026-08-26, after the net-p1 closeout):**
  `TODO.md`/`LESSONS.md` entries and the PR body — never only a chat message, which dies with
  the session (steward sessions cannot read lane transcripts, and the two-PC rule already makes
  committed files the one durable channel). Every lane brief states this. **And every lane
  merge gets a closeout sweep**: the steward reads what the lane filed and triages each entry —
  act now, route to a named lane, or hold for the §3 wave sweep — before the lane is closed;
  a wave does not close with an untriaged filing. (The pilot's proof: net-p1's last two commits
  filed two ruling requests, one security-shaped, that no batch had seen.)
- **An autonomous lane runs to its ship gate without ending its turn (ruled 2026-08-26).**
  "Commit and push every turn" is a rule against leaving work unpushed, never a reason to stop:
  a lane does NOT end its turn at commit boundaries — it commits, pushes, and continues
  immediately to the next item. The ONLY stops are (1) a ruling request that blocks all
  remaining work (park cleanly, per `CLAUDE.md` rule 7 — continue on unblocked parts first),
  (2) a genuine external block, and (3) the ship gate itself (four-leg green + *ship* verdict →
  merge, then the lane is done). Status reporting is a side effect of those stops, never a
  reason for one — the commit trail and the PR are the live status surface. **Every lane brief
  cites this rule verbatim.** (The W2 net-p1 pilot ended its turn at each of four commits to
  report status — a misreading of the commit-and-push rule as a turn boundary; the lane's own
  post-mortem is the source of this wording.)
- **Merging is autonomous (ruled 2026-08-25).** Once the head is CI-green on all four `CANON.md`
  legs and §2's verdict is *ship*, the session merges without waiting for Rafael — his word is
  not a merge precondition; both preconditions are machine-checkable facts, not judgments. What
  still reaches him, as multiple-choice questions wherever the surface allows: **rulings** (RRs,
  and anything a spec is silent on), a reviewer's ***redesign*** verdict, **scope changes**, and
  any edit that *creates or amends a ruling* — there his sign-off is on the ruling itself; once
  given, the merge that lands it needs no second word.
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
- **Perf grading moves to an elected CI leg — elected: `ubuntu-latest` (ruled 2026-08-26,
  from perf.yml run 1's data; the request and the measurement record live in `TODO.md`).**
  `.github/workflows/perf.yml` measures the G-05 sweep N times per matrix leg, recording CPU
  model, medians and variance as artifacts. Run 1 measured the fleet as TWO silicon groups
  (both x64 legs AMD EPYC 7763; both arm64 legs Azure Cobalt 100, one reporting its Neoverse-N2
  core IP) — so the radar groups **by silicon, canonicalized, never by leg label**, and
  `ubuntu-latest` won on steadiness at the graded sizes (20k/50k p95 spread ≤ 0.5 %) plus ISA
  proximity to the Deck min-spec. The elected leg carries the **regression radar only**: the
  20k G-05 p50 median against a committed baseline, within the EPYC 7763 group — **never a
  comparison across models**. The radar bands live in `CANON.md` (Perf grading).
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

- **R-1 One lane = one PR**, merge commits, auto-delete; opened at lane start (§1; drafts
  retired by R-5).
- **R-2 The perf reference retires from owned hardware to the elected CI leg** (§4);
  **elected 2026-08-26: `ubuntu-latest`**, from perf.yml run 1's data (record in `TODO.md`).
- **R-3 Autonomous merges (ruled 2026-08-25, Rafael's request):** green CI + a *ship* verdict
  merge without a human word; Rafael's phone-side role is rulings, important decisions and
  choices — put to him as multiple-choice where the surface allows — never merge ceremony (§1).
- **R-4 Pre-review branch rewrites (ruled 2026-08-26):** a lane may rewrite its own branch
  before its first review round; after review begins, history is frozen (§1).
- **R-5 PRs open ready, never draft (ruled 2026-08-26):** the merge preconditions gate, the
  draft flag gated nothing and split the W2 ecs record across two PRs (§1).
- **R-6 Autonomous lanes do not stop at commit boundaries (ruled 2026-08-26, Rafael, after
  babysitting the net-p1 lane):** commit-and-push is an anti-unpushed-work rule, not a turn
  boundary; a lane runs to its ship gate and stops only for a blocking ruling request, a
  genuine external block, or the gate itself — and every lane brief cites this verbatim (§1).
- **R-7 Lane closeout sweep (ruled 2026-08-26, Rafael):** a lane's report is its filed record,
  and every merge is followed by a steward triage of that lane's filings before the lane
  closes; a wave never closes over an untriaged filing (§1).
- **R-8 Budget-aware wave sequencing (ruled 2026-08-26, Rafael):** the DAG decides what MAY
  run, the weekly token budget decides WHEN — Fable lanes launch just after a reset, never
  demoted to fit before one (§6).
- **R-9 Two-tier reviews (ruled 2026-08-26, Rafael):** breadth rounds fresh-context, middle
  rounds delta-scoped, the ship-verdict round a full re-read; the model per round is
  `ROADMAP.md` §2's policy (§6).
- **R-10 Steward economy (ruled 2026-08-26, Rafael):** the steward's tokens buy judgment,
  never mechanical reads — targeted queries, python over saved dumps, cheap-model subagents
  for harvesting, windows retired at phase boundaries (§6).
- **R-11 Lane token discipline (ruled 2026-08-26, Rafael):** tag-scoped iteration, local
  validation before every push, read-once specs, terse output, cheap subagents for search;
  every lane brief cites it (§6).

## 6. The token budget — scheduling and session economy (ruled 2026-08-26)

Fable 5 usage is capped per week (reset: Tuesday); the program codes every week without
stopping. Four rules, and quality is never the variable traded — the `ROADMAP.md` §2 model
policy ("never low effort on sim or netcode code") outranks every line here.

- **R-8 — sequencing.** Within a wave, Sonnet/Opus lanes launch first; a Fable lane is
  scheduled against the reset (it starts just after one, not squeezed in before the cap).
  `ROADMAP.md` §1's DAG bounds what may run in parallel; the budget only reorders inside
  those bounds. Deferring a Fable lane a few days is always cheaper than demoting it — the
  net-p1 review record (43 defects, five of them canonical-bytes classes feeding the §20.2.8
  hash chain) is the standing evidence that model strength at the sim/netcode gate pays.
- **R-9 — reviews.** Round 1 of any review is a fresh-context breadth pass; middle rounds
  verify fixes delta-scoped — the fix diff against the finding, not a whole-PR re-read; the
  ship-verdict round is always a full re-read. Which model runs which round is `ROADMAP.md`
  §2's policy (its home). The strong gate holds where it decides; the breadth and fix-check
  rounds stop billing the top tier.
- **R-10 — the steward.** The steward (model: `ROADMAP.md` §2) spends tokens on judgment,
  never on mechanical reads: CI queries are SHA-targeted with minimal page sizes; an
  oversized tool result is parsed from its saved file with python, never re-fetched or read
  raw; log and dump harvesting is delegated to a cheap-model subagent that returns
  conclusions, not content; and a steward window retires at a phase boundary with a
  committed-file handoff rather than growing — a long window re-bills its whole prefix every
  turn, cache-discounted but never free.
- **R-11 — inside a lane.** Iterate with tag-scoped test runs and run the full suite only
  before a push; validate locally before every push, because a red CI round costs a lane fix
  cycle PLUS a steward investigation; read a spec end-to-end once, then grep the working
  copy; terse commits and no narrative recaps (§1 R-6 already forbids status stops); broad
  searches go to cheap subagents. Every lane brief cites this rule.

*Rev 1 — 2026-08-25; §1/§4 amended same day by the slice's own adversarial review (D4/D6: the
PR-fallback actor named, absolute grading pinned to the PC rev-2 record until the Deck). This
doc shipped as the first §1-governed PR — opened via the App grant, review verdict recorded on
it. §1/§4/§5 amended 2026-08-26 by the morning ruling pass after the W2 autonomy pilot (R-2
election, R-4, R-5). §5/§6 amended 2026-08-26 evening: the token-budget rulings R-8..R-11.*
