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
	static void DumpVehicles(UWorld* World);

	/** Releases reservations of every truck standing behind a standing truck. BJ.Unstick */
	static int32 ReleaseStandingReservations(UWorld* World);

	/**
	 * Lists every exclusive and shared reservation held in the segments' own block arrays, with
	 * its owner and whether the owner still references it. Reflection cannot see these arrays;
	 * a reservation nobody references is a ghost and blocks the junction forever. BJ.Blocks [filter]
	 */
	static void DumpBlockReservations(UWorld* World, const FString& Filter);

	/** Releases every ghost reservation found by the same walk. BJ.Purge */
	static int32 PurgeGhostReservations(UWorld* World);

private:
	/** Pre-hook: reservation lookahead never extends past a standing truck ahead. */
	static float ClampLookaheadToStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot, float MaxLookahead, float VehicleHalfLength);

	/** Post-hook of the subsystem autopilot tick: the standing-behind-standing watchdog. */
	static void TickWatchdog(AFGVehicleSubsystem* Subsystem, float DeltaTime);

	static bool IsStandingBehindStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot);
	static void ReleaseReservations(UFGVehicleAutopilotComponent* Autopilot, const TCHAR* Reason);

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
