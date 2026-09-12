# Persistent Execution Fabric

**Persistent Execution Fabric is an open-source, vendor-neutral C++20 runtime for governing durable execution identity, resumable progress, checkpoints, replay, recovery, fencing, and generation-bound continuation across process and machine restarts.**

It answers one systems question:

> **Given an execution with durable identity, persisted progress, checkpoints, side effects, dependencies, bindings, and authority — what execution state may be resumed, replayed, reconstructed, fenced, abandoned, or committed after interruption, and which continuation is still authoritative?**

The defining principle is:

> **Persistence is not resumability.**

A checkpoint existing on disk does not mean execution may resume from it. A journal record existing does not mean the corresponding side effect committed. A process restarting does not regain its previous authority. A worker replacement does not inherit an old execution lease. A replayable action is not automatically safe to replay. A durable completion record is not an authoritative logical commit unless its generations and side-effect semantics are still valid.

UNKNOWN remains first-class. When interruption leaves completion ambiguous, the runtime represents that ambiguity rather than inventing certainty.

---

## Contents

- [Systems boundary](#systems-boundary)
- [Relationship to adjacent runtimes](#relationship-to-adjacent-runtimes)
- [Execution identity and incarnation](#execution-identity-and-incarnation)
- [Lifecycle](#lifecycle)
- [Progress semantics](#progress-semantics)
- [Continuation semantics](#continuation-semantics)
- [Checkpoint semantics](#checkpoint-semantics)
- [Side-effect taxonomy](#side-effect-taxonomy)
- [Ambiguous completion](#ambiguous-completion)
- [Replay](#replay)
- [Exactly-once logical progress](#exactly-once-logical-progress)
- [Worker reincarnation](#worker-reincarnation)
- [Coordinator restart](#coordinator-restart)
- [Leases](#leases)
- [Bindings and revalidation](#bindings-and-revalidation)
- [Policy](#policy)
- [Persistence and durability points](#persistence-and-durability-points)
- [Recovery planning](#recovery-planning)
- [Invariant auditor](#invariant-auditor)
- [Control plane](#control-plane)
- [Real OS-process proof](#real-os-process-proof)
- [REAL, SYNTHETIC, and UNSUPPORTED evidence](#real-synthetic-and-unsupported-evidence)
- [Optional hardware-bound continuation proof](#optional-hardware-bound-continuation-proof)
- [CLI](#cli)
- [Examples](#examples)
- [Benchmarks](#benchmarks)
- [Build](#build)
- [Test](#test)
- [Install and consume](#install-and-consume)
- [Known limitations](#known-limitations)
- [License](#license)

---

## Systems boundary

Persistent Execution Fabric owns the **persistent continuation contract**:

- which durable execution state exists;
- which durable point is valid;
- which continuation may resume;
- which replay is legal;
- which execution incarnation is current;
- which interruption outcome is ambiguous;
- which recovered continuation may become authoritative;
- which physical side-effect repetition is permitted, and which is refused.

It owns durable execution identity, persisted progress, continuation authority, resumability, replay safety, checkpoint lineage, recovery classification, restart fencing, ownership, leases, retry admission, exactly-once *logical* commit, and authoritative continuation.

It does **not** own, and does not attempt to own:

| Adjacent concern | Owned by |
|---|---|
| Agent semantics, model calls, tool meaning, memory semantics, autonomous reasoning | Agent Runtime |
| Which physical execution attempt has authority for a distributed job | Execution Fabric |
| Checkpoint production, persistence, migration, rollback at the broader state layer | Checkpoint Fabric |
| Durable checkpoint bytes, chunking, deduplication, integrity, retention, GC, replicas | Checkpoint Store |
| Generalized lineage and provenance | State Provenance |
| Placement, scheduling, resource arbitration | Compute Fabric / Resource Broker / schedulers |
| Failure classification and compensation semantics | Failure Fabric |

Persistent Execution Fabric is **not** a scheduler, workflow engine, batch system, generic database, consensus system, cluster manager, resource broker, checkpoint object store, storage engine, tool executor, model runtime, agent framework, container runtime, VM manager, process supervisor, or general event-sourcing framework.

It is the layer that decides whether a durable execution may continue, from where, and under whose authority — and that refuses to decide when the evidence does not support a decision.

---

## Relationship to adjacent runtimes

### Agent Runtime

Agent Runtime governs long-running autonomous-agent execution across model calls, tools, memory bindings, checkpoints, retries, budgets, interruption, recovery, and action authority. It answers *what may this agent do next*.

Persistent Execution Fabric sits **below or beside** Agent Runtime and owns the generic durable-execution substrate. Agent Runtime may use this fabric to persist and restore execution state. The fabric never inspects a prompt, a tool schema, a model route, a memory record, or an agent plan; it understands only execution identity, durable progress, generations, side effects, and authority.

The boundary is preserved sharply: **Agent Runtime is not collapsed into this repository, and this repository does not become an agent runtime.**

### Execution Fabric

Execution Fabric governs which *physical execution attempt* is authoritative for a distributed logical execution: dispatch, ownership, attempts, fencing, cancellation, and preemption across a fleet.

Persistent Execution Fabric governs the *durable continuation* of an execution across process, worker, coordinator, and machine restarts. Where Execution Fabric asks "which attempt owns this logical execution right now", Persistent Execution Fabric asks "which durable point may an execution legally continue from, and is that point still valid under current authority and side-effect semantics".

Both use strongly typed identities and monotonically advancing generations. They are different layers and neither subsumes the other.

---

## Execution identity and incarnation

Semantically different authority domains are **different C++ types**, not interchangeable integers or raw strings. Passing a WorkerId where an ExecutionId is required does not compile.

```
ExecutionId                    ExecutionGeneration
ExecutionIncarnationId         ExecutionIncarnationGeneration
CoordinatorEpoch               StoreId
WorkerId                       WorkerBootId                  SessionId
AttemptId                      AttemptGeneration
ContinuationId                 ContinuationGeneration
CheckpointId                   CheckpointGeneration
ProgressId                     ProgressGeneration
ActionId                       ActionGeneration
SideEffectId                   SideEffectGeneration
ReplayId                       ReplayGeneration
LeaseId                        LeaseGeneration
BindingId                      BindingGeneration
PolicyId                       PolicyGeneration
RecoveryId                     RecoveryGeneration
CommitId                       RequestId
EvidenceId                     EvidenceGeneration
AmbiguityId                    AmbiguityGeneration
```

**Persistent execution identity outlives process identity.** An ExecutionId survives every legal worker, coordinator, and machine restart. An ExecutionIncarnationId changes whenever a new process or worker assumes execution. A reused PID, address, worker slot, connection, filename, device ordinal, or integer never resurrects stale authority, because authority is checked against generations rather than against any process-local fact.

Derived identities (CommitId, SideEffectId, ReplayId, CheckpointId, ActionId, …) are **pure functions** of canonical state with per-domain salts, so replaying the same durable history reproduces the same identities and two domains can never collide on the same input tuple.

---

## Lifecycle

```
Created → Ready → Running ⇄ Checkpointing
                    │  ╲
                    │   Suspending → Suspended → Resuming
                    │
                    ├→ Draining → Completed
                    ├→ RecoveryRequired → Recovering → Resuming
                    ├→ Fenced
                    ├→ Failed
                    └→ Cancelled

Completed → Retired        Cancelled → Retired        Failed → RecoveryRequired (policy)
Fenced → RecoveryRequired  RecoveryRequired → Recovering
```

Legal transitions are a single explicit table in src/lifecycle.cpp. Everything else is refused with ILLEGAL_TRANSITION:

- Completed → Running fails.
- Retired → Ready fails.
- RecoveryRequired → Running fails: recovery must classify, and only then may execution resume.
- Fenced execution cannot continue under old authority.
- Failed may resume only when policy explicitly allows it *and* a valid continuation exists.
- A completed execution cannot commit a second logical completion.
- **Self-transitions are never legal.** Idempotent operations return success without changing state; they do not "transition" to where they already are.

---

## Progress semantics

The fabric distinguishes four different things that are often conflated:

1. **observed progress** — a worker says step 50 finished;
2. **persisted progress** — a record reached the journal;
3. **authoritative progress** — the execution aggregate promoted that record through a logical commit;
4. **committed logical progress** — the durable commit identity that may never be repeated.

The commit path is:

```
begin action                  (durable identity reserved, dispatch recorded)
  → perform work
  → record completion evidence / durable receipt
  → validate side-effect semantics
  → revalidate generations (epoch, incarnation, lease, policy, bindings)
  → persist authoritative progress + commit identity   (ONE journal record)
  → install the aggregate that promotes it
  → expose the next durable continuation point
```

**If persistence fails, progress does not become authoritative.** The in-memory record is installed only after its journal record is written. If authority changes before the commit, the commit is refused and progress does not become current.

A record that reached the journal but whose aggregate promotion was lost to a torn tail is reconciled on the next open: the detail records are the durable facts, so the aggregate is brought forward and the repair is itself journaled.

---

## Continuation semantics

A continuation is an explicit governed object, not an implication. It binds:

```
ExecutionId, ExecutionGeneration
ExecutionIncarnationId, ExecutionIncarnationGeneration
CheckpointId, CheckpointGeneration        (when applicable)
ProgressGeneration, ActionGeneration
PolicyId, PolicyGeneration
binding generations
CoordinatorEpoch
RecoveryId, RecoveryGeneration
resume ordinal
```

A continuation is classified as exactly one of:

| State | Meaning |
|---|---|
| VALID | every bound generation is current, the durable point is unambiguous, and the checkpoint (if any) can seed execution |
| STALE | execution generation, incarnation, or coordinator epoch moved |
| REVALIDATION_REQUIRED | a binding or policy generation moved and policy permits revalidation rather than refusal |
| AMBIGUOUS | an unresolved ambiguous action sits at or below the continuation point |
| UNSUPPORTED | the referenced durable point is something this build cannot interpret |
| INVALID | a hard inconsistency: missing execution, progress or action generation ahead of the frontier, an unresolvable policy change, or a broken checkpoint reference |

Validation is a pure function of durable state (validate_continuation_state). At most one continuation per execution may be VALID, and it must be the execution's current continuation; every other continuation is retired when a newer durable point is established.

A continuation whose progress generation is *behind* the frontier is still legal: it describes an earlier durable point from which execution may continue, provided policy allows it.

---

## Checkpoint semantics

Checkpoint existence is not resumability. A checkpoint binds:

```
ExecutionId, ExecutionGeneration, ExecutionIncarnationGeneration
ProgressGeneration, ActionGeneration
PolicyId, PolicyGeneration
binding generations
lineage: parent CheckpointId/Generation, lineage depth
creation authority: WorkerId, WorkerBootId, CoordinatorEpoch
effect boundary ordinal
content metadata: size + integrity digest
```

States: CREATED, VERIFIED, CURRENT, STALE, SUPERSEDED, CORRUPT, REVALIDATION_REQUIRED, UNSUPPORTED. Only VERIFIED or CURRENT may seed a continuation.

The fabric **does not store checkpoint bytes** — a checkpoint carries a size and a content digest, and the bytes are owned externally (by Checkpoint Store or by the caller). What the fabric verifies is the metadata it does own: generation consistency, lineage integrity, creation authority, and the effect boundary.

Lineage is deterministic. A checkpoint's parent must be the checkpoint the caller believed was current, and lineage depth must follow the parent exactly; a mismatch is refused with STALE_CHECKPOINT_GENERATION, and a missing parent is CORRUPT_STATE.

A newer checkpoint does **not** automatically invalidate an older one: when policy sets allow_older_checkpoints, the previous checkpoint is demoted to VERIFIED and remains usable; otherwise it becomes SUPERSEDED.

---

## Side-effect taxonomy

The runtime does not understand domain-specific tool semantics. It understands only how safe it is to re-drive an action whose physical outcome may be unknown. Callers must classify their actions; the fabric refuses to guess.

| Class | Automatic replay | Notes |
|---|---|---|
| PURE | permitted | recomputation leaves no external trace |
| IDEMPOTENT | permitted | the external contract is idempotent |
| REPEATABLE_WITH_KEY | permitted | a stable idempotency/request key is **required** before dispatch |
| COMPENSATABLE | refused by default | requires explicit compensation or an operator decision |
| NON_REPEATABLE | **forbidden** after ambiguous completion | |
| UNKNOWN | **forbidden** | |

Physical outcome and logical completion are separate fields on the action record. An action can be IN_FLIGHT, EFFECT_APPLIED, EFFECT_NOT_APPLIED, COMPLETED_UNACKNOWLEDGED, COMMITTED, FAILED, AMBIGUOUS, COMPENSATED, or ABANDONED.

---

## Ambiguous completion

The critical case the fabric exists for:

1. the execution dispatches an action;
2. the action may have applied an external side effect;
3. the worker dies before any durable acknowledgement;
4. the runtime cannot know whether the effect happened.

The fabric does not retry blindly. It returns AMBIGUOUS_COMPLETION, opens a durable ambiguity record, seals the action as AMBIGUOUS, and **blocks every progress commit above that action's ordinal** (blocked_above_ordinal).

Recovery policy then requires one of:

- a durable external receipt lookup (REPORT_SIDE_EFFECT);
- explicit system or operator reconciliation;
- a compensating action;
- explicit abandonment of the progress;
- explicit risk acceptance;
- restart from an earlier safe continuation.

An ambiguity is resolved by RESOLVE_AMBIGUITY as RESOLVED_APPLIED, RESOLVED_NOT_APPLIED, ACCEPTED, ABANDONED, or MANUAL_RESOLUTION_REQUIRED. Only the first two are evidence-backed; the third records deliberate risk acceptance; the fourth deliberately forfeits the progress.

A durable receipt that states the effect **applied** removes the ambiguity: the coordinator commits the logical progress from the receipt with no worker and no replay. A durable receipt that states the effect did **not** apply makes re-driving the same logical action a re-execution of work that never happened.

---

## Replay

Replay is explicit and generation-bound. A replay record binds the execution, the original action identity *and generation*, the replay generation, the side-effect classification, the original request identity, the progress and checkpoint generations, the policy generation, and the coordinator epoch.

Re-driving an action does **not** create a new logical action. The same ActionId advances to a new ActionGeneration; the durable action table is keyed by (ActionId, ActionGeneration), so replay history is never overwritten. A replay is admitted only when:

- the action holds a durable failure or a receipt stating no effect;
- policy permits replay for that class;
- the replay depth bound has not been reached;
- the coordinator epoch and every generation are current.

Replay after any authority change that invalidates the original assumptions is refused.

---

## Exactly-once logical progress

The fabric **does not claim exactly-once physical execution.** A physical action may execute zero times, once, or more than once depending on failure and retry behaviour.

What is enforced is **exactly-once authoritative logical progress where semantics permit it**:

- CommitId is derived deterministically from (ExecutionId, ActionId, ActionGeneration, ProgressGeneration), and a second commit record for the same identity is refused with DUPLICATE_COMMIT;
- duplicate completion messages return the *recorded* commit identity and progress generation with duplicate = true and change nothing;
- two completion messages racing for the same ActionId produce exactly one logical commit — the runtime is serialized, and the loser observes the winner's commit identity;
- a retry after durable completion returns the same authoritative result;
- the commit and everything it implies are written as **one journal record**, so a crash can never leave a commit without its progress or progress without the aggregate that promotes it.

---

## Worker reincarnation

A worker restart creates a fresh WorkerBootId, minted from the worker identity, a per-process counter, and the process creation marker — a reused operating-system process id cannot resurrect a previous boot.

The recovery flow is:

```
detect worker loss          (connection ends; the coordinator observes it)
  → fence old boot          (execution → FENCED, lease revoked, worker binding cleared)
  → classify in-flight state
  → identify durable continuation
  → admit replacement worker
  → validate replacement binding
  → advance execution incarnation
  → issue fresh continuation authority (new lease generation)
  → resume
```

Old worker traffic arriving later is refused with a specific code: STALE_EPOCH, STALE_LEASE, AUTHORITY_REVOKED, STALE_WORKER_BOOT, STALE_INCARNATION, or STALE_EXECUTION_GENERATION depending on exactly which generation moved.

---

## Coordinator restart

A coordinator restart advances CoordinatorEpoch. Persisted execution identities survive; process-local sessions do not.

On restart the coordinator:

1. loads the durable snapshot and replays the journal;
2. verifies integrity and refuses corruption;
3. reconciles records whose aggregate promotion was lost to a torn tail;
4. advances the epoch and journals the advance;
5. invalidates every process-local session;
6. revokes every lease from the previous epoch and clears the worker bindings it authorised;
7. classifies in-flight actions;
8. revalidates checkpoints and bindings;
9. marks continuations from the previous epoch STALE;
10. forces every execution that was doing work into RECOVERY_REQUIRED;
11. recovers continuations that are safe to recover;
12. refuses stale-epoch traffic;
13. audits invariants.

**RUNNING is process-local and is never revived from durable bytes.** A durable record saying an execution was running is evidence that it *was* running, not that it still is.

---

## Leases

Continuation authority is lease-bound. A lease binds:

```
ExecutionId, ExecutionGeneration
ExecutionIncarnationId, ExecutionIncarnationGeneration
WorkerId, WorkerBootId
CoordinatorEpoch
LeaseId, LeaseGeneration
PolicyGeneration
binding generations
```

A stale lease fails after a coordinator restart, a worker restart, an execution generation change, an incarnation change, a policy change, a binding change, an explicit revoke, or a recovery transition. Lease generations never restart, and **old leases are never restored after a restart.**

Leases are **authority-bound, not time-bound**. There is no wall-clock expiry, because a clock-dependent expiry would make recovery decisions non-deterministic. Liveness is handled explicitly through FENCE, session end, incarnation change, and policy change.

---

## Bindings and revalidation

An execution may depend on external bindings — memory, model, tool, resource, environment, filesystem, or hardware. The fabric does not own those domains. It stores typed generation references.

When a required binding generation moves:

- with StaleBindingBehavior::Refuse, the continuation is INVALID and execution cannot continue;
- with StaleBindingBehavior::RequireRevalidation, the continuation becomes REVALIDATION_REQUIRED, and an explicit resume carrying the observed bindings revalidates it.

Persisted state never silently keeps using stale external assumptions.

---

## Policy

Policy is durable state, not startup configuration. A continuation carries the policy generation it was derived under.

```
checkpoint_interval_actions   allow_older_checkpoints   max_checkpoints_per_execution
replay_allowance              max_replay_depth          allow_recompute
ambiguity_handling            ambiguity_blocks_later_progress
recovery_preference           allow_recovery_from_failed
stale_binding_behavior        policy_change_invalidates_continuations
durability                    max_action_history / replay / ambiguity / continuation bounds
```

Changing policy advances PolicyGeneration, revokes the authority granted under the old generation, and invalidates or requires revalidation of continuations that bound it — according to the policy's own rule.

---

## Persistence and durability points

Storage is **one directory** holding one atomically replaced snapshot and one append-only journal.

```
<store>/pef.journal     append-only, one file header, CRC-protected record frames
<store>/pef.snapshot    atomically replaced state image
```

Every journal record is a **full-record upsert**, so journal replay is a pure function of the record sequence and is idempotent.

The exact durability points:

| Point | Meaning |
|---|---|
| memory-visible only | in-process state that has not been journaled. **Never observable** by a caller: a record is installed only after its journal write succeeds. |
| persisted | appended to the journal inside the writer's buffer or the operating system's page cache. Recoverable only if the host does not lose that cache. Used by DurabilityMode::JournaledNotFlushed. |
| durable | appended **and flushed to stable storage** before the caller is told the transition succeeded. Used by DurabilityMode::DurableOnCommit, which is the default. |
| externally committed | a side effect applied in a system the fabric does not own; represented by a durable receipt, never assumed. |
| authoritative | promoted by a durable aggregate record, or by an explicit operator decision that is itself journaled. |

Audited ordering: **mutate-as-new-record → persist → install → acknowledge**. There is no window in which a reader observes state that a fresh coordinator could not recover, because persistence happens under the same lock as the mutation, and installation happens after it.

Torn writes: a record frame is magic, kind, flags, sequence, length, payload CRC, header CRC, payload, and a trailing frame CRC. On load, the first unreadable boundary is examined: if a valid record exists *after* it, the journal is **corrupt** and loading is refused; if nothing valid follows, it is a **torn tail**, the prefix is accepted, and the truncation is reported in the load report and by the CLI.

---

## Recovery planning

Classification is a pure function of durable state, current evidence, and policy. It consults no clock, no process table, and no thread timing. It yields exactly one of:

```
RESUME_FROM_CURRENT        RESUME_FROM_CHECKPOINT     REPLAY_SAFE_ACTION
REVALIDATE_BINDINGS        RECOMPUTE                  COMPENSATE
AMBIGUOUS_COMPLETION       MANUAL_RESOLUTION_REQUIRED UNSUPPORTED      TERMINAL
```

Decision order:

1. terminal lifecycle gives TERMINAL;
2. FAILED without explicit policy permission gives TERMINAL;
3. an engaged frontier block gives AMBIGUOUS_COMPLETION or MANUAL_RESOLUTION_REQUIRED;
4. a moved required binding generation gives REVALIDATE_BINDINGS, or UNSUPPORTED when policy refuses;
5. an open ambiguity gives AMBIGUOUS_COMPLETION or MANUAL_RESOLUTION_REQUIRED;
6. an unusable referenced checkpoint gives UNSUPPORTED;
7. an interrupted action: a durable applied receipt gives RESUME_FROM_CURRENT with a coordinator-side commit; a durable no-effect receipt or a not-applied resolution gives REPLAY_SAFE_ACTION; an automatically replay-safe class gives REPLAY_SAFE_ACTION; a compensatable class under compensation policy gives COMPENSATE; anything else gives AMBIGUOUS_COMPLETION;
8. a valid current checkpoint, or (when policy allows) the newest seedable checkpoint, gives RESUME_FROM_CURRENT or RESUME_FROM_CHECKPOINT;
9. otherwise the progress frontier itself is a legal continuation point, giving RESUME_FROM_CURRENT.

Recovery is conservative by construction: "best effort" never hides unresolved ambiguity, because the ambiguity block is durable state.

---

## Invariant auditor

Runtime::audit and pef_cli audit run the same pure function over durable state. It reports stable, machine-readable identifiers, including:

```
EXECUTION_ID_NIL                       EXECUTION_GENERATION_ZERO
INCARNATION_ID_MISMATCH                EXECUTION_WORKER_WITHOUT_LEASE
EXECUTION_REFERENCES_INACTIVE_LEASE    LEASE_STALE_EPOCH
TERMINAL_EXECUTION_HOLDS_LEASE         RETIRED_EXECUTION_ACTIVE_LEASE
ORPHAN_ACTIVE_LEASE                    STALE_LEASE_STILL_ACTIVE
CONTINUATION_GENERATION_AHEAD          CONTINUATION_PROGRESS_AHEAD
CONTINUATION_CHECKPOINT_UNUSABLE       MULTIPLE_VALID_CONTINUATIONS
VALID_CONTINUATION_NOT_CURRENT         ACTIVE_EXECUTION_WITHOUT_VALID_CONTINUATION
PROGRESS_GENERATION_NOT_CONTIGUOUS     PROGRESS_ORDINAL_DUPLICATE
COMMITTED_ACTION_COUNT_MISMATCH        FRONTIER_NOT_LATEST_COMMIT
PROGRESS_BEYOND_AMBIGUOUS_ACTION       DUPLICATE_LOGICAL_COMMIT
COMMITTED_ACTION_WITHOUT_COMMIT        ACTION_BEYOND_FRONTIER
CHECKPOINT_LINEAGE_BREAK               CHECKPOINT_LINEAGE_DEPTH_MISMATCH
CHECKPOINT_HASH_ZERO                   VALID_CONTINUATION_UNSEEDABLE_CHECKPOINT
ACTION_MISSING_REQUEST_KEY             REPLAY_TARGET_MISSING
OPEN_AMBIGUITY_NOT_REFERENCED          AMBIGUITY_ACTION_MISSING
POLICY_INVALID                         STORE_SCHEMA_UNEXPECTED
```

The final audit on the release proofs reports **zero violations**.

---

## Control plane

Three executables:

- pef_coordinator — owns durable execution authority;
- pef_worker — executes bound work; holds no durable authority of its own;
- pef_cli — inspects and drives execution and recovery.

### Frame format

```
offset  size  field
0       4     magic 'P','E','F','P'
4       2     protocol version
6       2     message type (reply bit 0x8000)
8       8     request identity
16      4     payload length   (bounded: 1 MiB, refused before allocation)
20      4     flags            (reserved, must be zero)
24      4     header CRC       (low 32 bits of CRC-64 over bytes 0..23)
28      n     payload
28+n    8     frame CRC        (CRC-64 over header + payload)
```

### Operations

HELLO, PING, SHUTDOWN, CREATE_EXECUTION, BIND_WORKER, START, BEGIN_ACTION, COMPLETE_ACTION, FAIL_ACTION, REPORT_SIDE_EFFECT, CHECKPOINT, SUSPEND_BEGIN, SUSPEND_COMMIT, RESUME, FENCE, CANCEL, RETIRE, DRAIN, COMPLETE_EXECUTION, ADVANCE_GENERATION, UPDATE_POLICY, CLASSIFY, RECOVER, RESOLVE_AMBIGUITY, VALIDATE_CONTINUATION, QUERY, LIST_EXECUTIONS, SNAPSHOT, AUDIT.

Every operation reply begins with a status, so a refusal is never mistaken for a result with defaulted fields.

### Defences

Version negotiation; bounded frames; request identity; per-client request identity minted from a per-instance nonce; a bounded reply cache that returns a repeated request identity verbatim without re-executing it; deterministic refusal codes; defensive decoding (magic, version, type, flags, header CRC, declared length, frame CRC, trailing bytes); unknown message types refused; reply frames refused as requests; reply identity matched to the request.

The control plane defends against malformed, truncated, oversized, reordered, duplicated, and replayed input, and against stale epochs, stale worker boots, stale leases, stale execution generations, stale continuations, stale checkpoint generations, stale policy generations, stale binding generations, duplicate commits, connection loss, coordinator restart, and worker restart. Each has its own refusal code.

**No telemetry is transmitted.** The coordinator opens one listening socket on the configured address and does nothing else on the network.

---

## Real OS-process proof

tests/test_multiprocess.cpp drives **real processes** over **real loopback TCP** against a **real durable store**. Threads are never substituted for processes:

1. start pef_coordinator;
2. start Worker A;
3. create an execution;
4. bind it to Worker A;
5. advance three authoritative progress steps;
6. register and validate a checkpoint;
7. kill Worker A;
8. prove the old boot is fenced and its lease revoked;
9. admit Worker A′ under a new boot and a new incarnation;
10. recover from the durable continuation;
11. resume;
12. prove progress continues exactly once logically (5 commits for 5 actions, no duplicates);
13. exercise a NON_REPEATABLE side-effect case;
14. record a durable side-effect receipt;
15. kill the worker before acknowledgement or commit;
16. prove the runtime reports ambiguity rather than retrying;
17. prove no unauthorized progress commits;
18. resolve the ambiguity as not-applied and re-drive the same logical action under a new generation;
19. restart the coordinator;
20. prove the epoch advances;
21. prove pre-restart leases, sessions, and tokens are rejected;
22. recover the execution identity;
23. revalidate the continuation;
24. resume;
25. audit invariants to zero violations;
26. shut down cleanly;
27. prove zero leaked child processes.

Additional multiprocess cases cover the **process-death matrix** (death while idle, holding after a checkpoint, in flight with PURE, IDEMPOTENT, NON_REPEATABLE, or UNKNOWN, after an applied effect, and after a no-effect receipt), shutdown while work is pending, and coordinator restart under load.

---

## REAL, SYNTHETIC, and UNSUPPORTED evidence

Every durable record that carries an observation labels where it came from; nothing is inferred.

| Label | What it means here |
|---|---|
| **REAL** | observed in this environment: Windows process behaviour, process kill and restart, TCP loopback, durable file persistence, journal and snapshot recovery, coordinator restart, epoch advance, and RTX 5090 device execution with exact CPU parity |
| **SYNTHETIC** | modelled or injected: remote-machine restart, multi-node migration, external side-effect systems, alternative hardware backends, distributed storage, and the external receipts used in the ambiguity proofs, which stand in for a system the fabric does not own |
| **UNSUPPORTED** | the environment cannot produce the observation: consensus-backed coordinator HA, replicated persistent stores, cross-machine live process migration, and exactly-once physical side effects |

The evidence kind is carried explicitly on receipts and resolutions, and rendered by pef_cli and explain_execution.

---

## Optional hardware-bound continuation proof

cuda/pef_cuda_continuation.cpp builds when -DPEF_ENABLE_CUDA=ON. The CUDA **driver** API is loaded dynamically at run time and the kernel is JIT-compiled from embedded PTX, so enabling it requires **no CUDA toolkit and no CUDA build support**.

The proof:

1. discovers the real device, its UUID, compute capability, and memory;
2. derives a HARDWARE binding identity from the device UUID;
3. creates and starts a durable execution under hardware binding generation 1;
4. allocates a device buffer, copies host data in, launches a real kernel, copies the result out, and verifies **exact CPU parity**;
5. registers a checkpoint whose content digest is the verified output digest;
6. commits the logical progress;
7. fences the worker and releases the device buffers;
8. presents hardware binding generation 2, and proves the continuation bound to generation 1 no longer validates and that a resume presenting the stale generation is refused with STALE_BINDING_GENERATION;
9. revalidates and resumes under generation 2;
10. safely recomputes the PURE hardware action, verifies parity again, and confirms the recomputed digest matches the checkpoint digest;
11. commits exactly once more, audits to zero violations, and releases every device resource.

**The fabric never claims that CUDA execution state was checkpointed.** What is proven is durable execution semantics *around* a hardware-bound action: stale device binding generations cannot resume, and a PURE hardware action can be safely recomputed. The proof states this explicitly in its own output.

On a machine with no CUDA driver or device, the proof reports UNSUPPORTED and exits successfully **without claiming hardware evidence**.

Verified run in this environment:

```
REAL device ordinal=0 name=NVIDIA GeForce RTX 5090
REAL device uuid=d1056bb6-4fec-2891-83f2-3a24fc70276b compute=12.0 memory_bytes=34162016256
REAL kernel executed on NVIDIA GeForce RTX 5090; CPU parity: exact
STALE DEVICE BINDING: STALE_BINDING_GENERATION: a required binding generation changed
RESUMED under hardware binding generation 2; continuation=VALID
REAL recomputed digest matches the checkpoint digest: yes
AUDIT violations=0
```

---

## CLI

```
pef_cli [--host H] [--port N] <command> [arguments]

version                                   print build identity
ping                                      liveness, store identity, and current epoch
execution create [--interval N] [--allow-older-checkpoints] [--durability durable|journaled]
execution list | show <execution>
execution start <execution> [--binding d:id:gen[:label]]
execution suspend | resume | fence | cancel | retire | drain | complete <execution>
execution advance-generation <execution> [--reason R]
execution policy <execution> [--max-replay-depth N] [--durability MODE]
action show <execution> [--ordinal N]
checkpoint create <execution> --hash H [--size N] [--boundary N]
checkpoint list | explain <execution>
progress show <execution>
continuation show | validate <execution>
recovery explain | apply <execution>
ambiguity list <execution>
ambiguity resolve <execution> --ambiguity ID --as applied|not-applied|accepted|abandoned|manual
lease show <execution>
snapshot [--truncate]
audit [--store DIR]        online audit, or a non-mutating offline store audit
verify [--store DIR]       same as audit, exit code 3 on violations
demo                       a self-contained end-to-end scenario
```

Exit codes: 0 success, 1 refusal or failure, 2 usage, 3 audit violations.

pef_cli explains, in deterministic text with no clock or pointer values:

- which executions exist and which generation is current;
- which worker and boot own an execution, and which lease is current;
- what progress is authoritative and what the last logical commit is;
- what checkpoint is current, what is resumable, and **why a checkpoint is not resumable**;
- what action is in flight, whether replay is safe, and why automatic retry is allowed or refused;
- whether completion is ambiguous and what would resolve it;
- which binding became stale and which generation moved;
- why recovery chose a continuation, and why a continuation was rejected;
- which durable request and commit identity applies;
- what changed after a restart.

---

## Examples

Every example performs real runtime operations and prints what it actually observed. None hardcodes expected output.

| Example | Demonstrates |
|---|---|
| pef_example_create_persist | creating an execution and committing durable progress |
| pef_example_checkpoint_resume | checkpointing, restarting the coordinator against the same store, and resuming |
| pef_example_worker_reincarnation | worker loss, fencing, a fresh boot, and continued progress |
| pef_example_stale_continuation | a continuation that stops being valid, and the revalidation that fixes it |
| pef_example_replay_safe | replay safety across every side-effect class |
| pef_example_ambiguous_nonrepeatable | ambiguity, evidence-based resolution, and re-driving the same logical action |
| pef_example_coordinator_restart | epoch advance, stale token refusal, and what survives |
| pef_example_installed_consumer | the shape an independent downstream consumer uses |

Every example is registered with CTest and runs in the release matrix.

---

## Benchmarks

pef_benchmarks measures **completed work**, not submission latency: every measurement is taken after the coordinator acknowledged a durable commit. Measured on this machine (AMD Ryzen 7 9800X3D, Windows, Release, MSVC 19.44):

| Measurement | n | per operation | Notes |
|---|---|---|---|
| execution create | 100,000 | ~2 µs | journaled mode |
| progress commit | 100,000 | ~16 µs | journaled mode, flat from 1,000 to 100,000 |
| execution query | 1,000 | ~13 µs | independent of history length |
| continuation validate | 1,000 | ~13 µs | |
| recovery classify | 1,000 | ~14 µs | |
| snapshot and compaction | 20,000 actions | ~54 ms | |
| snapshot load | 20,000 actions | ~43 ms | |
| invariant audit | 20,000 actions | ~9 ms | |
| **durable commit** | 2,000 | **~2.6 ms** | DurableOnCommit: a flush to stable storage before acknowledgement |

Durable commits are two orders of magnitude more expensive than journaled commits. That is the honest cost of the durability contract, and it is why DurabilityMode is an explicit policy field rather than a hidden default. Choose DurableOnCommit when an acknowledged transition must survive a host power loss; choose JournaledNotFlushed when the host's write-back cache is trusted and throughput matters, and take explicit barriers.

Hot paths are indexed: action lookup, commit counting, checkpoint listing, and replay listing are per-execution indexes maintained in O(1) amortized, not scans. The scale suite asserts that shape, and the correctness of the numbers is checked by the audit that runs in the same case.

---

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. No database, no service, no CUDA toolkit, no third-party dependency. Windows/MSVC and POSIX are both implemented; the Windows path is the one exercised by the proofs in this environment.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

| Option | Default | Effect |
|---|---|---|
| PEF_BUILD_TOOLS | ON | build pef_coordinator, pef_worker, pef_cli |
| PEF_BUILD_TESTS | ON | build the test suites and register them with CTest |
| PEF_BUILD_EXAMPLES | ON | build the examples |
| PEF_BUILD_BENCHMARKS | ON | build pef_benchmarks |
| PEF_WARNINGS_AS_ERRORS | ON | /W4 /WX /permissive- on MSVC; -Wall -Wextra -Wpedantic -Wshadow -Werror elsewhere |
| PEF_ENABLE_ASAN | OFF | build with AddressSanitizer |
| PEF_ENABLE_CUDA | OFF | build the optional hardware-bound continuation proof |

Debug and Release are both supported and both are clean at /W4 /WX with zero first-party warnings.

---

## Test

```
ctest --test-dir build --output-on-failure
```

| Suite | Covers |
|---|---|
| pef_tests_core | identities, lifecycle, codec determinism and rejection, side-effect taxonomy, policy, recovery classification |
| pef_tests_runtime | creation, binding, progress, duplication, checkpoints, suspension, reincarnation, restart, ambiguity, receipts, policy invalidation, lifecycle refusals |
| pef_tests_adversarial | torn tail, mid-journal corruption, corrupt and truncated snapshot, stale authority in every domain, duplicate and reordered completion, lineage breaks, superseded checkpoints, malformed and oversized frames, hostile persisted payloads |
| pef_tests_concurrency | racing completions, concurrent distinct commits, checkpoint and progress races, fence and completion races, snapshot under mutation, repeated recovery, concurrent creation, shutdown races |
| pef_tests_property | randomized lifecycle, failure and recovery, persistence round trips, byte-for-byte state round trips, and decision determinism with recorded seeds |
| pef_tests_multiprocess | the real OS-process proof, the process-death matrix, and shutdown with pending work |
| pef_tests_scale | 10, 100, 1,000, 10,000, and 100,000 actions; audit at scale; snapshot and load round trips; honest durable-commit cost |
| pef_example_* | every example executable |
| pef_cuda_continuation | the hardware-bound proof, when enabled |

Cases are individually addressable, so a hang localizes to an exact case and an exact phase:

```
build/pef_tests_multiprocess --list
build/pef_tests_multiprocess --case multiprocess.durable_lifecycle_proof
```

Every case prints a flushed BEGIN, flushed lifecycle phase markers (SETUP, CREATE, BIND, START, ACTION, CHECKPOINT, PERSIST, KILL, FENCE, RECOVER, RESUME, REPLAY, AMBIGUITY, COMMIT, RESTART, VERIFY, SHUTDOWN), and a flushed PASS or FAIL. No test uses a sleep as a substitute for correctness; the only bounded waits are for process readiness and state observation, and a timeout fails loudly with the observed output.

---

## Install and consume

```
cmake --install build --prefix /some/prefix
```

The install provides headers under include/pef, the library, the three executables, the license, and a CMake package:

```
find_package(PersistentExecutionFabric CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE PersistentExecutionFabric::pef)
```

The exported target is namespaced, the include layout is the installed one, and a downstream consumer outside the source tree builds and runs against the installed prefix. That path is validated as part of the release rather than asserted in prose. See examples/ex_installed_consumer.cpp.

---

## Known limitations

These are scope statements, not defects.

- **No consensus.** One coordinator per durable store. There is no replicated coordinator and no automatic distributed failover.
- **No replicated persistent store.** The journal and snapshot live on one filesystem. Durability is bounded by that filesystem's guarantees.
- **No cross-machine live process migration.** Execution *semantics* survive a machine restart; a running process does not move.
- **Exactly-once logical commit only.** Physical side effects may occur zero, one, or more times. The fabric never claims otherwise.
- **No generic external side-effect reconciliation protocol.** Receipts and resolutions are supplied by the caller or an operator; the fabric records and validates them but does not go and ask an external system.
- **Checkpoint bytes are externally owned.** The fabric stores and validates a checkpoint's description and integrity digest, not its bytes.
- **No built-in authentication.** The control plane is a loopback-oriented, unauthenticated protocol; a deployment that exposes it must provide its own transport security.
- **Leases are authority-bound, not time-bound.** There is no wall-clock expiry, because that would make recovery decisions non-deterministic.
- **Hardware-bound continuations require revalidation.** A device binding generation that moved invalidates continuations derived from it.
- **The auditor is a diagnostic, not a hot path.** Its cost is linear in the number of executions times their records; it is designed to be run at rest, at shutdown, and from the CLI.
- **Journal load reads the store into memory.** Snapshot plus compaction bounds the journal; very large single journals are limited by the configured load bound.
- **The POSIX persistence and socket path is implemented but is exercised only on Windows in this environment.**

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
