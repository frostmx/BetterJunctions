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

Measured on 12.09.2026: 29 trucks, four lanes, one reservation
(`Build_VehiclePath_Universal_C_2147435657`, block #1) held by the truck standing second in line.
Releasing that reservation (toggle its autopilot off and on) drained three lanes within three
seconds and the fourth as soon as the first one emptied.

## What the mod does

Two SML hooks, both on the authority only:

- **Prevention.** A pre-hook on `ReserveVehiclePathBlocks_Parallel`, the only place where blocks
  get booked. If a truck is standing ahead (a vehicle-avoidance target slower than 1 m/s), every
  block that starts beyond it is dropped from the list to book
  (`CalculateTotalDistanceBetweenPathBlocks` from the current block). The same list also fills
  the set of "wanted" blocks the game uses to clean up surplus reservations, so blocks already
  held beyond the leader are released in the same tick. The first version clamped the lookahead
  in `CalculatePathReservationStopTarget` instead; measurement showed that this has no effect on
  booking: a truck 5 cm behind a standing truck still took a junction block two blocks ahead. A
  moving leader is deliberately left alone.
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
  reservations, stop target, how long it has been standing behind a standing truck.
- `BJ.Unstick` — release the reservations of every truck standing behind a standing truck right
  now, without the 5-second delay.
- `BJ.Blocks [filter]` — every reservation straight from the segments' block arrays (reflection
  cannot see them): exclusive ones with their owner, shared ones with the exclusive they belong
  to, and a GHOST mark on those nobody references any more.
- `BJ.Purge` — release every ghost reservation found by the same walk.
- `Log LogBetterJunctions Verbose` — log every trimmed booking list.

Command output goes to the console it was typed in and to the log.

## Building

Same rules as any C++ mod: the editor must be closed. The first packaging of a new mod fails
during cook (no `UnrealEditor-BetterJunctions.dll`, alpakit passes `-nocompileeditor`): build the
`FactoryEditor` target once, then package with Alpakit as usual. Only Windows (EGS and Steam
clients) and WindowsServer targets are built.

## Verifying

The gridlock is hard to reproduce, so verify the other way round. `BJ.Dump` on a live server
should never show a truck with `reservations > 0`, `speed 0` and a stop target of
`vehicle … at 0 cm/s` for longer than five seconds. Every such release is logged under
`LogBetterJunctions` with the truck's name.

Not to be confused with another kind of stop: a truck whose route has only one station (this
happens after a station is rebuilt, the game removes it from every route) stops where it stands
with the `TooFewStations` status and blocks the lane. That is not a junction problem and not this
mod; give the truck its second station back.
