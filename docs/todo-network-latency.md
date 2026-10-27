# Firmware latency TODO

Investigate four firmware improvements for a stationary reference station whose
RTCM reaches a remote caster late during brief slowdowns. Local rover clients and
other caster slots must continue receiving corrections independently. This task
records proposals, review findings and validation requirements. The local dev
implementation is summarized below; no deployment is authorized by this file.

Back to [development](development.md).

The broader [shipped-feature review](todo-shipped-feature-review.md) records
additional bugs and improvements separately from this candidate's changes.

## Implementation decision

Implement proposals 1, 2 and 4, and observation-only arrival metadata from
proposal 3. Both independent sender and rover reviews advised deferring automatic
age-based skipping or reconnects until complete-epoch and isolation evidence
justifies a specific policy. Existing send deadlines, buffers, retries, hub
consumer policies, local-caster delivery and recovery ladder remain unchanged.

The local implementation adds the negative-scan progression through 24 h,
explicit scan ownership/cancellation and radio-lifetime admission; a fixed
eight-event slow/failed-write history per caster; a read-only TCP socket snapshot
through the pinned lwIP hook; and an optional 600-cycle probe recording with an
admin API and Logs → Network controls. See the [API contract](../components/web/API.md#get-apimonitordiagnosticscursorn-adm).
Passive diagnostics retain local evidence rather than infer end-to-end delivery.

The TCP adapter lives in a separate lower-layer `tcp_diag` component, preserving
the rule that network and NTRIP components do not depend on each other. It uses
the public getsockopt socket reference and TCP/IP serialization, preserves IDF's
existing extended-option hook, and reads only fixed scalar fields. Unsupported
desktop options such as TCP_INFO or SO_SNDBUF are not assumed available.

Both transport reviewers advised sampling TCP only between complete application
writes. The synchronous getsockopt handoff has no caller deadline, so it is
excluded from partial TLS/chunk completion and active write deadlines. It can
still delay the next write; cadence is bounded, callback wait is not. During a
blocked write the last TCP sample ages and must not be interpreted as current.
A bounded asynchronous sampler would require a separate socket-lifetime design.
Known planned closes, drains and abort conditions skip sampling. Each actual
snapshot call records its duration, including the synchronous handoff wait;
per-slot saturating counters and eight duration buckets persist across reconnects
until reboot. This makes hardware overhead measurable without extra socket calls.
TCP samples are retained with slow/failed writes, not as a continuous history;
a brief backlog hidden by send buffers may require a live capture to observe.

Cross-review also requires local probe receive/select failures to remain local
errors and each target's RTT/timeout to use its own dispatch timestamp. Probe
work uses its own socket/task and short snapshot locks; normal monitoring stays
at 30 seconds. No per-packet text or flash logging is added.

## Evidence and limits

- Remote latency alerts can occur while the firmware still reports a streaming
  session. A successful socket write means local TCP accepted bytes, not that the
  caster received or processed them.
- The remote caster's observation latency includes the receiver and the network;
  it is not the same measurement as a ping round trip.
- The hub's lag policy operates before socket transmission and preserves an
  epoch already partly delivered. It is not an end-to-end delivery deadline.
- The supplied logs do not establish that roaming scans caused the latency
  incidents. Firmware investigations must distinguish local delays from delays
  after transmission, without assigning an unverified cause.

## 1 Back off unsuccessful roaming scans

Checked items are implemented or source-reviewed locally; hardware radio validation remains below.

- [x] Fix scan-result validity before adding long backoffs. The Wi-Fi event
  handler in v0.1.10 queued `MSG_SCAN_DONE` without the driver's completion
  status or scan identifier; the roaming handler then processes the records as
  a successful result. Research failed, cancelled and late completions so none
  can incorrectly count as an unsuccessful search and earn a 24 h delay.
- [x] Check scan ownership across watchdog cancellation and queued user scans.
  Preserve result status and associate it with the right operation; distinguish
  verified failure paths from races that still need reproduction.
- [x] Implement and host-test adaptive intervals after completed scans find no
  worthwhile alternative: **10 min, 30 min, 1 h, 3 h, 6 h, 12 h, then 24 h**.
- [x] Keep the 24 h cap while results and signal conditions remain unchanged.
  Small fluctuations around the weak-signal threshold must not reset it.
- [x] Define a smoothed, sustained signal-deterioration trigger that resets the
  backoff without creating repeated scan bursts. Its numeric thresholds need
  investigation, not guesswork.
- [x] Preserve normal reconnection after link loss, the stronger-AP confirmation
  scans, access-point-directed roaming and the existing update/recovery holds.
- [x] Keep failed scans separate from successful searches with no candidate.
  Internet or caster latency alone must not trigger extra Wi-Fi scans.
- [x] Clear candidate confirmation hits after a failed or cancelled scan. The
  v0.1.10 `net_roam_scan_failed()` kept those hits, allowing an old positive
  result to survive failed confirmation attempts.
- [x] Define how discovering or losing a second AP affects the schedule, and
  whether the schedule resets on association or persists across reboots.

Source: [roaming policy](../components/network/net_roam.c),
[policy constants](../components/network/priv_include/net_roam.h),
[driver integration](../components/network/network.c),
[existing host tests](../test/host/main/test_net_roam.c).

Validation must cover the entire progression and cap, threshold oscillation,
signal deterioration, scan failures, candidate confirmation, link loss,
operational holds and monotonic-clock wrap. Scans use the shared radio, so check
local rover timing as well as remote caster timing.

### Scan validity and ownership findings

The findings in this subsection describe the v0.1.10 baseline and motivated the
new ownership core. The proprietary Wi-Fi driver's ordering still needs a later
authorized hardware check; QEMU has no radio.

In the v0.1.10 baseline the completion payload is discarded in `network.c`'s Wi-Fi event handler.
ESP-IDF's `wifi_event_sta_scan_done_t` carries a success/failure status, but
`handle_scan_done()` selects the recipient using the current user/roaming flags
and treats drained roaming records as a successful result. Preserve failure
status before a negative result is allowed to advance the backoff.

A queued user request also starts its watchdog before it starts scanning. If the
user and roaming watchdogs expire in the same housekeeping pass, the user flag is
cleared first; `roam_scan_end()` can then start the queued scan without restoring
that flag. This is a reachable state-management defect in the source; its timing
has not been reproduced on hardware.

There is a separate late-completion risk: the roaming watchdog stops a scan and
immediately starts a queued user scan. A completion from the stopped operation
may then be attributed to the new operation. Espressif documents that both
stopping and overriding a scan generate a completion event
([scan-done event contract](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi-driver/overview.html#wifi-event-scan-done)).
The public start API returns no scan identifier to bind to a request, so carrying
the completion's `scan_id` alone is not an established ownership fix.

Recommended prerequisite: serialize active owner, queued request and cancellation
as separate states in the network task. Start each watchdog when its driver
operation starts; consume cancellation/completion before starting its successor,
with bounded handling of a genuinely missing event. Preserve completion evidence
across message-queue overflow and keep stale-event cleanup from clearing a newer
operation's results.

### Backoff design recommendation

Keep the routine negative-result tier separate from the 30-second confirmation
and failed-scan retry timers. Advance only on valid negative results; saturate at
24 hours. A network with no alternate ever seen should start at its existing
six-hour interval, then reach 12 and 24 hours, so this change does not make
single-AP installations scan more often. Discovering an inadequate alternate
does not need to reset an already mature tier.

Keep this state in RAM initially and reset it on a new association. Holds, user
scans, small RSSI threshold crossings and WAN latency leave the tier alone; a hold
ending permits one eligible scan, not a burst of missed scans. A deterioration
reset should be relative to a stable smoothed-signal reference, latched and
rebased after use, with a minimum routine-scan gap. A 6 dB change sustained for
60 seconds is a candidate for validation, not an established threshold. The
24-hour cap deliberately trades slower discovery of a newly improved AP for
fewer interruptions on a stable connection.

## 2 Make brief transmission stalls observable

- [x] Record slow write episodes, their duration and progress, pending-data age,
  affected caster slot, and scan start/end times. Keep hot-path overhead bounded
  and rate-limit logs.
- [x] Preserve monotonic timestamps and add UTC when trusted, so network events
  can be compared with remote reports even across clock synchronization.
- [x] Design a temporary diagnostic mode with approximately one-second router,
  internet and caster measurements and bounded history in RAM. The current
  monitor samples every 30 seconds and stores five-minute buckets.
- [x] Keep probe scheduling independent of slow probe replies and RTCM work.
  Establish memory, CPU and traffic budgets and an automatic expiry.
- [x] Distinguish queued locally, acknowledged by TCP, and processed by the
  remote service; only claim measurements the firmware actually has.
- [x] Update API documentation and existing API tests if new fields are added.

Source: [caster writes](../components/ntrip_server/ntrip_server.c),
[monitor task](../components/monitor/monitor.c),
[monitor storage](../components/monitor/include/monitor.h),
[diagnostic journal](../components/diag/diag_journal.c),
[web API](../components/web/API.md).

Validation must include multiple simultaneous slow caster slots, unavailable
probe targets, uncertain UTC, continuous receiver traffic and local rover
clients. Record gaps and sampling limits rather than interpreting a missing
sample as a healthy link.

## 3 Assess stale data handling in the transmit path

Two independent assessments recommend deferring automatic age-based drops and
reconnects. Proceed with timestamp propagation and observation first. Any later
age policy should be per caster, experimental and disabled by default until its
effect on complete received epochs and local rover continuity is established.

- [x] Carry observation/frame arrival evidence through the send path and examine
  an absolute age budget rather than only a no-progress timeout that resets on
  each accepted byte. Keep measurement age, UART arrival age and socket wait
  distinct; cached station metadata needs its own treatment.
- [x] Determine when a wholly unsent stale epoch can be skipped by just the
  affected consumer. Preserve RTCM frames, MSM epoch boundaries and HTTP chunks.
- [x] Determine what can safely happen after part of a frame, epoch, HTTP chunk
  or TLS record has already been submitted. Compare waiting with ending only
  that caster session; include reconnect delay and login-guard costs.
- [x] Verify socket-close behavior: queued TCP bytes may outlive a normal
  application close. Do not assume reconnecting automatically purges them.
- [ ] Keep any remote-caster policy local to that slot. Do not clear or pause the
  shared hub, restart the receiver or Wi-Fi, or change local rover lag policy to
  address one remote caster's delay.
- [x] Evaluate indirect effects through health recovery, shared task scheduling,
  locks, buffers, memory and the radio. Per-consumer state alone is not proof of
  rover isolation.
- [x] Research the protected-caster escalation path explicitly. A disconnected
  protected caster feeds network recovery while RTCM is healthy; short healthy
  spells do not immediately reset the episode. Determine when retries caused
  by a proposed freshness policy could lead to reconnecting or restarting Wi-Fi
  and interrupting local rovers. This is a risk of the proposed policy, not
  evidence that the supplied latency incidents already caused those actions.
- [ ] Specify how a deliberate per-slot freshness recovery is distinguished from
  a real association, address or upstream-connectivity fault. Preserve recovery
  for genuine network failures rather than suppressing the entire ladder.
- [ ] Select a policy only after comparing delivered complete epochs and rover
  continuity with the current behavior during realistic interruptions.

Source: [sender](../components/ntrip_server/ntrip_server.c),
[sender state machine](../components/ntrip_server/ntrip_proto.c),
[hub contract](../components/rtcm_hub/include/rtcm_hub.h),
[hub core](../components/rtcm_hub/rtcm_hub_core.c),
[local caster](../components/local_caster/local_caster.c),
[health recovery](../components/health/health.c).

Required comparisons include short and prolonged slowdowns, partial writes,
slow trickle progress, packet loss, remote zero-window/backpressure, TCP and TLS,
NTRIP 1.0 and 2.0, paused receiver work and planned shutdown. Exercise at least
two independent consumers including a continuously receiving local rover, using
the existing test and bench infrastructure and recorded RTCM fixtures. Track
complete received epochs and their age, not just bytes accepted by send().

### Independent opinions

| Assessment | Recommendation | Main reasons |
|---|---|---|
| Sender and transport | Keep the current recovery behavior while measuring; evaluate a per-caster pending-write ceiling later | TLS may have submitted bytes despite a would-block return; ordinary close need not discard queued TCP bytes; reconnect and login-guard costs can exceed a short delay |
| RTCM integrity and rover isolation | Defer automatic drops/reconnects until epoch accounting and recovery isolation are proved | An unsent buffer can continue an already-sent epoch; current shared recovery can interrupt rovers even when the initial decision belongs to one caster |

Both opinions agree that the 1,000 ms Onocoy hub limit intentionally yields to
finishing an epoch already started. Local rover consumers have their own
2,000 ms limit; cached station messages have a separate 30-second replay limit.
Changing all three into one hard observation deadline would alter their existing
contracts.

The hub advances its consumer cursor and epoch state as it copies bytes, and a
batch can cross epoch boundaries. The sender updates its epoch tracker before
the socket write. Therefore zero bytes written from the current buffer does not
prove that its whole epoch is unsent. A future skip operation needs frame/epoch
boundaries, arrival metadata, staged sender accounting and consumer-local loss
accounting. `rtcm_hub_resync()` alone records idle skips and would hide deliberate
losses.

For TLS, a would-block return can follow a partial encrypted-record flush.
Retries must preserve the same write arguments. Once a TLS write has been
attempted, replacing its buffer is unsafe unless the connection is destroyed.
Ordinary close also leaves TCP teardown to the stack, which may send already
queued bytes. Abortive teardown is a separate feature to investigate; it still
cannot retract bytes received or queued downstream.

### Protected caster recovery and rover safety

The current source path is `stream_step()` write failure, `session_failed()`,
`hm_protected_down()`, then `net_ladder_triggers()`'s protected-caster trigger P
while receiver data remains healthy. Retry/backoff and retry connection attempts
after a write stall can hold that trigger. An uninterrupted 60-second healthy
period without a trigger clears the pass; shorter recoveries preserve its clock
but no action runs while the trigger is absent. If association-without-IP (A)
or unreachable-caster (S) participated, calm also requires an IP and, after S,
no unreached slot. Repeated passes are separated by a 600-second hold.

| Rung due after pass start | Action if permitted by fresh probe evidence and existing gates |
|---|---|
| 60 s | Reconnect Wi-Fi |
| 180 s | Restart DHCP and use fallback DNS |
| 360 s | Restart Wi-Fi |
| 600 s | Consider restarting the ESP32 |

These are due times, not unconditional execution times. A successful fresh
upstream probe completed at or after that rung became due skips an action.
A probe that does not finish within its
60-second allowance becomes `NO_UPSTREAM`: that can permit the first three
actions, but not an ESP32 restart. Reboot additionally requires the protected
trigger and `NO_IP` or `DNS_FAILED`, plus the existing restart gates.

The AP-client guard counts clients of the station's own Wi-Fi AP, not rover
clients on the normal LAN. It does not block the first reconnect. Another
streaming caster is not currently supplied as a protective ladder input.
Consequently this is a current rover-continuity limitation, and a specific risk
of adding frequent intentional freshness reconnects; it is not proof of the
cause of the reported latency incidents.

Recommended design investigation: give intentional freshness recovery a distinct,
bounded cause and exclude only that cause from P-trigger evidence. Do not label
it healthy, a maintenance pause, a remote takeover or a generic write stall.
Preserve the independent association-without-IP trigger A, unreachable-caster
trigger S and subsequent real DNS/connect/link failures. Rate-limit policy
reconnects; exhausting the policy budget should fall back to ordinary transport
handling, not indefinitely exempt a broken connection from recovery.

Also evaluate positive evidence of local connectivity before disruptive shared
recovery. `NO_UPSTREAM` comes from external TCP probes and cannot by itself
identify a broken local interface. Recent successful LAN or other-caster traffic
could inform that decision, with explicitly bounded validity. Do not suppress
the entire ladder or disable recovery for genuine local faults.

Source: [health classification](../components/health/health_mon.c),
[health inputs](../components/health/health.c),
[network ladder](../components/network/net_ladder.c),
[ladder timings and contract](../components/network/priv_include/net_ladder.h),
[interface actions](../components/network/network.c).

### Required integrity and isolation checks

Keep a healthy local rover receiving while the protected slot alternates failures
and recoveries shorter than 60 seconds, across every probe outcome and ladder
interval. Check received frame order and CRC, complete epochs, added arrival
delay, disconnects, loss accounting and bounded TCP/memory use. Include multiple
buffers per epoch, multiple epochs per buffer, partial HTTP chunks, TLS pending
records, cached metadata, receiver holds and planned drains. The acceptance
criterion is zero additional rover loss or interface recovery caused solely by
the new caster policy.

Include 59-second versus 60-second healthy gaps, the second pass after its hold,
and simultaneous healthy and stalled local rover clients.

Reuse the [existing rover scenarios](../tools/qemu/README.md),
[recorded fixtures](../test/fixtures/README.md) and
[timing tools](../tools/bench/README.md#measuring-latency). For historical replay,
measure against the replay schedule rather than subtracting old GNSS timestamps
from today's clock. QEMU skips actual Wi-Fi recovery, so it can validate framing
and state decisions but cannot prove radio isolation; that needs a later
authorized hardware bench comparison.

## 4 Measure TCP backlog before tuning buffers

- [x] Investigate safe ESP-IDF v6.1/lwIP access to queued and unacknowledged bytes,
  acknowledgment progress and retransmission evidence for one caster socket.
- [x] Read the pinned stack source before selecting APIs. Check socket lifetime,
  descriptor reuse, TCP-thread ownership, TLS compatibility and build variants.
- [x] Prefer a narrow read-only integration; avoid global TCP buffer or retry
  changes that would also affect local rovers and the web server.
- [x] Document which delays remain invisible, including acknowledged data held
  downstream, and avoid presenting TCP acknowledgments as application delivery.
- [ ] Use measurements to decide whether a buffer or recovery change improves
  complete, timely epochs enough to justify its losses and complexity.

Source: [socket setup and writes](../components/ntrip_server/ntrip_server.c),
[network integration](../components/network/network.c),
[build defaults](../sdkconfig.defaults), and the ESP-IDF v6.1 lwIP sources used by
the build. Standard desktop socket options must not be assumed to exist here.

## Investigation status

- [x] Independent advice on proposal 1.
- [x] Research failed and late scan-result handling before adaptive backoff.
- [x] Independent advice on proposal 2.
- [x] Independent sender/transport advice on proposal 3.
- [x] Independent rover/integrity advice on proposal 3.
- [x] Research protected-caster retries escalating into shared Wi-Fi recovery.
- [x] Independent stack-integration advice on proposal 4.
- [x] Reconcile findings, record unresolved choices and order implementation.

Initial order: investigate all four; prefer observability and conservative scan
backoff before changing transmission behavior. Proposal 3 remains contingent on
the isolation and complete-epoch comparisons above.

## Local validation, 2026-10-26

- Station dev build: ESP-IDF v6.1, normal station defaults, bench console and
  USB mirror disabled. App fits the smallest 8 MB board's OTA slot; both 8 MB
  and 16 MB full-install images pass checksum and flash-size validation.
- Host suites: 1,329 passed on macOS and 1,329 in the pinned Linux CI container;
  no compiler warnings in project sources in the Linux run.
- Bench suite: 743 passed; five opt-in Docker-caster tests and one release-tag
  test skipped.
- QEMU smoke/API: 87 passed, including diagnostic authentication, lifecycle,
  cursor handling and unavailable evidence.
- QEMU local caster: 89 passed. While probes ran and another rover stopped
  reading, the healthy rover received 450 epochs with zero gaps and whole frames;
  the upstream NTRIP 1.0 session stayed connected with zero hub drops. TCP
  snapshots, ACK baselines, clean shutdown and post-restart login passed.
- QEMU NTRIP 2.0 upstream: 28 passed, including recorded-stream integrity and
  receiver maintenance across a planned restart.
- Independent systems/API/UI reviews finished with no outstanding concrete
  findings. Review fixes include radio-lifetime scan admission, per-target probe
  timing, local-error classification, minimum probe spacing, inter-write-only
  TCP sampling, and explicit stale/unavailable diagnostic views.

These emulator checks do not qualify real Wi-Fi timing or TLS under hardware
contention. Subsequent owner-authorized hardware qualification is recorded below.
No commit or push was made.

## Status satellite refresh follow-up

- [x] Reproduce a stalled JSON body leaving the existing single-flight poller
  pending indefinitely after the shared API helper cleared its timeout at headers.
- [x] Retain the request deadline through JSON body completion. Preserve bounded
  GET retries and never replay mutating requests automatically.
- [x] Keep one satellite detail read every five seconds while Status is visible;
  refresh a due view on return, guard overlapping triggers and ignore late replies
  after navigation. Treat backward wall-clock changes as due rather than freezing.
- [x] Show failed refreshes explicitly while retaining the last received values.
- [x] Exercise body stalls, ordinary slow responses, visibility, teardown and
  recovery through the existing UI harness. This reproduces a possible freeze
  mechanism; it does not establish which request stalled in a user's old session.

## Remaining validation and deferred policy

- [x] Complete the authorized canary comparison with a continuously receiving
  local rover, optional probes, satellite polling and TCP-sample overhead counters.
- [ ] Separately qualify scan/stop event ordering, sustained RSSI deterioration,
  forced radio interruption, TLS under contention and the full 24-hour crash watch.
- [ ] Correlate saved probe/write/TCP timestamps with a new remote latency
  event before assigning a cause or tuning transport buffers.
- [ ] Revisit proposal 3 only with measured complete-epoch benefit and a tested
  distinction between intentional slot recovery and genuine connectivity faults.
  No stale-data dropping or intentional freshness reconnect is in this build.

## Authorized canary results, 2026-10-26

The app-only OTA response matched the uploaded image's SHA-256 and size, and
the running ELF identity matched the candidate. First-minute confirmation and
pre-update station-state verification passed; boot was stable after 120 seconds.
The previous release remains available for rollback. The 24-hour watch is ongoing.

- Baseline: 180 seconds, 180 consecutive GPS epochs, zero CRC errors or malformed
  chunks. Candidate: 600 seconds, 600 consecutive GPS epochs, 5,403 RTCM frames,
  zero CRC errors, bytes outside frames, partial tails, split chunks or epoch gaps.
- The protected upstream caster remained in one connection generation with no
  slow writes or hub drops. Receiver framing/UART errors, GNSS stalls and recovery
  counters remained zero. Receiver, upstream-caster and local-caster configurations
  matched the baseline.
- Across 712 snapshot calls observed by the end of the test, mean synchronous
  cost was 115.61 microseconds and maximum 630 microseconds. No call exceeded
  one millisecond. This is a measured window, not an upper bound under all loads.
- The first two candidate minutes used passive diagnostics; subsequent minutes
  added one-second probes. All 464 retained probes per target succeeded. Maximum
  router/internet/caster RTTs were 81/83/194 ms; RTT is not correction age.
- All 117 five-second satellite reads succeeded and returned changing satellite
  detail. CPU samples were 4–8%, versus 3–5% during the shorter baseline; the
  candidate also had weaker Wi-Fi and more polling. Internal free memory remained
  stable during the run. No causal CPU or radio comparison is claimed.
- Local rover observation-age p95 was 71.1 ms, versus 63.0 ms before the update;
  maxima were 140.2 and 329.0 ms respectively. These include host-clock and path
  effects and do not establish remote Onocoy latency.
- One automatic singleton roaming scan completed in 2,620 ms before the rover
  capture; the next routine search was scheduled no earlier than six hours.
  No forced scan or radio interruption was introduced during the capture.
- At completion, the probe recorder was stopped and the test rover disconnected;
  receiver and upstream remained healthy and streaming. No remote latency incident
  was captured, so attribution to scans, the receiver or the WAN remains unresolved.

Private raw captures and image checksums are stored with the local build,
outside tracked files. The current candidate also passed a fresh 87-check QEMU
smoke run and a 28-check NTRIP 2 upstream/maintenance/restart run.
