#pragma once

#include "CoreMinimal.h"

class AFGVehicleSubsystem;
class UFGVehicleAutopilotComponent;

/**
 * The two hooks and the console commands. Declared as a class (not a namespace) because the
 * access transformers make it a friend of UFGVehicleAutopilotComponent and AFGVehicleSubsystem,
 * which is what lets it name their protected methods and read the reservation map.
 *
 * Measured on 12.09.2026 with 29 trucks and four lanes gridlocked around one crossing: a single
 * reservation (block #1 of the turn segment, held by a truck queued second on the eastbound
 * lane) blocked all three other lane heads. Releasing it by toggling that truck's autopilot
 * cleared three lanes within three seconds and the fourth once the first drained.
 */
class FBetterJunctionsHooks
{
public:
	static void Install();

	/** Prints every autopilot truck with speed, stop target and reservation count. BJ.Dump */
	static void DumpVehicles(UWorld* World, FOutputDevice* Ar = nullptr);

	/** Releases reservations of every truck standing behind a standing truck. BJ.Unstick */
	static int32 ReleaseStandingReservations(UWorld* World, FOutputDevice* Ar = nullptr);

	/**
	 * Switches autopilot off or on for every truck whose name contains Filter (empty: all).
	 * Per-truck lines go to Ar; the one-line summary is returned for the caller to print,
	 * because the console and the chat command report it differently. BJ.Autopilot, /autopilot
	 */
	static FString SetAutopilotForAll(UWorld* World, bool bEnable, const FString& Filter, FOutputDevice* Ar = nullptr);

	/** "off" / "on" (any case) into a flag; false for anything else. Shared by the console and the chat command. */
	static bool ParseAutopilotMode(const FString& Word, bool& bOutEnable)
	{
		if (Word.Equals(TEXT("on"), ESearchCase::IgnoreCase)) { bOutEnable = true; return true; }
		if (Word.Equals(TEXT("off"), ESearchCase::IgnoreCase)) { bOutEnable = false; return true; }
		return false;
	}

	/**
	 * Lists every exclusive and shared reservation held in the segments' own block arrays, with
	 * its owner and whether the owner still references it. Reflection cannot see these arrays;
	 * a reservation nobody references is a ghost and blocks the junction forever. BJ.Blocks [filter]
	 */
	static void DumpBlockReservations(UWorld* World, const FString& Filter, FOutputDevice* Ar = nullptr);

	/** Releases every ghost reservation found by the same walk. BJ.Purge */
	static int32 PurgeGhostReservations(UWorld* World, FOutputDevice* Ar = nullptr);

	/**
	 * For every autopilot truck whose name contains Filter: the rest of its current leg against
	 * the best path around standing vehicles, and with bApply the switch to it where it pays off.
	 * With bAvoid (a test aid) the next few segments of each truck's path are priced as jammed.
	 * Returns the number of trucks rerouted. BJ.Reroute [filter] [apply] [avoid]
	 */
	static int32 RerouteVehicles(UWorld* World, const FString& Filter, bool bApply, bool bAvoid, FOutputDevice* Ar = nullptr);

private:
	/** Rerouting: every few seconds, trucks with a jam ahead switch to a cheaper path when BJ.Reroute.Auto is set. Game thread, post-tick. */
	static void TickReroute(AFGVehicleSubsystem* Subsystem, float DeltaTime);

	/**
	 * Replans the tail of one truck's current leg with jam penalties and, with bApply, swaps it in.
	 * Returns true if the truck was rerouted. Penalties maps a segment to its extra cost in cm and
	 * gets the new path's herding penalty added when a truck is rerouted.
	 */
	static bool TryReroute(AFGVehicleSubsystem* Subsystem, UFGVehicleAutopilotComponent* Autopilot, TMap<const class AFGVehiclePathSegment*, float>& Penalties,
		bool bApply, bool bAvoid, bool bVerbose, FOutputDevice* Ar);

	/**
	 * True if nobody but Self holds or blocks the segment's blocks (all of them, or only the first):
	 * no reservations of others on them, no exclusives of others on blocks overlapping them, and
	 * (for the whole segment) no other vehicle inside.
	 */
	static bool IsSegmentFreeFor(const class AFGVehiclePathSegment* Segment, const class AFGWheeledVehicle* Self, bool bFirstBlockOnly);

	/** Extra cost of each segment: vehicles that have stood on it long enough. Also keeps the standing timers. */
	static void BuildJamPenalties(AFGVehicleSubsystem* Subsystem, TMap<const class AFGVehiclePathSegment*, float>& OutPenalties);

	/**
	 * Pre-hook of the booking call: drops every block that lies beyond a standing truck ahead,
	 * and the entry into any junction the truck could not leave because a slow or standing truck
	 * ahead leaves no room past the exit. Returns false when nothing has to change, true with the
	 * filtered list in OutKept otherwise. The booking call also fills the set of blocks the game
	 * keeps, so blocks dropped here are released by the game's own cleanup.
	 */
	static bool FilterBlocksBeyondStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot,
		const TArray<struct FVehicleAutopilotBlockReference>& PathBlocks, TArray<struct FVehicleAutopilotBlockReference>& OutKept);

	/** Offset from a block's node index to its segment's index in the route segment array, or INDEX_NONE if it cannot be settled. */
	static int32 NodeIndexOffset(const UFGVehicleAutopilotComponent* Autopilot, const struct FVehicleAutopilotBlockReference& Current);
	static const class AFGVehiclePathSegment* ResolveSegment(const UFGVehicleAutopilotComponent* Autopilot, const struct FVehicleAutopilotBlockReference& Block, int32 NodeOffset);

	/** ", sequence { <seg>#<idx>=free|mine|[holders] ... }" for a truck waiting on a block: every block of the pending sequence with what stands in the way. BJ.Dump */
	static FString DescribeAwaitedBlock(const UFGVehicleAutopilotComponent* Autopilot);

	/** Post-hook of the subsystem autopilot tick: the standing-behind-standing watchdog. */
	static void TickWatchdog(AFGVehicleSubsystem* Subsystem, float DeltaTime);

	static bool IsStandingBehindStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot);
	/** Standing on a reservation stop target (a block, not a vehicle) with a pending block sequence. */
	static bool IsWaitingOnBlock(const UFGVehicleAutopilotComponent* Autopilot);
	/** Rebuilds the junction priority map from the trucks that have waited long enough on a block. Game thread, post-tick. */
	static void RebuildPriority(AFGVehicleSubsystem* Subsystem);
	static void ReleaseReservations(UFGVehicleAutopilotComponent* Autopilot, const TCHAR* Reason, FOutputDevice* Ar);

	/** True if the owner vehicle's autopilot still has this exclusive reservation in its map. */
	static bool IsReferencedByOwner(const TSharedPtr<struct FVehiclePathBlockExclusiveReservation>& Exclusive);

	/**
	 * Walks every block of every segment under the segment's read lock, calling OnBlock for each
	 * block that holds any reservation and collecting ghosts. Nothing may be released inside the
	 * walk: the release functions take the same lock.
	 */
	static void WalkReservations(AFGVehicleSubsystem* Subsystem, const FString& Filter, TArray<struct FBJGhostReservation>& OutGhosts,
		const TFunctionRef<void(const class AFGVehiclePathSegment*, int32, const struct FVehiclePathBlock&)>& OnBlock);
};
