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
    overlapped those. A truck already inside a
    junction is never held back: it has to leave.

  - *junction priority*: a truck that has waited ten seconds on a junction claims it, and nobody
    else books that junction (or any segment overlapping its blocks) until the waiter is in.
    Booking needs every block of the sequence free at the moment of the attempt, and a crossing
    with steady traffic never has that moment for a truck that needs more of it than the passing
    ones do, a truck waited twelve minutes at a crossing with a different
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

## Rerouting (experimental, off by default)

The game plans a truck's way to its next station once, with A* over the road network and a static
cost per segment, and keeps it until the station is reached. Nothing in that cost knows about a
queue. With `BJ.Reroute.Auto 1` the mod replans the rest of the current leg every two seconds and
switches a truck to another road when that pays off:

- *jam ahead*: a vehicle that has stood on a segment for 10 s or more adds 150 m to that segment's
  cost. A truck switches when the new way is cheaper by 20% and by 50 m at least, and is not
  rerouted again for 30 s. Each truck sent onto a road makes it a little dearer for the next one
  in the same pass, so a column does not follow blindly;
- *waiting at a junction*: a truck that has waited 5 s for its junction blocks may leave by another
  exit, but only if every block of that way through the junction is free this very moment and the
  detour is no longer than 6 m for every second waited (20 m at least). Measured with rerouting
  off (18.09.2026, 51 trucks, 376 waits): a truck that has waited t seconds waits about 0.6 t
  more, so at ~10 m/s that is the break-even detour. A flat allowance sent trucks that had waited
  6 s on 250 m detours.

Only the tail of the route is replaced, from the end of the last segment the truck holds a
reservation on: nothing booked is ever given up, so a truck inside a junction or braking into one
keeps its way through. The station list is not touched.

Recon on the author's save: for a 100 m queue a detour no longer than 1.5x exists on about half
of the road the trucks drive; a quarter of the legs have none. On a calm save an A/B run of 15
minutes each gave 295 arrivals at stations with rerouting on against 294 with it off: no harm,
and no gain where there are no jams.

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
- `BJ.Autopilot off|on [filter]` — switch autopilot for every truck at once; the filter is a
  substring of the vehicle name. `off` also drops every path block reservation, so `off` then
  `on` is the crudest unstick there is. `on` skips trucks whose route is too short, and every
  truck books its path anew in the same tick, so busy crossings take a moment to sort out.
- `BJ.Reroute [filter] [apply] [avoid]` — for every truck (or those whose name contains the
  filter): the rest of its leg against the best way around standing vehicles, with the verdict.
  `apply` switches the trucks for which it pays off; `avoid` is a test aid that prices the next
  three segments of each truck's way as jammed.
- `BJ.Reroute.Auto 0|1` — rerouting on its own, see above. Off by default.
- `Log LogBetterJunctions Verbose` — log every trimmed booking list with the rule that trimmed it.

Command output goes to the console it was typed in and to the log.

## Chat command

`/autopilot off|on [filter]` does the same as `BJ.Autopilot` from the in-game chat of any
player. SML intercepts a chat line starting with `/` on the client and sends it to the server
through its own remote call object, so this works from a client of a dedicated server, and the
client needs only SML, not this mod. The one-line summary comes back in the chat; the per-truck
lines go to the server log only.
