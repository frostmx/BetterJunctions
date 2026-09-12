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
	 * Lists every exclusive and shared reservation held in the segments' own block arrays, with
	 * its owner and whether the owner still references it. Reflection cannot see these arrays;
	 * a reservation nobody references is a ghost and blocks the junction forever. BJ.Blocks [filter]
	 */
	static void DumpBlockReservations(UWorld* World, const FString& Filter, FOutputDevice* Ar = nullptr);

	/** Releases every ghost reservation found by the same walk. BJ.Purge */
	static int32 PurgeGhostReservations(UWorld* World, FOutputDevice* Ar = nullptr);

private:
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
