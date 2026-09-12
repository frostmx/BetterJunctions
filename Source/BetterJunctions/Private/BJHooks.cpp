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
	/**
	 * A vehicle ahead slower than this counts as a queue for the junction-entry rule: a truck
	 * does not enter a junction unless it can leave it past such a vehicle. Faster traffic is
	 * flowing, and the gap in front of it will have moved on by the time the junction is crossed.
	 */
	constexpr float SlowLeaderSpeed = 300.0f;
	/** Extra room required past a junction exit, beyond the truck's own length. */
	constexpr float JunctionExitMargin = 200.0f;
	/** How far ahead the booking filter looks for a standing vehicle. The game's own stop-target lookahead was measured at 1800. */
	constexpr float AvoidanceLookahead = 3000.0f;
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

	/** Command output goes to the log as before and, when a console asked, to that console too. */
	void Say(FOutputDevice* Ar, const FString& Line)
	{
		UE_LOG(LogBetterJunctions, Display, TEXT("%s"), *Line);
		if (Ar)
		{
			Ar->Logf(TEXT("%s"), *Line);
		}
	}
	void Say(FOutputDevice& Ar, const FString& Line) { Say(&Ar, Line); }

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

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDumpCommand(
		TEXT("BJ.Dump"),
		TEXT("BetterJunctions: print every autopilot truck with speed, stop target and reservations."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld* World, FOutputDevice& Ar)
		{
			FBetterJunctionsHooks::DumpVehicles(World, &Ar);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GUnstickCommand(
		TEXT("BJ.Unstick"),
		TEXT("BetterJunctions: release the reservations of every truck standing behind a standing truck."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld* World, FOutputDevice& Ar)
		{
			const int32 Released = FBetterJunctionsHooks::ReleaseStandingReservations(World, &Ar);
			Say(Ar, FString::Printf(TEXT("BJ.Unstick: released reservations of %d truck(s)"), Released));
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GBlocksCommand(
		TEXT("BJ.Blocks"),
		TEXT("BetterJunctions: list every path block reservation held by the segments, with owners and ghosts. Optional argument filters by segment name."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			FBetterJunctionsHooks::DumpBlockReservations(World, Args.Num() > 0 ? Args[0] : FString(), &Ar);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GPurgeCommand(
		TEXT("BJ.Purge"),
		TEXT("BetterJunctions: release every ghost path block reservation (one no vehicle references any more)."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld* World, FOutputDevice& Ar)
		{
			const int32 Purged = FBetterJunctionsHooks::PurgeGhostReservations(World, &Ar);
			Say(Ar, FString::Printf(TEXT("BJ.Purge: released %d ghost reservation(s)"), Purged));
		}));
}

void FBetterJunctionsHooks::Install()
{
	// Prevention. ReserveVehiclePathBlocks_Parallel is the one place blocks get booked, and the
	// list it receives is not bounded by the stop-target lookahead: a truck standing 5 cm behind
	// another was measured booking the junction two blocks past it (clamping the lookahead in
	// CalculatePathReservationStopTarget changed nothing). So the list itself is filtered: every
	// block beyond a standing truck ahead is dropped. Runs on a worker thread inside the parallel
	// autopilot tick; the helpers used are the ones that tick calls on the same thread.
	Installing(TEXT("UFGVehicleAutopilotComponent::ReserveVehiclePathBlocks_Parallel"));
	SUBSCRIBE_METHOD(UFGVehicleAutopilotComponent::ReserveVehiclePathBlocks_Parallel,
		[](auto& Scope, UFGVehicleAutopilotComponent* Self, const TArray<FVehicleAutopilotBlockReference>& PathBlocks, TSet<FVehiclePathBlockReference>& ReferencedPathBlocks)
		{
			TArray<FVehicleAutopilotBlockReference> Kept;
			if (FilterBlocksBeyondStandingVehicle(Self, PathBlocks, Kept))
			{
				Scope(Self, Kept, ReferencedPathBlocks);
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

bool FBetterJunctionsHooks::FilterBlocksBeyondStandingVehicle(const UFGVehicleAutopilotComponent* Autopilot,
	const TArray<FVehicleAutopilotBlockReference>& PathBlocks, TArray<FVehicleAutopilotBlockReference>& OutKept)
{
	if (PathBlocks.Num() == 0)
	{
		return false;
	}
	const AFGWheeledVehicle* Vehicle = Cast<AFGWheeledVehicle>(Autopilot->GetOwner());
	if (!Vehicle)
	{
		return false;
	}

	// Two rules, both keyed on the nearest vehicle ahead.
	//
	// Standing leader: the follower has to stop behind it, so nothing past it can be of use, and
	// a booking past it is exactly what locks the leader out of the junction. A moving leader is
	// left alone here on purpose: the two sort themselves out by speed matching.
	//
	// Junction entry: a truck does not book its way into a junction unless it can also leave it,
	// with a slow or standing vehicle ahead leaving room past the exit for the whole truck. The
	// game only checks that the exit block can be booked, not that it is free of a queue, so a
	// queue backing up through a junction leaves trucks standing inside it, holding its blocks.
	// Measured on 12.09.2026 on a dedicated server: two queue heads Deadlocked for minutes on
	// blocks overlapping the ones held by trucks standing inside the next junction back in the
	// same queue, and the watchdog could not help, since a truck must keep the block it stands on.
	const float HalfLength = UFGVehicleAutopilotComponent::CalculateVehicleHalfLength(Vehicle);
	TArray<FVehicleStopTarget> Ahead;
	Autopilot->CalculateVehicleAvoidanceTarget(AvoidanceLookahead, HalfLength, Ahead);
	float StandingDistance = TNumericLimits<float>::Max();
	float SlowDistance = TNumericLimits<float>::Max();
	for (const FVehicleStopTarget& Target : Ahead)
	{
		if (!Target.TargetMovementSpeed.IsSet())
		{
			continue;
		}
		const float Speed = Target.TargetMovementSpeed.GetValue();
		const float Distance = FMath::Max(Target.DistanceToStopTarget, 0.0f);
		if (Speed < LeaderStandingSpeed)
		{
			StandingDistance = FMath::Min(StandingDistance, Distance);
		}
		if (Speed < SlowLeaderSpeed)
		{
			SlowDistance = FMath::Min(SlowDistance, Distance);
		}
	}
	if (SlowDistance == TNumericLimits<float>::Max())
	{
		return false;
	}

	FVehicleAutopilotBlockReference Current;
	float DistanceToEndOfCurrentBlock = 0.0f;
	if (!Autopilot->FindPathBlockFromVehiclePosition(Autopilot->mCurrentServerVehicleSplinePosition, Current, DistanceToEndOfCurrentBlock))
	{
		return false;
	}
	const int32 NodeOffset = NodeIndexOffset(Autopilot, Current);
	const auto IsJunction = [&](const FVehicleAutopilotBlockReference& Block)
	{
		const AFGVehiclePathSegment* Segment = ResolveSegment(Autopilot, Block, NodeOffset);
		return Segment && Segment->IsJunctionBlock();
	};

	OutKept.Reset(PathBlocks.Num());
	const TCHAR* Reason = nullptr;
	float Room = 0.0f;
	for (int32 Index = 0; Index < PathBlocks.Num(); ++Index)
	{
		const FVehicleAutopilotBlockReference& Block = PathBlocks[Index];
		// The block under the truck is always kept.
		if (Block == Current)
		{
			OutKept.Add(Block);
			continue;
		}
		// Any block that starts short of a standing leader is kept too (including the one the
		// leader stands in: the follower is next in line for it and holding it keeps cross
		// traffic from cutting in). Beyond the leader nothing is, and the list is in path order.
		const float Distance = Autopilot->CalculateTotalDistanceBetweenPathBlocks(Current, Block);
		if (Distance > StandingDistance)
		{
			Reason = TEXT("standing vehicle");
			Room = StandingDistance;
			break;
		}
		// Entering a junction: the first junction block after a non-junction one. A truck already
		// inside one (its current block is a junction block) has to leave it, so no check there.
		if (NodeOffset != INDEX_NONE && IsJunction(Block) && !IsJunction(Index > 0 ? PathBlocks[Index - 1] : Current))
		{
			float ExitDistance = -1.0f;
			for (int32 Later = Index + 1; Later < PathBlocks.Num(); ++Later)
			{
				if (!IsJunction(PathBlocks[Later]))
				{
					ExitDistance = Autopilot->CalculateTotalDistanceBetweenPathBlocks(Current, PathBlocks[Later]);
					break;
				}
			}
			if (ExitDistance < 0.0f)
			{
				// The list ends inside the junction; take the end of its last block as the exit.
				const AFGVehiclePathSegment* LastSegment = ResolveSegment(Autopilot, PathBlocks.Last(), NodeOffset);
				ExitDistance = Autopilot->CalculateTotalDistanceBetweenPathBlocks(Current, PathBlocks.Last())
					+ (LastSegment ? LastSegment->GetVehiclePathBlockSize() : 0.0f);
			}
			if (SlowDistance < ExitDistance + 2.0f * HalfLength + JunctionExitMargin)
			{
				Reason = TEXT("junction exit not clear");
				Room = SlowDistance;
				break;
			}
		}
		OutKept.Add(Block);
	}
	if (!Reason)
	{
		return false;
	}

	UE_LOG(LogBetterJunctions, Verbose, TEXT("%s: booking %d of %d block(s), %s, vehicle %.0f cm ahead"),
		*VehicleLabel(Autopilot), OutKept.Num(), PathBlocks.Num(), Reason, Room);
	return true;
}

int32 FBetterJunctionsHooks::NodeIndexOffset(const UFGVehicleAutopilotComponent* Autopilot, const FVehicleAutopilotBlockReference& Current)
{
	// The route segment array is documented as "segment at index i starts at node i - 1 and ends
	// at node i", which leaves it open whether a block's node index names the segment before or
	// after the node. Settled per call against the segment the truck is known to be on.
	for (const int32 Offset : {0, 1, -1})
	{
		const AFGVehiclePathSegment* Segment = ResolveSegment(Autopilot, Current, Offset);
		if (Segment && Segment == Autopilot->mCurrentServerPathSegment)
		{
			return Offset;
		}
	}
	return INDEX_NONE;
}

const AFGVehiclePathSegment* FBetterJunctionsHooks::ResolveSegment(const UFGVehicleAutopilotComponent* Autopilot, const FVehicleAutopilotBlockReference& Block, int32 NodeOffset)
{
	if (NodeOffset == INDEX_NONE)
	{
		return nullptr;
	}
	const int32 Index = Block.PathNodeIndex + NodeOffset;
	return Autopilot->mCurrentVehicleRouteSegments.IsValidIndex(Index) ? Autopilot->mCurrentVehicleRouteSegments[Index].Get() : nullptr;
}

FString FBetterJunctionsHooks::DescribeAwaitedBlock(const UFGVehicleAutopilotComponent* Autopilot)
{
	if (!Autopilot->mNextBlockSequenceReference.IsSet())
	{
		return FString();
	}
	FVehicleAutopilotBlockReference Current;
	float DistanceToEnd = 0.0f;
	if (!Autopilot->FindPathBlockFromVehiclePosition(Autopilot->mCurrentServerVehicleSplinePosition, Current, DistanceToEnd))
	{
		return FString();
	}
	const int32 NodeOffset = NodeIndexOffset(Autopilot, Current);
	if (NodeOffset == INDEX_NONE)
	{
		return TEXT(", awaiting: route segments unresolved");
	}
	// The first block of the pending sequence that the truck does not hold is the one it waits for.
	for (const FVehicleAutopilotBlockReference& Ref : Autopilot->mNextBlockSequenceReference.GetValue())
	{
		AFGVehiclePathSegment* Segment = const_cast<AFGVehiclePathSegment*>(ResolveSegment(Autopilot, Ref, NodeOffset));
		if (!Segment)
		{
			continue;
		}
		FVehiclePathBlockReference Key;
		Key.Segment = Segment;
		Key.PathBlockIndex = Ref.PathBlockIndex;
		if (Autopilot->mPathBlockReservations.Contains(Key))
		{
			continue;
		}
		FString Holders;
		{
			FReadScopeLock Lock(Segment->mPathBlocksLock);
			const TArray<FVehiclePathBlock>& Blocks = Segment->GetVehiclePathBlocks();
			if (Blocks.IsValidIndex(Ref.PathBlockIndex))
			{
				for (const auto& Exclusive : Blocks[Ref.PathBlockIndex].ExclusiveReservations)
				{
					if (Exclusive.IsValid())
					{
						Holders += FString::Printf(TEXT(" %s (exclusive)"), *VehicleLabel(Exclusive->OwnerVehicle.Get()));
					}
				}
				for (const auto& Shared : Blocks[Ref.PathBlockIndex].SharedReservations)
				{
					const TSharedPtr<FVehiclePathBlockExclusiveReservation> Owner = Shared.IsValid() ? Shared->OwnerReservation.Pin() : nullptr;
					if (Owner.IsValid())
					{
						Holders += FString::Printf(TEXT(" %s (shared)"), *VehicleLabel(Owner->OwnerVehicle.Get()));
					}
				}
			}
		}
		return FString::Printf(TEXT(", awaiting %s#%d%s held by%s"), *ObjectTag(Segment), Ref.PathBlockIndex,
			Segment->IsJunctionBlock() ? TEXT(" J") : TEXT(""), Holders.IsEmpty() ? TEXT(" nobody") : *Holders);
	}
	return TEXT(", awaiting: sequence fully held");
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

void FBetterJunctionsHooks::ReleaseReservations(UFGVehicleAutopilotComponent* Autopilot, const TCHAR* Reason, FOutputDevice* Ar)
{
	const int32 Before = Autopilot->mPathBlockReservations.Num();
	Autopilot->ReleaseVehiclePathBlockReservations_Parallel();
	Say(Ar, FString::Printf(TEXT("%s: released %d reservation(s), %d left in map (%s)"),
		*VehicleLabel(Autopilot), Before, Autopilot->mPathBlockReservations.Num(), Reason));
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
			ReleaseReservations(Autopilot, TEXT("standing behind a standing vehicle"), nullptr);
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

int32 FBetterJunctionsHooks::ReleaseStandingReservations(UWorld* World, FOutputDevice* Ar)
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
			ReleaseReservations(Autopilot, TEXT("BJ.Unstick"), Ar);
			++Released;
		}
	}
	return Released;
}

void FBetterJunctionsHooks::DumpVehicles(UWorld* World, FOutputDevice* Ar)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem))
	{
		Say(Ar, FString::Printf(TEXT("BJ.Dump: no vehicle subsystem")));
		return;
	}
	Say(Ar, FString::Printf(TEXT("BJ.Dump: net mode %d, %d vehicle(s)"), (int32)Subsystem->GetNetMode(), Subsystem->GetAllVehicles().Num()));
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
			Say(Ar, FString::Printf(TEXT("  %s: vehicle not loaded"), *Id->GetVehicleName().ToString()));
			continue;
		}
		FString StopTarget = TEXT("none");
		if (Autopilot->mServerClosestStopTargetIsSet)
		{
			const FVehicleStopTarget& Target = Autopilot->mServerClosestStopTarget;
			StopTarget = Target.TargetMovementSpeed.IsSet()
				? FString::Printf(TEXT("vehicle %.0f cm ahead at %.0f cm/s"), Target.DistanceToStopTarget, Target.TargetMovementSpeed.GetValue())
				: FString::Printf(TEXT("block or dock in %.0f cm%s"), Target.DistanceToStopTarget, *DescribeAwaitedBlock(Autopilot));
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
		Say(Ar, FString::Printf(TEXT("  %s: status %d, speed %.0f, on %s, waited %.0f s on block, reservations %d {%s }, stop target: %s%s"),
			*VehicleLabel(Vehicle), (int32)Id->GetAutopilotErrorStatus(), Autopilot->GetCurrentForwardSpeed(),
			*ObjectTag(Autopilot->mCurrentServerPathSegment), Autopilot->mTimeSpentWaitingOnCurrentFreeBlock,
			Autopilot->mPathBlockReservations.Num(), *Reservations, *StopTarget, *Standing));
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

void FBetterJunctionsHooks::DumpBlockReservations(UWorld* World, const FString& Filter, FOutputDevice* Ar)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem))
	{
		Say(Ar, FString::Printf(TEXT("BJ.Blocks: no vehicle subsystem")));
		return;
	}
	int32 Blocks = 0;
	TArray<FBJGhostReservation> Ghosts;
	WalkReservations(Subsystem, Filter, Ghosts, [&Blocks, Ar](const AFGVehiclePathSegment* Segment, int32 Index, const FVehiclePathBlock& Block)
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
		Say(Ar, FString::Printf(TEXT("%s"), *Line));
	});
	Say(Ar, FString::Printf(TEXT("BJ.Blocks: %d block(s) with reservations, %d ghost(s)%s"), Blocks, Ghosts.Num(),
		Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", filter '%s'"), *Filter)));
}

int32 FBetterJunctionsHooks::PurgeGhostReservations(UWorld* World, FOutputDevice* Ar)
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
			Say(Ar, FString::Printf(TEXT("BJ.Purge: exclusive %s#%d owned by %s"), *ObjectTag(Segment), Ghost.Exclusive->ReservedPathBlockIndex, *VehicleLabel(Ghost.Exclusive->OwnerVehicle.Get())));
			Segment->ReleaseExclusiveReservation_ThreadSafe(Ghost.Exclusive);
			++Purged;
		}
		else if (Ghost.Shared.IsValid())
		{
			Say(Ar, FString::Printf(TEXT("BJ.Purge: shared %s#%d"), *ObjectTag(Segment), Ghost.Shared->ReservedPathBlockIndex));
			Segment->ReleaseSharedReservation_ThreadSafe(Ghost.Shared);
			++Purged;
		}
	}
	return Purged;
}
