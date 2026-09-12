#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBetterJunctions, Log, All);

/**
 * Keeps self-driving trucks from gridlocking at crossroads.
 *
 * The game reserves junction "path blocks" ahead of every autopilot truck within a braking
 * lookahead. That lookahead does not stop at the truck in front: a follower reserves blocks of
 * the junction the leader is waiting to enter, then stops behind the leader on vehicle
 * avoidance. The leader cannot take its sequence because the follower's block overlaps it, and
 * the follower never releases because it is waiting on a vehicle, not on a block - the built-in
 * 120 s deadlock timer only counts the latter. Every truck that then arrives at any of the
 * crossing lanes queues behind one of the two.
 *
 * Two server-side hooks close the hole: reservation lookahead is clamped to a standing truck
 * ahead, and a watchdog releases the reservations of a truck that has been standing behind a
 * standing truck for a few seconds. Nothing is replicated, so the mod is server-only.
 */
class FBetterJunctionsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
