#include "BJHooks.h"

#include "BetterJunctions.h"
#include "Patching/NativeHookManager.h"

#include "WheeledVehicles/FGVehicleAutopilotComponent.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"

/** A reservation found in a segment's block arrays that no vehicle references any more. */
struct FBJGhostReservation
{
	TWeakObjectPtr<AFGVehiclePathSegment> Segment;
	TSharedPtr<FVehiclePathBlockExclusiveReservation> Exclusive;
	TSharedPtr<FVehiclePathBlockSharedReservation> Shared;
};

namespace
{
	// Units are the autopilot's own: centimeters and centimeters per second. The header comment
	// on the forward speed says meters per second; the running game says otherwise (a truck at
	// cruise reports about 1100).

	/** A vehicle ahead slower than this counts as standing. */
	constexpr float LeaderStandingSpeed = 100.0f;
	/** Our own speed below this counts as standing. */
	constexpr float SelfStandingSpeed = 30.0f;
	/** How long a truck may stand behind a standing truck before its reservations are dropped. */
	constexpr float WatchdogGraceSeconds = 5.0f;
	/** Forgotten watchdog entries are swept this often. */
	constexpr float WatchdogSweepSeconds = 30.0f;

	struct FStandingState
	{
		float StandingSeconds = 0.0f;
		bool bReleasedThisEpisode = false;
	};

	// Game thread only: written by the subsystem post-tick and the console commands.
	TMap<TWeakObjectPtr<UFGVehicleAutopilotComponent>, FStandingState> GStanding;
	float GSinceSweep = 0.0f;

	void Installing(const TCHAR* Target)
	{
		// SML turns a funchook refusal into a fatal error inside StartupModule. Naming the target
		// first turns that from a stack trace into a log line saying which function was refused.
		UE_LOG(LogBetterJunctions, Display, TEXT("installing: %s"), Target);
	}

	/** Short, stable label: the actor name tail, since the display name changes with the route. */
	FString ObjectTag(const UObject* Object)
	{
		const FString Name = GetNameSafe(Object);
		return Name.Len() > 10 ? Name.Right(10) : Name;
	}

	FString VehicleLabel(const AFGWheeledVehicle* Vehicle)
	{
		const AFGWheeledVehicleIdentifier* Id = Vehicle ? Vehicle->GetVehicleIdentifier() : nullptr;
		return FString::Printf(TEXT("%s [%s]"), Id ? *Id->GetVehicleName().ToString() : TEXT("?"), *ObjectTag(Vehicle));
	}

	FString VehicleLabel(const UFGVehicleAutopilotComponent* Autopilot)
	{
		return VehicleLabel(Autopilot ? Cast<AFGWheeledVehicle>(Autopilot->GetOwner()) : nullptr);
	}

	FAutoConsoleCommandWithWorldAndArgs GDumpCommand(
		TEXT("BJ.Dump"),
		TEXT("BetterJunctions: print every autopilot truck with speed, stop target and reservations."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			FBetterJunctionsHooks::DumpVehicles(World);
		}));

	FAutoConsoleCommandWithWorldAndArgs GUnstickCommand(
		TEXT("BJ.Unstick"),
		TEXT("BetterJunctions: release the reservations of every truck standing behind a standing truck."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			const int32 Released = FBetterJunctionsHooks::ReleaseStandingReservations(World);
			UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Unstick: released reservations of %d truck(s)"), Released);
		}));

	FAutoConsoleCommandWithWorldAndArgs GBlocksCommand(
		TEXT("BJ.Blocks"),
		TEXT("BetterJunctions: list every path block reservation held by the segments, with owners and ghosts. Optional argument filters by segment name."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FBetterJunctionsHooks::DumpBlockReservations(World, Args.Num() > 0 ? Args[0] : FString());
		}));

	FAutoConsoleCommandWithWorldAndArgs GPurgeCommand(
		TEXT("BJ.Purge"),
		TEXT("BetterJunctions: release every ghost path block reservation (one no vehicle references any more)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			const int32 Purged = FBetterJunctionsHooks::PurgeGhostReservations(World);
			UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Purge: released %d ghost reservation(s)"), Purged);
		}));
}

void FBetterJunctionsHooks::Install()
{
	// Prevention. CalculatePathReservationStopTarget books the junction blocks within the
	// lookahead and reports where the booked stretch ends. It runs on a worker thread inside the
	// parallel autopilot tick; CalculateVehicleAvoidanceTarget is what the same tick uses to
	// look for vehicles ahead, so calling it from here adds no new shared state.
	Installing(TEXT("UFGVehicleAutopilotComponent::CalculatePathReservationStopTarget"));
	SUBSCRIBE_METHOD(UFGVehicleAutopilotComponent::CalculatePathReservationStopTarget,
		[](auto& Scope, const UFGVehicleAutopilotComponent* Self, float MaxLookaheadDistance, float VehicleHalfLength, TArray<FVehicleStopTarget>& OutStopTargets)
		{
			const float Clamped = ClampLookaheadToStandingVehicle(Self, MaxLookaheadDistance, VehicleHalfLength);
			if (Clamped < MaxLookaheadDistance)
			{
				Scope(Self, Clamped, VehicleHalfLength, OutStopTargets);
			}
		});

	// Cure. The subsystem tick runs every autopilot in parallel and then applies the results on
	// the game thread; after it returns nothing is in flight, so releasing here is safe.
	Installing(TEXT("AFGVehicleSubsystem::TickVehicleAutopilot"));
	SUBSCRIBE_METHOD_AFTER(AFGVehicleSubsystem::TickVehicleAutopilot,
		[](AFGVehicleSubsystem* Self, float DeltaTime)
		{
			TickWatchdog(Self, DeltaTime);
		});
}

float FBetterJunctionsHooks::ClampLookaheadToStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot, float MaxLookahead, float VehicleHalfLength)
{
	TArray<FVehicleStopTarget> Ahead;
	Autopilot->CalculateVehicleAvoidanceTarget(MaxLookahead, VehicleHalfLength, Ahead);

	float Clamped = MaxLookahead;
	for (const FVehicleStopTarget& Target : Ahead)
	{
		// A moving leader is left alone on purpose: clamping to it would turn its tail into a
		// stop target and make convoys crawl. Only a standing one has to be respected, and the
		// follower has to stop there anyway.
		if (Target.TargetMovementSpeed.IsSet() && Target.TargetMovementSpeed.GetValue() < LeaderStandingSpeed)
		{
			Clamped = FMath::Min(Clamped, FMath::Max(Target.DistanceToStopTarget, 0.0f));
		}
	}

	if (Clamped < MaxLookahead)
	{
		UE_LOG(LogBetterJunctions, Verbose, TEXT("%s: lookahead %.0f -> %.0f, standing vehicle ahead"),
			*VehicleLabel(Autopilot), MaxLookahead, Clamped);
	}
	return Clamped;
}

bool FBetterJunctionsHooks::IsStandingBehindStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot)
{
	if (FMath::Abs(Autopilot->GetCurrentForwardSpeed()) >= SelfStandingSpeed)
	{
		return false;
	}
	// Only vehicle-avoidance targets carry a movement speed. A reservation or a docking target
	// has none, and a truck waiting on those is covered by the game's own deadlock timer.
	if (!Autopilot->mServerClosestStopTargetIsSet || !Autopilot->mServerClosestStopTarget.TargetMovementSpeed.IsSet())
	{
		return false;
	}
	return Autopilot->mServerClosestStopTarget.TargetMovementSpeed.GetValue() < LeaderStandingSpeed;
}

void FBetterJunctionsHooks::ReleaseReservations(UFGVehicleAutopilotComponent* Autopilot, const TCHAR* Reason)
{
	const int32 Before = Autopilot->mPathBlockReservations.Num();
	Autopilot->ReleaseVehiclePathBlockReservations_Parallel();
	UE_LOG(LogBetterJunctions, Display, TEXT("%s: released %d reservation(s), %d left in map (%s)"),
		*VehicleLabel(Autopilot), Before, Autopilot->mPathBlockReservations.Num(), Reason);
}

void FBetterJunctionsHooks::TickWatchdog(AFGVehicleSubsystem* Subsystem, float DeltaTime)
{
	// Reservations exist on the authority only. On a client the subsystem still ticks the
	// simulated movement, and there is nothing to release.
	if (!IsValid(Subsystem) || Subsystem->GetNetMode() == NM_Client)
	{
		return;
	}

	for (AFGWheeledVehicleIdentifier* Id : Subsystem->GetAllVehicles())
	{
		if (!IsValid(Id) || !Id->IsAutopilotEnabled())
		{
			continue;
		}
		const AFGWheeledVehicle* Vehicle = Id->GetOwnerVehicle();
		UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
		if (!IsValid(Autopilot))
		{
			continue;
		}

		FStandingState& State = GStanding.FindOrAdd(Autopilot);
		if (!IsStandingBehindStandingVehicle(Autopilot))
		{
			State = FStandingState{};
			continue;
		}

		State.StandingSeconds += DeltaTime;
		// Once per standing episode: after the release the pre-hook keeps new reservations short
		// of the leader, and those are worth keeping, they hold the follower's place in line.
		if (!State.bReleasedThisEpisode && State.StandingSeconds >= WatchdogGraceSeconds && Autopilot->mPathBlockReservations.Num() > 0)
		{
			ReleaseReservations(Autopilot, TEXT("standing behind a standing vehicle"));
			State.bReleasedThisEpisode = true;
		}
	}

	GSinceSweep += DeltaTime;
	if (GSinceSweep >= WatchdogSweepSeconds)
	{
		GSinceSweep = 0.0f;
		for (auto It = GStanding.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}
}

int32 FBetterJunctionsHooks::ReleaseStandingReservations(UWorld* World)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem) || Subsystem->GetNetMode() == NM_Client)
	{
		return 0;
	}
	int32 Released = 0;
	for (AFGWheeledVehicleIdentifier* Id : Subsystem->GetAllVehicles())
	{
		const AFGWheeledVehicle* Vehicle = IsValid(Id) && Id->IsAutopilotEnabled() ? Id->GetOwnerVehicle() : nullptr;
		UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
		if (IsValid(Autopilot) && IsStandingBehindStandingVehicle(Autopilot) && Autopilot->mPathBlockReservations.Num() > 0)
		{
			ReleaseReservations(Autopilot, TEXT("BJ.Unstick"));
			++Released;
		}
	}
	return Released;
}

void FBetterJunctionsHooks::DumpVehicles(UWorld* World)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Dump: no vehicle subsystem"));
		return;
	}
	UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Dump: net mode %d, %d vehicle(s)"), (int32)Subsystem->GetNetMode(), Subsystem->GetAllVehicles().Num());
	for (AFGWheeledVehicleIdentifier* Id : Subsystem->GetAllVehicles())
	{
		if (!IsValid(Id) || !Id->IsAutopilotEnabled())
		{
			continue;
		}
		const AFGWheeledVehicle* Vehicle = Id->GetOwnerVehicle();
		const UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
		if (!IsValid(Autopilot))
		{
			UE_LOG(LogBetterJunctions, Display, TEXT("  %s: vehicle not loaded"), *Id->GetVehicleName().ToString());
			continue;
		}
		FString StopTarget = TEXT("none");
		if (Autopilot->mServerClosestStopTargetIsSet)
		{
			const FVehicleStopTarget& Target = Autopilot->mServerClosestStopTarget;
			StopTarget = Target.TargetMovementSpeed.IsSet()
				? FString::Printf(TEXT("vehicle %.0f cm ahead at %.0f cm/s"), Target.DistanceToStopTarget, Target.TargetMovementSpeed.GetValue())
				: FString::Printf(TEXT("block or dock in %.0f cm"), Target.DistanceToStopTarget);
		}
		FString Reservations;
		for (const auto& Pair : Autopilot->mPathBlockReservations)
		{
			Reservations += FString::Printf(TEXT(" %s#%d"), *ObjectTag(Pair.Key.Segment), Pair.Key.PathBlockIndex);
		}
		FString Standing;
		if (const FStandingState* State = GStanding.Find(Autopilot); State && State->StandingSeconds > 0.0f)
		{
			Standing = FString::Printf(TEXT(", standing behind standing %.0f s%s"), State->StandingSeconds, State->bReleasedThisEpisode ? TEXT(" (released)") : TEXT(""));
		}
		UE_LOG(LogBetterJunctions, Display, TEXT("  %s: status %d, speed %.0f, on %s, waited %.0f s on block, reservations %d {%s }, stop target: %s%s"),
			*VehicleLabel(Vehicle), (int32)Id->GetAutopilotErrorStatus(), Autopilot->GetCurrentForwardSpeed(),
			*ObjectTag(Autopilot->mCurrentServerPathSegment), Autopilot->mTimeSpentWaitingOnCurrentFreeBlock,
			Autopilot->mPathBlockReservations.Num(), *Reservations, *StopTarget, *Standing);
	}
}

bool FBetterJunctionsHooks::IsReferencedByOwner(const TSharedPtr<FVehiclePathBlockExclusiveReservation>& Exclusive)
{
	const AFGWheeledVehicle* Vehicle = Exclusive->OwnerVehicle.Get();
	const UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
	if (!IsValid(Autopilot))
	{
		return false;
	}
	for (const auto& Pair : Autopilot->mPathBlockReservations)
	{
		if (Pair.Value.Reservation.Get() == Exclusive.Get())
		{
			return true;
		}
	}
	return false;
}

void FBetterJunctionsHooks::WalkReservations(AFGVehicleSubsystem* Subsystem, const FString& Filter, TArray<FBJGhostReservation>& OutGhosts,
	const TFunctionRef<void(const AFGVehiclePathSegment*, int32, const FVehiclePathBlock&)>& OnBlock)
{
	for (AFGVehiclePathSegment* Segment : Subsystem->mAllPathSegments)
	{
		if (!IsValid(Segment) || (!Filter.IsEmpty() && !Segment->GetName().Contains(Filter)))
		{
			continue;
		}
		FReadScopeLock Lock(Segment->mPathBlocksLock);
		const TArray<FVehiclePathBlock>& Blocks = Segment->GetVehiclePathBlocks();
		for (int32 Index = 0; Index < Blocks.Num(); ++Index)
		{
			const FVehiclePathBlock& Block = Blocks[Index];
			if (Block.ExclusiveReservations.Num() == 0 && Block.SharedReservations.Num() == 0)
			{
				continue;
			}
			OnBlock(Segment, Index, Block);
			for (const auto& Exclusive : Block.ExclusiveReservations)
			{
				if (Exclusive.IsValid() && !IsReferencedByOwner(Exclusive))
				{
					OutGhosts.Add({Segment, Exclusive, nullptr});
				}
			}
			for (const auto& Shared : Block.SharedReservations)
			{
				if (Shared.IsValid() && !Shared->OwnerReservation.IsValid())
				{
					OutGhosts.Add({Segment, nullptr, Shared});
				}
			}
		}
	}
}

void FBetterJunctionsHooks::DumpBlockReservations(UWorld* World, const FString& Filter)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Blocks: no vehicle subsystem"));
		return;
	}
	int32 Blocks = 0;
	TArray<FBJGhostReservation> Ghosts;
	WalkReservations(Subsystem, Filter, Ghosts, [&Blocks](const AFGVehiclePathSegment* Segment, int32 Index, const FVehiclePathBlock& Block)
	{
		++Blocks;
		FString Line = FString::Printf(TEXT("  %s#%d%s:"), *ObjectTag(Segment), Index, Segment->IsJunctionBlock() ? TEXT(" J") : TEXT(""));
		for (const auto& Exclusive : Block.ExclusiveReservations)
		{
			if (!Exclusive.IsValid())
			{
				Line += TEXT(" exclusive(null)");
				continue;
			}
			const AFGWheeledVehicle* Owner = Exclusive->OwnerVehicle.Get();
			Line += FString::Printf(TEXT(" exclusive by %s%s (%d shared)"), *VehicleLabel(Owner),
				IsReferencedByOwner(Exclusive) ? TEXT("") : TEXT(" GHOST"), Exclusive->SharedReservations.Num());
		}
		for (const auto& Shared : Block.SharedReservations)
		{
			if (!Shared.IsValid())
			{
				Line += TEXT(" shared(null)");
				continue;
			}
			const TSharedPtr<FVehiclePathBlockExclusiveReservation> Owner = Shared->OwnerReservation.Pin();
			Line += Owner.IsValid()
				? FString::Printf(TEXT(" shared for %s#%d of %s"), *ObjectTag(Owner->ReservedSegment.Get()), Owner->ReservedPathBlockIndex, *VehicleLabel(Owner->OwnerVehicle.Get()))
				: FString(TEXT(" shared GHOST (owner reservation gone)"));
		}
		UE_LOG(LogBetterJunctions, Display, TEXT("%s"), *Line);
	});
	UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Blocks: %d block(s) with reservations, %d ghost(s)%s"), Blocks, Ghosts.Num(),
		Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", filter '%s'"), *Filter));
}

int32 FBetterJunctionsHooks::PurgeGhostReservations(UWorld* World)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem) || Subsystem->GetNetMode() == NM_Client)
	{
		return 0;
	}
	TArray<FBJGhostReservation> Ghosts;
	WalkReservations(Subsystem, FString(), Ghosts, [](const AFGVehiclePathSegment*, int32, const FVehiclePathBlock&) {});

	// Released outside the walk: the release functions take the segment lock for writing.
	int32 Purged = 0;
	for (const FBJGhostReservation& Ghost : Ghosts)
	{
		AFGVehiclePathSegment* Segment = Ghost.Segment.Get();
		if (!IsValid(Segment))
		{
			continue;
		}
		if (Ghost.Exclusive.IsValid())
		{
			UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Purge: exclusive %s#%d owned by %s"), *ObjectTag(Segment), Ghost.Exclusive->ReservedPathBlockIndex, *VehicleLabel(Ghost.Exclusive->OwnerVehicle.Get()));
			Segment->ReleaseExclusiveReservation_ThreadSafe(Ghost.Exclusive);
			++Purged;
		}
		else if (Ghost.Shared.IsValid())
		{
			UE_LOG(LogBetterJunctions, Display, TEXT("BJ.Purge: shared %s#%d"), *ObjectTag(Segment), Ghost.Shared->ReservedPathBlockIndex);
			Segment->ReleaseSharedReservation_ThreadSafe(Ghost.Shared);
			++Purged;
		}
	}
	return Purged;
}
