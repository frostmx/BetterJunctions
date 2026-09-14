# BetterJunctions

Server-side C++ mod for Satisfactory: fixes the vanilla gridlock of self-driving trucks at
crossroads. No assets, no client part (`RequiredOnRemote: false`). Install it where the
authority lives: on the dedicated server, or on the host of a listen game.

## What breaks in vanilla

The autopilot reserves junction "path blocks" ahead of the truck over its braking distance
(`UFGVehicleAutopilotComponent::CalculatePathReservationStopTarget`). That distance does not stop
at the truck driving in front. The second truck in a queue books blocks of the junction the first
one is still trying to enter, then stops behind the first one on vehicle avoidance. The first truck
cannot take its block sequence, because the second truck's reservation overlaps it, and the
second truck never lets go: it is waiting for a *vehicle*, not for a *block*, and the built-in
120-second deadlock timer (`mAutopilotWaitTimeToDeadlock`) only counts the latter. Every other
lane of the crossing then queues behind one of the two.

## What the mod does

Two SML hooks, both on the authority only. The pre-hook carries three rules; the last two were
added after the first build met two further kinds of jam on a dedicated server.

- **Prevention.** A pre-hook on `ReserveVehiclePathBlocks_Parallel`, the only place where blocks
  get booked, with two rules keyed on the nearest vehicle ahead (`CalculateVehicleAvoidanceTarget`):
  - *standing leader*: if a truck is standing ahead (slower than 1 m/s), every block that starts
    beyond it is dropped from the list to book (`CalculateTotalDistanceBetweenPathBlocks` from the
    current block). The same list also fills the set of "wanted" blocks the game uses to clean up
    surplus reservations, so blocks already held beyond the leader are released in the same tick.
    A moving leader is deliberately left alone;
  - *junction entry*: a truck does not book its way into a junction unless it can leave it. If a
    slow vehicle ahead (slower than 3 m/s) leaves less room than the distance to the junction's
    exit plus the truck's own length plus 2 m, the junction blocks and everything after them are
    dropped, and the truck waits at the entrance. The game only checks that the exit block can be
    booked, not that it is free of a queue, so a queue backing up through a junction left trucks
    standing inside it, holding its blocks, and the heads of two queues Deadlocked on blocks that
    overlapped those (measured 12.09.2026 on a dedicated server). A truck already inside a
    junction is never held back: it has to leave.

  - *junction priority*: a truck that has waited ten seconds on a junction claims it, and nobody
    else books that junction (or any segment overlapping its blocks) until the waiter is in.
    Booking needs every block of the sequence free at the moment of the attempt, and a crossing
    with steady traffic never has that moment for a truck that needs more of it than the passing
    ones do: measured 12.09.2026, a truck waited twelve minutes at a crossing with a different
    fuel truck inside it at every look, while the queue behind it backed up through two other
    junctions. Longest wait wins; a waiter whose junction is already claimed by a longer one
    stays out, so two trucks on crossing paths never hold each other back; a truck already
    inside a junction is never held back. The claim is rebuilt after every autopilot tick.

  The vehicle ahead is looked for 80 m along the path: a connector between two roads 32 m
  apart was entered with a 30 m lookahead because the standing truck past its exit was just
  out of sight.

  The first version clamped the lookahead in `CalculatePathReservationStopTarget` instead;
  measurement showed that this has no effect on booking: a truck 5 cm behind a standing truck
  still took a junction block two blocks ahead.
- **Cure.** A post-hook on `AFGVehicleSubsystem::TickVehicleAutopilot`: a truck that has been
  standing behind a standing truck for 5 seconds while holding reservations releases them
  (`ReleaseVehiclePathBlockReservations_Parallel`), once per standing episode. After the release
  the prevention hook stops it from booking past the leader again, and the short reservations up
  to the leader keep its place in the queue.

Both methods are protected; access goes through friend access transformers
(`Config/AccessTransformers.ini`), which is why every build of this mod recompiles all of
`FactoryGame`.

## Console commands

- `BJ.Dump` — every truck on autopilot: status, speed, time waited on a block, number of
  reservations, stop target, how long it has been standing behind a standing truck, and
  `HAS PRIORITY` when it holds a junction claim. A truck
  waiting on a block also shows the whole block sequence it is trying to book, and for each block
  what stands in the way: reservations by others, exclusives on overlapping blocks, vehicles inside.
- `BJ.Unstick` — release the reservations of every truck standing behind a standing truck right
  now, without the 5-second delay.
- `BJ.Blocks [filter]` — every reservation straight from the segments' block arrays (reflection
  cannot see them): exclusive ones with their owner, shared ones with the exclusive they belong
  to, and a GHOST mark on those nobody references any more.
- `BJ.Purge` — release every ghost reservation found by the same walk.
- `Log LogBetterJunctions Verbose` — log every trimmed booking list with the rule that trimmed it.

Command output goes to the console it was typed in and to the log.
