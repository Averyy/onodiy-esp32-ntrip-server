# Shipped feature review follow-ups

Source review on 2026-10-26, covering the shipped feature families while the
network-latency development build was being qualified. Fixes are being qualified
sequentially in a separate development build. No receiver firmware update or
intentional network failure was attempted on a production station for this review.

Back to [development](development.md) · [Latency work](todo-network-latency.md).

## High priority

- [x] **Receiver flashing must stop if its reboot protection cannot be acquired.**
  [lockout_take](../components/station/station.c) logs failures to claim the update
  slot or reboot inhibit, but returns no error. `flash_new` and `flash_action`
  proceed into destructive receiver operations. A reboot can commit while a new
  request is still identifying the receiver or validating its package; the
  inhibit then correctly refuses acquisition, yet flashing continues during
  shutdown. Make full acquisition fail closed. Unwind resources acquired by the
  attempt, preserve an existing journal, and park retry/recovery for the user.
  Test reboot commitment between validation and lockout, inhibit exhaustion,
  and retry of an existing journal. Assert that no reset/loader/transfer command
  is sent after acquisition failure. This is source-confirmed, not a reproduced
  hardware interruption. Fixed by acquiring full protection at job entry before
  identification or RAW access, with transactional acquisition, retained/parked
  existing journals and serialized request admission. Five host regressions pass;
  the host suite totals 1,334 passing tests at this step. The ESP-IDF target builds.
  Follow-up review also reserves request admission across boot parking and
  session completion, through lifecycle release and the final progress event.
  A release-callback regression checks both ownership handoffs; the combined
  host suite now passes 1,344 tests.

## Medium priority

- [x] **Preserve TLS read/write readiness requirements.**
  [sess_send / sess_recv](../components/ntrip_server/ntrip_server.c) collapse
  `WANT_READ` and `WANT_WRITE` into one would-block result. The callers then
  wait for their usual direction. During TLS 1.2 renegotiation a write can need
  inbound data; waiting for an already writable socket can spin the sender task
  and consume CPU needed by other streams. Preserve the requested readiness
  direction for both writes and reply reads. A TLS write must retain its input
  pointer/length until positive progress. Do not use the buffered-plaintext
  shortcut in `sess_wait` as proof that a write's handshake can continue.
  Keep existing timeout/abort policies. Test actual firmware with a TLS peer,
  renegotiation and a concurrent healthy rover. This does not explain latency
  on a plain-TCP caster connection. Fixed with explicit transport readiness
  results and unchanged pending-write spans. Three new host regressions pass
  (1,337 total). The real TLS 1.2 QEMU run passes 31 checks, including verified
  custom-CA trust, server renegotiation, recorded-frame integrity and restart.
  Final TLS/rover qualification passes 92 checks: 448 healthy-rover epochs
  without gaps, byte agreement with upstream, stalled-client isolation,
  active diagnostic probes, source tables and clean planned restart.

- [x] **Do not consume an automatic recovery action before dispatch succeeds.**
  [net_ladder_tick](../components/network/net_ladder.c) advances its rung before
  [network_ladder_tick](../components/network/network.c) queues the action.
  A full queue drops it, but the rung remains consumed and the action counter
  and journal already say it started. Separate decision from accepted dispatch,
  retain a bounded pending action, and count/journal its actual admission.
  Test a full queue followed by capacity becoming available. Also revalidate
  automatic queued actions against restored connectivity before execution;
  distinguish explicit user recovery requests from obsolete automatic ones.
  Fixed with one retained offer, generation-checked execution admission and
  fresh health/network evidence. Six new host regressions cover refused sends,
  cancellation, delayed execution, policy changes and late acknowledgements;
  all 1,343 host tests pass, and the ESP32-S3 target builds. Manual recovery
  messages retain their existing path.

- [x] **A failed logout must not appear confirmed.**
  [logout](../components/web/ui/js/20-shell.js) catches all DELETE failures and
  displays Login anyway. If the request never reached the station, the HttpOnly
  remembered cookie and server session still authenticate after a reload.
  Treat success or an already-unauthorized response as confirmed; clear visible
  secrets on every path, but show an unconfirmed-logout error and retry when
  transport fails. Test a lost request separately from a lost response.
  Fixed with a pending/uncertain logout page, an explicit retry, duplicate-click
  protection and a generation guard against delayed password replies. Ten
  browser-harness regressions exercise successful and failed confirmations,
  lost responses and late secret responses.

- [x] **Reconcile receiver-update UI state after missed completion events.**
  [Updates load](../components/web/ui/js/70-updates.js) initializes `rflow` from
  polling, but only the final progress event settles an existing flow. If that
  event is lost while disconnected, later successful snapshots can leave the
  page showing downloading/installing indefinitely and hide the last outcome.
  Reconcile active and completed state from `/api/update` and `/api/gnss/flash`,
  with job identity checks so an old outcome cannot complete a newer operation.
  Test disconnection at download, package check and flash completion.
  Fixed with explicit queued/running flags, ordered downloader-to-receiver
  reads, one in-flight read, and local action/event revisions. Unavailable or
  indistinguishable old results are reported as unknown rather than success.
  Ten browser-harness regressions cover missed events, queued handoffs,
  repeated jobs, delayed replies, refusals and teardown. Status-builder tests
  cover the additive flags; the host suite passes 1,345 tests and the target builds.

- [x] **A failed receiver-status refresh must not hide a recovery session.**
  [Updates load / renderSession](../components/web/ui/js/70-updates.js) converts a
  failed `/api/gnss/flash` request to null, then treats null as no open session.
  Preserve the last snapshot with an explicit stale/error state and disable
  recovery actions until refreshed. Test one endpoint failing while the general
  update endpoint succeeds, and recovery after the next successful read.
  Fixed with separately tracked freshness/errors, retained session details,
  an explicit unknown state without a prior snapshot, and action/confirmation
  guards. Safety-related button changes bypass ordinary interaction deferral.
  Seven focused regressions pass, along with the ten completion-flow tests.
  Same-phase progress counters no longer starve a slow ordered refresh.

## Improvements and qualification

- [x] **Audit the actual station-build configuration before deployment.**
  The earlier local development image inherited a stale root configuration
  with receiver flashing disabled, despite the shipped default being enabled.
  Its saved status confirms that feature regression. The successor uses an
  isolated configuration with receiver flashing enabled; verify the generated
  configuration and deployed API, not only the defaults file. No receiver
  firmware update is needed to restore the app's capability.

- [x] **Keep source-table bitrate sampling stable across timer jitter.**
  TLS/rover qualification found a zero advertised bitrate despite continuous
  RTCM. The sampler can reject a callback 999 ms after its last sample, then
  reset history on the next callback 2,001 ms after that sample. Reproduce the
  cadence in host tests and retain rate history without weakening stale-data,
  counter-reset or wraparound handling. Actual callback timestamps were not
  logged in the failing emulator run, so the causal link still needs validation.
  Fixed with nominal sampling deadlines and actual sample timestamps. Three
  regressions cover jitter, deadline catch-up, resets and wraparound; all 1,348
  host tests pass. The unchanged NTRIP 1 and NTRIP 2 source-table assertions
  both pass in the final TLS/rover emulator run (92 checks, no failures).

- [ ] **Show how long each Onocoy/NTRIP outage lasted in Logs → History,
  alongside the daily totals.**
  - [x] Journal the return after every journalled drop once the slot streams
    again, however soon. It was skipped when the first attempt reconnected
    within 10 s; the fast profile's first retry is 1 s. Host tests cover the
    immediate return, the 10-minute flapping limit and one return per drop.
    Saved settings and restarts the user asks for are not an outage: their
    close is not journalled, nor the reconnect unless an attempt fails. A
    pause for receiver work past the idle limit (or a stall still silent
    from one after the resume) is journalled as planned and shown as a
    neutral "paused" row; health-monitor restarts stay outages.
  - [x] Put the outage's length first in the event list ("Onocoy offline
    40s"), keep the session length, give each outage its own row instead of
    folding it into a run, and add an Onocoy filter. Console log lines now say
    how long a session lasted and, on reconnecting, how long the slot was
    offline and how many attempts failed.
  - [ ] Flapping: sessions shorter than 60 s within 10 minutes of the last
    journalled drop are counted into the next drop, but their offline time is
    not recorded. Recording it would need a new journal kind (kind values
    are never renumbered); decide once the new records show how often it
    happens.
  - [ ] Reconcile the listed outages with the day's offline total. The ledger
    also counts receiver RTCM gaps while the session stays open, start-up after
    a restart, power-off gaps and stopped slots. The journal may not reach back
    30 days, and outages can cross midnight or a restart.
  - [ ] An outage across a restart is not paired: the first return after a boot
    counts from that boot's first failed attempt, not from the drop before the
    restart.
- [ ] Separate a lost reply at the internet target from a lost internet path.
  The internet ping is one ICMP echo to `1.1.1.1` every 30 s. Compare the
  router, Cloudflare and caster losses per bucket (`GET /api/monitor`) over
  several days first; if Cloudflare's losses do not coincide with the router's,
  try a second target only after a lost reply and count the internet lost
  only when both fail.
- [ ] Add an upload progress/response watchdog to the raw XMLHttpRequest helper;
  currently a stalled upload response can leave its UI pending indefinitely.
  Never automatically replay uploads: reconcile server state after uncertainty.
- [ ] Give the Receiver sky map the same explicit failed-refresh indication as
  Status. Audit event-triggered background reads for visibility and single-flight
  guards without suppressing necessary operation-completion reconciliation.
- [ ] Qualify TLS and NTRIP 2 raw-body firmware transmission, plus upstream
  backpressure with live diagnostic exports. Existing recorded-stream QEMU tests
  cover NTRIP 1 upstream, NTRIP 2 chunked upstream and local rover isolation;
  the latency candidate's local-rover scenario also enables active probes.
  Optional Docker BKG tests exercise the Python clients with NTRIP 1, not the
  firmware's TLS transport.
- [ ] Correlate a real remote latency alert with local arrival/write/TCP and probe
  times before changing freshness, reconnect or buffering policies. Automatic
  skipping/reconnecting remains deferred because of complete-epoch and shared
  radio recovery risks.
- [ ] Account for LAN rover activity in shared Wi-Fi recovery decisions. The
  current AP-client guard protects clients on the station's own access point,
  not every rover connected through the router. Sustained WAN trouble can still
  escalate into a radio action that interrupts useful LAN corrections. Keep
  genuine radio failures recoverable and test both cases before changing policy.

## Coverage and limits

The corrected development image was deployed to the owner-authorized canary
after 1,348 host tests on macOS and pinned Linux, 770 bench/UI tests, 87 emulator
smoke checks and 92 TLS/rover checks passed. Live qualification captured 600
consecutive epochs and 5,404 RTCM frames over ten minutes without CRC errors,
epoch gaps, caster drops or reconnects. Receiver/caster/rover settings were
unchanged; all 115 satellite refreshes and 463 probes per target succeeded.
The deployed API confirms receiver flashing is enabled. Trial confirmation and
stable boot passed; the 24-hour crash watch remains ongoing. Temporary probes
and the test rover were stopped. No receiver firmware transfer or forced
network failure was performed on the production canary.

| Feature family | Review focus / evidence |
|---|---|
| RTCM parsing, hub and local rovers | Frame classification, overwrite validation, metadata separation, per-client pending-buffer ownership; recorded-stream tests |
| Upstream NTRIP | Request/chunk framing, independent slots, TLS readiness, partial-write invariants and diagnostic overhead |
| Network and health | Scan ownership, recovery queue dispatch, planned drains and shared radio effects |
| Receiver setup and firmware | Profile error mapping, identification/validation, journal recovery, reboot/update-slot protection |
| Application updates | Trial confirmation, rollback and app-only upload contract; authorized canary deployment |
| Web UI and API | Satellite polling, body timeouts, logout, update progress/session reconciliation, authorization and request gates |
| Onocoy reports | Fixed HTTPS destination, opt-in eligibility, bounded response parsing, credential-generation checks and retry scheduling |
| Availability/history | Existing host-suite coverage; this quick pass does not independently qualify long-duration persistence or power-loss behavior |

No additional concrete defect was found in the reviewed frame/cursor ownership,
profile error mapping, journal sequencing, OTA trial rollback or API request
authorization paths. That is a scoped source-review result, not exhaustive
qualification of every hardware/error combination. Completed fixes have the
validation recorded above; unchecked improvements remain deferred pending
their own implementation review and meaningful tests.
