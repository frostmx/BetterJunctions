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

private:
	/** Pre-hook: reservation lookahead never extends past a standing truck ahead. */
	static float ClampLookaheadToStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot, float MaxLookahead, float VehicleHalfLength);

	/** Post-hook of the subsystem autopilot tick: the standing-behind-standing watchdog. */
	static void TickWatchdog(AFGVehicleSubsystem* Subsystem, float DeltaTime);

	static bool IsStandingBehindStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot);
	static void ReleaseReservations(UFGVehicleAutopilotComponent* Autopilot, const TCHAR* Reason);
};
