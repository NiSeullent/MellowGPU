# MellowGPU shared device lifecycle

`Drivers/Core/DeviceLifecycle.hpp` uses namespace `MellowGPUCore` and is original C++17 policy code for the shared
invariants in [the specification](MELLOWGPU-SPEC.md). It allocates no heap memory,
uses no OS locks, invokes no callbacks, sleeps nowhere and performs no MMIO or
native calls. A native adapter must externally serialize **every** access,
including snapshots, and retain the policy object and native device owner until
all leases and native quiescence work have ended and the transition is complete.
Copy/assignment of the policy
object is disabled. Its destructor does not cancel, retire or free native work.

## Identity and typed ownership

One policy object belongs to a nonzero device-instance identity and nonzero
generation. The native owner assigns an instance identity that is never reused
while an older token can exist; constructing another policy object is not a way
to reset token identity. Reset generations strictly increase and never wrap.
Tokens contain device, generation, slot, monotonic serial and lease kind. The
five kinds are native operation, persistent pin, submission, publication and
display. There are 64 fixed slots. A reused slot receives a fresh serial; stale,
foreign, double-released and type-forged tokens cannot release its new owner.

The serial budgets default to `UINT64_MAX`. A trusted adapter may impose smaller
lifetime token budgets, for example for a bounded native handle namespace.
Exhaustion refuses new tokens without wrapping or resetting counters. Release of
existing tokens remains possible. Slot exhaustion and counter exhaustion are
distinct, explicit errors. Quiescence uses an independent serial budget.

`acquire(kind, &token)` records a device-owned native obligation. A request/client
may retain a token copy, but ending the request or closing the client does not
release that obligation. The native owner retains a copy until its actual work
and references have retired. There is no client-close cleanup shortcut.

`release(token, Unknown)` retains the complete obligation and marks uncertainty.
`release(token, KnownRetired)` is the native adapter's explicit assertion that
this particular operation/reference has retired. It must follow a real native
fence, cancellation-before-submission or other proven native lifetime boundary.
An application-supplied boolean is not authorization to call it. This API checks
identity and state; it cannot establish physical retirement itself.

## Stop, reset, loss and external native callbacks

`requestStop`, `requestReset` and `markDeviceLost` close admission before anything
can detach. They never erase live slots. Reset/loss also marks non-pin native
obligations uncertain. A pin remains present even though it has no GPU execution
of its own. A timeout does not change generation or make any resource reusable.
Stop dominates reset: reset cannot reopen a device already stopping.

The adapter follows this sequence:

1. Under its external synchronization, close admission and retain the native
   owner. Drain native operations and publication calls. Keep pins, submissions
   and display references owned while their flight is unknown.
2. Once native-operation and publication leases are zero, call
   `prepareQuiescence(&plan)`. Pins and submission/display leases may remain:
   GPU reset may be exactly what is needed to stop their DMA. The active plan
   represents the external native boundary and blocks another plan.
3. Release the adapter lock, perform the actual native quiescence operation and
   allow native callbacks. Keep the native owner retained throughout this step.
4. Reacquire synchronization and call `finishQuiescence` with the same plan and
   the actual native outcome. `Unknown` keeps the plan active. It does not permit
   retrying another operation, detaching or reopening the device.
5. `KnownQuiescent` ends that plan and records the adapter's native barrier only.
   The device enters `QuiescentRetained`; no slot is erased, no generation changes
   and admission stays closed. The barrier itself is a supplied native-adapter
   observation, not hardware evidence produced by the core.
6. Retire/cancel the old submissions and display ownership, and release pins
   only after actual native cleanup. Keep each existing lease and the device
   owner retained through its cleanup callback. Native reset does not imply that
   an application's resource reference has disappeared.
7. Call `completeTransition`. It requires the current acknowledged barrier and
   **all** leases drained. Only then may it finish a stop or install a strictly
   newer reset generation. A completed stop is terminal for this policy object.

`KnownNotQuiescent` is a separate, authoritative native-control failure: the
adapter guarantees that the attempted control call has finished and **no async
control operation from that attempt remains in flight**. It releases only the
plan, returns `NativeAttemptFailed`, leaves the barrier false, and retains all
pins, GPU/display obligations, closed admission and the same generation. A fresh
adapter-controlled attempt may then use the next plan serial, bounded by its
lifetime budget. It cannot substitute for `KnownQuiescent` or release memory.
An actual unknown control result must stay `Unknown`; it cannot be relabeled a
completed failure merely to permit another reset.

Requests and native callbacks may change intent while the external lock is
released. Stop escalation during a reset plan, or **any** device-loss notification
during a plan, supersedes it. Finishing that old boundary returns `Superseded`
without reopening or advancing generation. The owner stays retained and a fresh
quiescence plan is required. A loss after an acknowledged barrier, or escalation
from reset to stop, invalidates that barrier too. Repeating the same stop intent
is idempotent. Invalid and stale plans cannot finish another operation; a plan
cannot complete twice. The core never holds an adapter lock or calls a native
callback itself.

A native adapter must resolve self-worker termination using its supported
deferred continuation and PM acknowledgement mechanisms. This policy core does
not implement IOKit termination, module-unload safety, Darwin PM, vendor reset,
IRQ drain, DMA unmap or display replacement. A `Success` status from requesting
stop means the intent was accepted. Acknowledging native quiescence alone is
also insufficient. Only `completeTransition` and `Stopped` phase allow the
adapter to proceed to its separately implemented detach boundary.

## Verification scope

Run `python3 Tools/run-gpu-core-tests.py --sanitize --out build/gpu-core-tests`.
The actual header bodies are exercised for native work outliving its client,
all five owner kinds, timeout/device loss, foreign/stale/forged identities,
slot reuse/double release, fixed capacity, finite serial exhaustion, generation
overflow, GPU-hang recovery with retained pins, separate final drain and
callback-driven completion-plan supersession, plus known-failed native attempts
and bounded retained retry. Address/undefined
sanitizers are optional compiler support; the report records whether they ran.

The native outcomes in these tests are declared inputs to the policy. No native
adapter, GPU, Metal API or WindowServer executes. A policy pass cannot create
hardware evidence, qualify a supported profile or certify native quiescence.
