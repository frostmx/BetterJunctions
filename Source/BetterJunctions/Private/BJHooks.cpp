#include "BJHooks.h"

#include "BetterJunctions.h"
#include "Patching/NativeHookManager.h"

#include "WheeledVehicles/FGVehicleAutopilotComponent.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"

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

	FString VehicleLabel(const UFGVehicleAutopilotComponent* Autopilot)
	{
		const AFGWheeledVehicle* Vehicle = Autopilot ? Cast<AFGWheeledVehicle>(Autopilot->GetOwner()) : nullptr;
		const AFGWheeledVehicleIdentifier* Id = Vehicle ? Vehicle->GetVehicleIdentifier() : nullptr;
		return Id ? Id->GetVehicleName().ToString() : GetNameSafe(Vehicle);
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
		FString Standing;
		if (const FStandingState* State = GStanding.Find(Autopilot); State && State->StandingSeconds > 0.0f)
		{
			Standing = FString::Printf(TEXT(", standing behind standing %.0f s%s"), State->StandingSeconds, State->bReleasedThisEpisode ? TEXT(" (released)") : TEXT(""));
		}
		UE_LOG(LogBetterJunctions, Display, TEXT("  %s: status %d, speed %.0f, waited %.0f s on block, reservations %d, stop target: %s%s"),
			*Id->GetVehicleName().ToString(), (int32)Id->GetAutopilotErrorStatus(), Autopilot->GetCurrentForwardSpeed(),
			Autopilot->mTimeSpentWaitingOnCurrentFreeBlock, Autopilot->mPathBlockReservations.Num(), *StopTarget, *Standing);
	}
}
