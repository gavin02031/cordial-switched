# Event waiting on Horizon: grounded constraints and design direction

Status: constraints verified against source (2026-11-18). eventfd and
timerfd are working Morrow-virtual objects (timerfd evaluates
expirations lazily from the clock - no background thread), and
epoll_create/create1/ctl/wait/close work over those virtual objects:
same code on host and Horizon, readiness recomputed live at every wake,
blocking waits on a shared change generation with slices capped at the
soonest timer expiry (time-driven readiness raises no signal). ALooper
waits over Morrow-virtual fds through the same readiness path on every
platform; host-fd sets still use host poll on host builds and stay
refused on Horizon, and a mixed set refuses rather than half-waits.
The zero-fd infinite-timeout guard stands (this ELF imports no
ALooper_wake). Every event-waiting import the audited ELF needs has an
implementation; pipe/socketpair/accept4 stay fail-closed.

Known partial corners, by policy refused rather than faked:
- TFD_TIMER_CANCEL_ON_SET is not implemented; settime rejects it with
  EINVAL instead of accepting and ignoring it.
- timerfd EPOLLET follows the Linux edge source (unread count going
  zero to positive), not one event per firing; extra firings while a
  count is unread do not re-report.
- timerfd intervals are honored at nanosecond resolution with no
  minimum-period floor.
All of the above is host-tested only; no console run has observed any
of it yet, and host timing windows say nothing about console timing.

Documented epoll subset boundaries (all refusals are deliberate):
watches accept only Morrow-virtual descriptors (others fail EPERM, the
Linux answer for fds with no poll method); the accepted event mask is
EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP/EPOLLONESHOT/EPOLLET with genuine
edge semantics - per-object transition counters arbitrate edges that
fall entirely between two scans (drained and refilled), and MOD re-arms
an edge exactly as Linux re-registration does; epoll_wait caps maxevents
at 64. Descriptor fds embed a reuse generation, so a stale fd or a watch
on a closed object can never alias a new object that reused the slot
(watches on closed objects go silent, matching Linux file-description
teardown). Bounded waits budget from call start; unrelated change
notifications cannot extend them.

## Verified constraints

1. libnx provides a global `poll()`
   (libnx `nx/source/runtime/devices/socket.c:473`) and `select()`
   (same file, :376, implemented via poll). Its poll translates every
   fd through `_socketGetFd`, which fails for anything that is not a
   registered BSD socket, and the call then returns -1. Non-socket fds
   (pipes, files, any future virtual eventfd) therefore cannot be
   multiplexed by host poll on Horizon. Source: switchbrew/libnx master,
   file read directly.

2. The devkitPro newlib fork (branch `devkitPro`) carries no poll,
   select, epoll, eventfd, or timerfd implementation in
   `libgloss/libsysbase` or `newlib/libc/posix`; newlib's poll/select
   headers serve rtems/cygwin targets only. So nothing underneath
   Morrow supplies these facilities on Switch. Source: repository tree
   listing of devkitPro/newlib @ devkitPro, 2026-11-18.

3. Morrow's ALooper (`switch/source/android_looper_compat.c`) waited
   with host poll() over guest-registered fds. On Switch every such fd
   would be non-socket today: constraint 1 turns that into a -1 on every
   call, an eternal ALOOPER_POLL_ERROR storm (wedge by spin). And
   `ALooper_wake` is not bound, so `ALooper_pollOnce(-1)` with zero
   registered fds could never wake on any platform (wedge by sleep).
   Both shapes now refuse with a journaled diagnostic
   (`looper_wait refused reason=...`).

## Implication for the eventfd/timerfd/epoll queue item

The Android main loop the guest will drive (ALooper over an eventfd,
the classic Looper wake pattern) cannot be backed by host poll on
Horizon. The design that fits the verified constraints:

- Guest-visible fds become Morrow-virtual objects, each exposing a
  readiness predicate and (for blocking waits) a Horizon waitable:
  svcWaitSynchronization can wait on kernel handles (events, timers),
  which covers eventfd (manual-reset event) and timerfd (one-shot or
  periodic timer) honestly. The eventfd object landed in-process
  (pthread conditions over libgloss, which sits on Horizon condvars);
  lifting it onto a kernel handle is needed only when epoll must wait
  on it, and is a refinement of the same object, not a redesign.
- epoll_ctl/epoll_wait then multiplex over those handles with a single
  svcWaitSynchronization call, plus a self-wake handle so epoll_ctl from
  another thread can interrupt a wait in progress (the role epoll's
  own wake fd plays on Linux).
- ALooper migrates from host poll to the same primitive, so guest fds
  registered with ALooper_addFd work when they are Morrow-virtual
  objects; fds that are neither virtual objects nor BSD sockets stay
  refused and journaled.

INFERRED: Roblox's render/input loop will register a wakeable fd and
poll with a frame-budget timeout; the spin-wedge shape is exactly what
an unguarded port hits first. This is a hypothesis about guest
behavior, not an observed trace.

## What this does NOT settle

- Whether Horizon handle-wait latency meets frame budgets is unmeasured
  until the first console run with real constructors and a driven loop.
- Socket fds (once the guest's networking resolves somewhere) are the
  one fd class libnx poll can already multiplex; mixing them with
  virtual objects in one epoll instance needs an internal merge
  strategy that is not designed yet.
