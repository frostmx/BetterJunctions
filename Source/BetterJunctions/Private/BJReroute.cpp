#include "BJHooks.h"

#include "BetterJunctions.h"
#include "Algo/Reverse.h"
#include "Components/SplineComponent.h"
#include "HAL/IConsoleManager.h"

#include "WheeledVehicles/FGVehicleAutopilotComponent.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"

// Rerouting around jams.
//
// The game plans a truck's way to its next station once, with A* over the path network and a
// static cost per segment (its length), and keeps it until the station is reached or the network
// or the station list changes. Nothing in that cost knows about a queue. Here the tail of the
// current leg is replanned with standing vehicles priced in, and swapped in when it pays off.
//
// The route arrays are indexed by node: node i of mCurrentVehicleRoute, and segment i of
// mCurrentVehicleRouteSegments running from node i - 1 to node i. Block references held by the
// autopilot (reservations, the free block, the pending sequence) name node indices, so the
// prefix up to the end of the last segment the truck holds anything on is kept as it is and only
// what follows is replaced. The pending block sequence and the free block are reset and
// recomputed by the next tick. The layout was confirmed on a live save (18.09.2026): segment k
// joins nodes k - 1 and k for every truck on the road, segment 0 being the one it started on.
//
// A truck waiting at a junction gets a rule of its own: the junction segments it waits for are
// taken out of the network, and the best way round them is taken only if its own blocks through
// the junction are free this very moment. Otherwise the truck keeps its place in the queue.
//
// Recon on 18.09.2026 (probe_bj_detours.py): for a 100 m queue a detour no longer than 1.5x
// exists on about half of the road the trucks drive; 24 of 105 legs have none at all.

namespace
{
	/** Extra cost of a segment per vehicle standing on it, in cm: a queue of trucks costs roughly this much driving each. */
	constexpr float JamPenaltyPerVehicle = 15000.0f;
	/** A vehicle slower than this counts as standing (cm/s). */
	constexpr float JamStandingSpeed = 100.0f;
	/**
	 * ...and only once it has stood this long. A truck waiting a few seconds for its turn at a
	 * junction is traffic, not a jam: without this the first automatic run (18.09.2026) rerouted
	 * 14 trucks in 8 minutes on a save where nothing was stuck.
	 */
	constexpr float JamMinStandingSeconds = 10.0f;
	/** Extra cost laid on every segment of a new path for each truck rerouted onto it this pass, so the next one does not follow blindly. */
	constexpr float HerdPenaltyPerVehicle = 3000.0f;
	/** A new path has to beat the current one by this share of its cost... */
	constexpr float MinGainShare = 0.2f;
	/** ...and by at least this much (cm). */
	constexpr float MinGainAbsolute = 5000.0f;
	/** A truck closer than this to the end of its segment keeps the next segment too: it may be braking into it already. */
	constexpr float MinDecisionDistance = 3000.0f;
	/** How often the automatic pass runs. */
	constexpr float ReroutePassSeconds = 2.0f;
	/** A truck is not rerouted again for this long. */
	constexpr float RerouteCooldownSeconds = 30.0f;
	/**
	 * A truck waiting this long on a junction's blocks may take another way out of it: one whose
	 * blocks are all free right now, and which is longer than the way it waits for by no more than
	 * WaiterDetourPerSecond for every second it has waited (and WaiterMinDetour at least).
	 *
	 * Measured 18.09.2026 with rerouting off, 51 trucks, 15 minutes, 376 waits: a truck that has
	 * waited t seconds waits about 0.6 t more on average (7 s more after 5 s, 13 s after 20 s,
	 * 19 s after 30 s). At about 10 m/s that makes 6 m per second waited the break-even detour. A
	 * flat 1.5x allowance before that sent trucks that had waited 6 s on 250 m detours.
	 */
	constexpr float WaiterMinSeconds = 5.0f;
	constexpr float WaiterDetourPerSecond = 600.0f;
	constexpr float WaiterMinDetour = 2000.0f;
	/** BJ.Reroute ... avoid: how many segments past the branch are priced as jammed, and how dearly (cm). */
	constexpr int32 AvoidSegments = 3;
	constexpr float AvoidPenalty = 1000000.0f;

	/** On by default since 1.0.4 (the owner's call): an A/B run on a calm save showed no harm. */
	TAutoConsoleVariable<int32> CVarRerouteAuto(
		TEXT("BJ.Reroute.Auto"),
		1,
		TEXT("BetterJunctions: 1 (default) = trucks with standing vehicles ahead on their way, or waiting at a junction, switch to a cheaper path on their own. 0 = only BJ.Reroute apply does it."));

	float GSincePass = 0.0f;
	/** World time each vehicle was first seen standing on the road. Game thread only. */
	TMap<TWeakObjectPtr<const AFGWheeledVehicle>, float> GStandingSince;
	/** World time of the last reroute per truck. Game thread only. */
	TMap<TWeakObjectPtr<const UFGVehicleAutopilotComponent>, float> GLastReroute;

	void RSay(FOutputDevice* Ar, const FString& Line)
	{
		UE_LOG(LogBetterJunctions, Display, TEXT("%s"), *Line);
		if (Ar)
		{
			Ar->Logf(TEXT("%s"), *Line);
		}
	}

	FString RLabel(const UFGVehicleAutopilotComponent* Autopilot)
	{
		const AFGWheeledVehicle* Vehicle = Autopilot ? Cast<AFGWheeledVehicle>(Autopilot->GetOwner()) : nullptr;
		const AFGWheeledVehicleIdentifier* Id = Vehicle ? Vehicle->GetVehicleIdentifier() : nullptr;
		const FString Name = GetNameSafe(Vehicle);
		return FString::Printf(TEXT("%s [%s]"), Id ? *Id->GetVehicleName().ToString() : TEXT("?"), Name.Len() > 10 ? *Name.Right(10) : *Name);
	}

	FGuid EndGuid(const AFGVehiclePathSegment* Segment)
	{
		const AFGVehiclePathNode* End = Segment ? Segment->GetEndNode() : nullptr;
		return End ? End->GetPathNodeGUID() : FGuid();
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GRerouteCommand(
		TEXT("BJ.Reroute"),
		TEXT("BetterJunctions: BJ.Reroute [filter] [apply] [avoid] compares the rest of each truck's leg with the best path around standing vehicles; 'apply' switches where it pays off, 'avoid' (test aid) prices the next three segments as jammed."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			FString Filter;
			bool bApply = false;
			bool bAvoid = false;
			for (const FString& Arg : Args)
			{
				if (Arg.Equals(TEXT("apply"), ESearchCase::IgnoreCase))
				{
					bApply = true;
				}
				else if (Arg.Equals(TEXT("avoid"), ESearchCase::IgnoreCase))
				{
					bAvoid = true;
				}
				else
				{
					Filter = Arg;
				}
			}
			const int32 Rerouted = FBetterJunctionsHooks::RerouteVehicles(World, Filter, bApply, bAvoid, &Ar);
			RSay(&Ar, FString::Printf(TEXT("BJ.Reroute: %s %d truck(s)"), bApply ? TEXT("rerouted") : TEXT("would reroute"), Rerouted));
		}));
}

/**
 * What every truck of one pass shares. Built lazily: a pass in which no truck has a jam ahead
 * and none waits at a junction builds nothing at all. Before this each truck rebuilt the
 * segment table (some 1650 actors) and its network's adjacency on every pass, jam or not.
 */
struct FBJReroutePass
{
	struct FNetwork
	{
		/** Segment actor per index of the network's segment data. */
		TArray<AFGVehiclePathSegment*> Actors;
		/** Traversable segments leaving each node, per vehicle type. */
		TMap<const UFGVehiclePathPreset*, TArray<TArray<int32>>> Leaving;
	};

	/** AllSegments is the subsystem's protected segment set, handed over by the friend hooks class. */
	explicit FBJReroutePass(const TSet<TObjectPtr<AFGVehiclePathSegment>>& InAllSegments) : AllSegments(InAllSegments) {}

	AFGVehiclePathSegment* FindSegment(const FGuid& From, const FGuid& To)
	{
		if (!bSegmentsBuilt)
		{
			bSegmentsBuilt = true;
			for (AFGVehiclePathSegment* Segment : AllSegments)
			{
				if (IsValid(Segment))
				{
					SegmentByNodes.Add({Segment->GetStartPathNodeGuid(), EndGuid(Segment)}, Segment);
				}
			}
		}
		AFGVehiclePathSegment* const* Found = SegmentByNodes.Find({From, To});
		return Found ? *Found : nullptr;
	}

	const TArray<AFGVehiclePathSegment*>& Actors(const UFGVehiclePathNetwork* Network)
	{
		return Get(Network).Actors;
	}

	const TArray<TArray<int32>>& Leaving(const UFGVehiclePathNetwork* Network, const UFGVehiclePathPreset* Preset)
	{
		FNetwork& Entry = Get(Network);
		if (const TArray<TArray<int32>>* Found = Entry.Leaving.Find(Preset))
		{
			return *Found;
		}
		const TArray<FVehiclePathNetworkNodeData>& Nodes = Network->GetPathNodes();
		const TArray<FVehiclePathNetworkSegmentData>& Data = Network->GetPathSegments();
		TArray<TArray<int32>>& Out = Entry.Leaving.Add(Preset);
		Out.SetNum(Nodes.Num());
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			if (Nodes.IsValidIndex(Data[Index].FromNodeIndex) && Nodes.IsValidIndex(Data[Index].ToNodeIndex) && Network->CanVehicleTraverseSegment(Data[Index], Preset))
			{
				Out[Data[Index].FromNodeIndex].Add(Index);
			}
		}
		return Out;
	}

private:
	FNetwork& Get(const UFGVehiclePathNetwork* Network)
	{
		if (FNetwork* Found = Networks.Find(Network))
		{
			return *Found;
		}
		FNetwork& Entry = Networks.Add(Network);
		const TArray<FVehiclePathNetworkNodeData>& Nodes = Network->GetPathNodes();
		const TArray<FVehiclePathNetworkSegmentData>& Data = Network->GetPathSegments();
		Entry.Actors.SetNumZeroed(Data.Num());
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			if (Nodes.IsValidIndex(Data[Index].FromNodeIndex) && Nodes.IsValidIndex(Data[Index].ToNodeIndex))
			{
				Entry.Actors[Index] = FindSegment(Nodes[Data[Index].FromNodeIndex].PathNodeGuid, Nodes[Data[Index].ToNodeIndex].PathNodeGuid);
			}
		}
		return Entry;
	}

	const TSet<TObjectPtr<AFGVehiclePathSegment>>& AllSegments;
	bool bSegmentsBuilt = false;
	TMap<TPair<FGuid, FGuid>, AFGVehiclePathSegment*> SegmentByNodes;
	TMap<const UFGVehiclePathNetwork*, FNetwork> Networks;
};

void FBetterJunctionsHooks::BuildJamPenalties(AFGVehicleSubsystem* Subsystem, TMap<const AFGVehiclePathSegment*, float>& OutPenalties)
{
	OutPenalties.Reset();
	const float Now = Subsystem->GetWorld()->GetTimeSeconds();
	TSet<TWeakObjectPtr<const AFGWheeledVehicle>> StandingNow;
	for (const AFGVehiclePathSegment* Segment : Subsystem->mAllPathSegments)
	{
		if (!IsValid(Segment))
		{
			continue;
		}
		int32 Standing = 0;
		for (const AFGWheeledVehicle* Vehicle : Segment->GetVehicles())
		{
			if (!IsValid(Vehicle))
			{
				continue;
			}
			// A truck at a station is not in anybody's way on the road.
			const AFGWheeledVehicleIdentifier* Id = Vehicle->GetVehicleIdentifier();
			if (Id && Id->IsCurrentlyDocking())
			{
				continue;
			}
			// A parked vehicle without autopilot is an obstacle for good; one on autopilot counts while it stands.
			const UFGVehicleAutopilotComponent* Autopilot = Vehicle->GetVehicleAutopilotComponent();
			const bool bOnAutopilot = Id && Id->IsAutopilotEnabled();
			if (!bOnAutopilot || !IsValid(Autopilot) || FMath::Abs(Autopilot->GetCurrentForwardSpeed()) < JamStandingSpeed)
			{
				StandingNow.Add(Vehicle);
				if (Now - GStandingSince.FindOrAdd(Vehicle, Now) >= JamMinStandingSeconds)
				{
					++Standing;
				}
			}
		}
		if (Standing > 0)
		{
			OutPenalties.Add(Segment, Standing * JamPenaltyPerVehicle);
		}
	}
	// Whoever moved (or left the road) starts over.
	for (auto It = GStandingSince.CreateIterator(); It; ++It)
	{
		if (!StandingNow.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

bool FBetterJunctionsHooks::TryReroute(FBJReroutePass& Pass, AFGVehicleSubsystem* Subsystem, UFGVehicleAutopilotComponent* Autopilot,
	TMap<const AFGVehiclePathSegment*, float>& Penalties, bool bApply, bool bAvoid, bool bVerbose, FOutputDevice* Ar)
{
	const auto Skip = [&](const FString& Why)
	{
		if (bVerbose)
		{
			RSay(Ar, FString::Printf(TEXT("  %s: %s"), *RLabel(Autopilot), *Why));
		}
		return false;
	};

	const AFGWheeledVehicle* Vehicle = Cast<AFGWheeledVehicle>(Autopilot->GetOwner());
	const AFGWheeledVehicleIdentifier* Id = Vehicle ? Vehicle->GetVehicleIdentifier() : nullptr;
	if (!Id || Id->IsCurrentlyDocking())
	{
		return Skip(TEXT("docking"));
	}
	TArray<FGuid>& Route = Autopilot->mCurrentVehicleRoute;
	TArray<TObjectPtr<AFGVehiclePathSegment>>& Segments = Autopilot->mCurrentVehicleRouteSegments;
	if (Route.Num() < 2 || Segments.Num() != Route.Num())
	{
		return Skip(FString::Printf(TEXT("route layout: %d node(s), %d segment(s)"), Route.Num(), Segments.Num()));
	}

	// Where the truck is: segment k runs from node k - 1 to node k.
	const int32 Current = Segments.IndexOfByKey(Autopilot->mCurrentServerPathSegment);
	if (Current == INDEX_NONE)
	{
		return Skip(FString::Printf(TEXT("current segment %s not in route (target node %d)"),
			*GetNameSafe(Autopilot->mCurrentServerPathSegment), Autopilot->mCurrentPathNodeIndex));
	}
	const AFGVehiclePathSegment* CurrentSegment = Segments[Current];
	if (EndGuid(CurrentSegment) != Route[Current] || (Current > 0 && CurrentSegment->GetStartPathNodeGuid() != Route[Current - 1]))
	{
		return Skip(FString::Printf(TEXT("route layout: segment %d does not end at node %d"), Current, Current));
	}
	// A truck standing at a junction waiting for its blocks: the junction segments of the sequence it
	// waits for, to be routed around.
	const bool bWaiter = IsWaitingOnBlock(Autopilot) && Autopilot->mTimeSpentWaitingOnCurrentFreeBlock >= WaiterMinSeconds;
	TSet<const AFGVehiclePathSegment*> Banned;
	FVehicleAutopilotBlockReference CurrentBlock;
	float DistanceToEndOfBlock = 0.0f;
	const int32 NodeOffset = Autopilot->FindPathBlockFromVehiclePosition(Autopilot->mCurrentServerVehicleSplinePosition, CurrentBlock, DistanceToEndOfBlock)
		? NodeIndexOffset(Autopilot, CurrentBlock) : INDEX_NONE;
	if (bWaiter)
	{
		for (const FVehicleAutopilotBlockReference& Ref : Autopilot->mNextBlockSequenceReference.GetValue())
		{
			const AFGVehiclePathSegment* Segment = ResolveSegment(Autopilot, Ref, NodeOffset);
			if (Segment && Segment->IsJunctionBlock())
			{
				Banned.Add(Segment);
			}
		}
		if (Banned.Num() == 0)
		{
			return Skip(TEXT("waiting, but the awaited junction is not resolved"));
		}
	}

	// The node the new path branches off from; everything up to it stays. It lies past every
	// segment the truck holds a reservation on, so nothing booked is ever given up: a truck inside
	// a junction or braking into one keeps the blocks that let it through, exit included. A truck
	// already standing at the stop line is not braking and may branch right at the junction entry.
	const USplineComponent* Spline = CurrentSegment->GetSplineComponent();
	const float Remaining = Spline ? Spline->GetSplineLength() - Autopilot->mCurrentServerVehicleSplinePosition : 0.0f;
	int32 Branch = !bWaiter && Remaining < MinDecisionDistance ? Current + 1 : Current;
	for (const auto& Pair : Autopilot->mPathBlockReservations)
	{
		Branch = FMath::Max(Branch, Segments.IndexOfByKey(Pair.Key.Segment));
	}
	if (Branch >= Route.Num() - 1)
	{
		return Skip(TEXT("no fork left before the station"));
	}
	for (int32 Index = 0; Index <= Branch; ++Index)
	{
		if (Banned.Contains(Segments[Index].Get()))
		{
			return Skip(TEXT("waiting on a junction it cannot leave the route before"));
		}
	}

	// Test aid (BJ.Reroute ... avoid): price the next few segments as if a queue stood on them.
	TMap<const AFGVehiclePathSegment*, float> Avoided;
	if (bAvoid)
	{
		Avoided = Penalties;
		for (int32 Index = Branch + 1; Index <= FMath::Min(Branch + AvoidSegments, Route.Num() - 1); ++Index)
		{
			Avoided.FindOrAdd(Segments[Index].Get()) += AvoidPenalty;
		}
	}
	const TMap<const AFGVehiclePathSegment*, float>& Prices = bAvoid ? Avoided : Penalties;

	// The cheap test first: nothing to do for a truck with no jam on the rest of its way.
	bool bJamAhead = false;
	for (int32 Index = Branch + 1; Index < Route.Num() && !bJamAhead; ++Index)
	{
		bJamAhead = Prices.Contains(Segments[Index].Get());
	}
	if (!bJamAhead && !bWaiter)
	{
		return Skip(TEXT("no standing vehicle ahead"));
	}

	UFGVehiclePathNetwork* Network = Subsystem->FindNetworkByID(Autopilot->mCurrentPathNetworkID);
	if (!Network)
	{
		return Skip(FString::Printf(TEXT("network %d not found"), Autopilot->mCurrentPathNetworkID));
	}
	const TArray<FVehiclePathNetworkNodeData>& Nodes = Network->GetPathNodes();
	const TArray<FVehiclePathNetworkSegmentData>& SegmentData = Network->GetPathSegments();
	const int32 From = Network->FindPathNodeIndexByGuid(Route[Branch]);
	const int32 To = Network->FindPathNodeIndexByGuid(Route.Last());
	if (From == INDEX_NONE || To == INDEX_NONE)
	{
		return Skip(TEXT("route nodes not in the network"));
	}
	const UFGVehiclePathPreset* Preset = Vehicle->GetVehiclePathPreset();
	const TArray<TArray<int32>>& Leaving = Pass.Leaving(Network, Preset);
	const TArray<AFGVehiclePathSegment*>& Actors = Pass.Actors(Network);
	const auto Cost = [&](int32 SegmentIndex)
	{
		const float* Penalty = Actors[SegmentIndex] ? Prices.Find(Actors[SegmentIndex]) : nullptr;
		return SegmentData[SegmentIndex].SegmentLength + (Penalty ? *Penalty : 0.0f);
	};

	// The rest of the current leg at today's prices.
	float CurrentCost = 0.0f;
	float CurrentLength = 0.0f;
	for (int32 Index = Branch + 1; Index < Route.Num(); ++Index)
	{
		const int32 SegmentIndex = Network->FindPathIndexBetweenPathNodes(Network->FindPathNodeIndexByGuid(Route[Index - 1]), Network->FindPathNodeIndexByGuid(Route[Index]));
		if (SegmentIndex == INDEX_NONE)
		{
			return Skip(FString::Printf(TEXT("route node %d has no segment in the network"), Index));
		}
		CurrentCost += Cost(SegmentIndex);
		CurrentLength += SegmentData[SegmentIndex].SegmentLength;
	}

	// Dijkstra over the same network with the same traversability rule as the game.
	TArray<float> Distance;
	Distance.Init(TNumericLimits<float>::Max(), Nodes.Num());
	TArray<int32> Via;
	Via.Init(INDEX_NONE, Nodes.Num());
	struct FOpen
	{
		float Distance;
		int32 Node;
		bool operator<(const FOpen& Other) const { return Distance < Other.Distance; }
	};
	TArray<FOpen> Open;
	Distance[From] = 0.0f;
	Open.HeapPush({0.0f, From});
	while (Open.Num() > 0)
	{
		FOpen Top;
		Open.HeapPop(Top);
		if (Top.Node == To)
		{
			break;
		}
		if (Top.Distance > Distance[Top.Node])
		{
			continue;
		}
		for (const int32 SegmentIndex : Leaving[Top.Node])
		{
			if (Banned.Num() > 0 && Banned.Contains(Actors[SegmentIndex]))
			{
				continue;
			}
			const int32 Next = SegmentData[SegmentIndex].ToNodeIndex;
			const float Candidate = Top.Distance + Cost(SegmentIndex);
			if (Candidate < Distance[Next])
			{
				Distance[Next] = Candidate;
				Via[Next] = SegmentIndex;
				Open.HeapPush({Candidate, Next});
			}
		}
	}
	if (Via[To] == INDEX_NONE)
	{
		return Skip(bWaiter ? TEXT("waiting, no other way out of the junction to the station") : TEXT("station unreachable"));
	}
	TArray<FGuid> Tail;
	float NewLength = 0.0f;
	for (int32 Node = To; Node != From; Node = SegmentData[Via[Node]].FromNodeIndex)
	{
		Tail.Add(Nodes[Node].PathNodeGuid);
		NewLength += SegmentData[Via[Node]].SegmentLength;
	}
	Algo::Reverse(Tail);

	TArray<AFGVehiclePathSegment*> TailSegments;
	FGuid Previous = Route[Branch];
	for (const FGuid& Node : Tail)
	{
		AFGVehiclePathSegment* Actor = Pass.FindSegment(Previous, Node);
		if (!Actor)
		{
			return Skip(TEXT("no segment actor on the new path"));
		}
		TailSegments.Add(Actor);
		Previous = Node;
	}

	// A waiter only moves if the way it switches to can be booked now: every junction segment the
	// new path starts with, and the first block past them.
	bool bWayFree = true;
	if (bWaiter)
	{
		for (int32 Index = 0; Index < TailSegments.Num(); ++Index)
		{
			const bool bJunction = TailSegments[Index]->IsJunctionBlock();
			if (bJunction || Index > 0)
			{
				bWayFree &= IsSegmentFreeFor(TailSegments[Index], Vehicle, !bJunction);
			}
			if (!bJunction || !bWayFree)
			{
				break;
			}
		}
	}

	const float NewCost = Distance[To];
	const bool bSame = Tail.Num() == Route.Num() - Branch - 1 && FMemory::Memcmp(Tail.GetData(), Route.GetData() + Branch + 1, Tail.Num() * sizeof(FGuid)) == 0;
	const bool bShortEnough = NewLength - CurrentLength <= FMath::Max(WaiterMinDetour, Autopilot->mTimeSpentWaitingOnCurrentFreeBlock * WaiterDetourPerSecond);
	const bool bPays = !bSame && (bWaiter
		? bWayFree && bShortEnough
		: NewCost < CurrentCost * (1.0f - MinGainShare) && CurrentCost - NewCost > MinGainAbsolute);
	const TCHAR* Outcome = bSame ? TEXT("same path")
		: bPays ? (bApply ? TEXT("REROUTED") : TEXT("would reroute"))
		: !bWaiter ? TEXT("not worth it")
		: !bWayFree ? TEXT("the other way is busy too")
		: TEXT("the other way is too long");
	const FString Verdict = FString::Printf(TEXT("  %s: %srest of leg %.0f m road / %.0f m priced, best %.0f m road / %.0f m priced over %d node(s), %s"),
		*RLabel(Autopilot), bWaiter ? *FString::Printf(TEXT("waiting %.0f s at a junction, "), Autopilot->mTimeSpentWaitingOnCurrentFreeBlock) : TEXT(""),
		CurrentLength / 100.0f, CurrentCost / 100.0f, NewLength / 100.0f, NewCost / 100.0f, Tail.Num(), Outcome);
	if (!bPays)
	{
		if (bVerbose)
		{
			RSay(Ar, Verdict);
		}
		return false;
	}
	RSay(Ar, Verdict);
	if (!bApply)
	{
		return true;
	}

	// No reservation lies past the branch; a pending sequence may, and is recomputed next tick.
	// The free block is the one the truck parks on; it is kept unless it lies on the replaced tail.
	Autopilot->mNextBlockSequenceReference.Reset();
	if (Autopilot->mNextFreeBlockReference.IsSet()
		&& (NodeOffset == INDEX_NONE || Autopilot->mNextFreeBlockReference.GetValue().PathNodeIndex + NodeOffset > Branch))
	{
		Autopilot->mNextFreeBlockReference.Reset();
	}

	Route.SetNum(Branch + 1);
	Route.Append(Tail);
	Segments.SetNum(Branch + 1);
	for (AFGVehiclePathSegment* Segment : TailSegments)
	{
		Segments.Add(Segment);
	}
	Autopilot->UpdateNextVehiclePathSegment_ThreadSafe();

	// Herding: the next truck sees this path a little dearer.
	for (const AFGVehiclePathSegment* Segment : TailSegments)
	{
		Penalties.FindOrAdd(Segment) += HerdPenaltyPerVehicle;
	}
	GLastReroute.Add(Autopilot, Subsystem->GetWorld()->GetTimeSeconds());
	return true;
}

int32 FBetterJunctionsHooks::RerouteVehicles(UWorld* World, const FString& Filter, bool bApply, bool bAvoid, FOutputDevice* Ar)
{
	AFGVehicleSubsystem* Subsystem = AFGVehicleSubsystem::Get(World);
	if (!IsValid(Subsystem) || Subsystem->GetNetMode() == NM_Client)
	{
		RSay(Ar, TEXT("BJ.Reroute: only on the server or the host"));
		return 0;
	}
	TMap<const AFGVehiclePathSegment*, float> Penalties;
	BuildJamPenalties(Subsystem, Penalties);
	RSay(Ar, FString::Printf(TEXT("BJ.Reroute: %d segment(s) with standing vehicles"), Penalties.Num()));
	FBJReroutePass Pass(Subsystem->mAllPathSegments);
	int32 Rerouted = 0;
	for (AFGWheeledVehicleIdentifier* Id : Subsystem->GetAllVehicles())
	{
		if (!IsValid(Id) || !Id->IsAutopilotEnabled() || (!Filter.IsEmpty() && !Id->GetVehicleName().ToString().Contains(Filter)))
		{
			continue;
		}
		const AFGWheeledVehicle* Vehicle = Id->GetOwnerVehicle();
		UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
		if (IsValid(Autopilot) && TryReroute(Pass, Subsystem, Autopilot, Penalties, bApply, bAvoid, true, Ar))
		{
			++Rerouted;
		}
	}
	return Rerouted;
}

void FBetterJunctionsHooks::TickReroute(AFGVehicleSubsystem* Subsystem, float DeltaTime)
{
	GSincePass += DeltaTime;
	if (GSincePass < ReroutePassSeconds)
	{
		return;
	}
	GSincePass = 0.0f;

	// Runs with the automatic mode off too: it keeps the standing timers BJ.Reroute relies on.
	TMap<const AFGVehiclePathSegment*, float> Penalties;
	BuildJamPenalties(Subsystem, Penalties);
	if (CVarRerouteAuto.GetValueOnGameThread() == 0)
	{
		return;
	}
	const float Now = Subsystem->GetWorld()->GetTimeSeconds();
	for (auto It = GLastReroute.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || Now - It.Value() > RerouteCooldownSeconds)
		{
			It.RemoveCurrent();
		}
	}
	FBJReroutePass Pass(Subsystem->mAllPathSegments);
	for (AFGWheeledVehicleIdentifier* Id : Subsystem->GetAllVehicles())
	{
		const AFGWheeledVehicle* Vehicle = IsValid(Id) && Id->IsAutopilotEnabled() ? Id->GetOwnerVehicle() : nullptr;
		UFGVehicleAutopilotComponent* Autopilot = IsValid(Vehicle) ? Vehicle->GetVehicleAutopilotComponent() : nullptr;
		if (IsValid(Autopilot) && !GLastReroute.Contains(Autopilot))
		{
			TryReroute(Pass, Subsystem, Autopilot, Penalties, true, false, false, nullptr);
		}
	}
}

bool FBetterJunctionsHooks::IsSegmentFreeFor(const AFGVehiclePathSegment* Segment, const AFGWheeledVehicle* Self, bool bFirstBlockOnly)
{
	// Anyone inside a junction segment holds its block; past a junction only the first block matters.
	if (!bFirstBlockOnly)
	{
		for (const AFGWheeledVehicle* Other : Segment->GetVehicles())
		{
			if (IsValid(Other) && Other != Self)
			{
				return false;
			}
		}
	}
	TArray<FVehiclePathBlockReference> Overlapping;
	{
		FReadScopeLock Lock(Segment->mPathBlocksLock);
		const TArray<FVehiclePathBlock>& Blocks = Segment->GetVehiclePathBlocks();
		const int32 Count = bFirstBlockOnly ? FMath::Min(1, Blocks.Num()) : Blocks.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVehiclePathBlock& Block = Blocks[Index];
			for (const auto& Exclusive : Block.ExclusiveReservations)
			{
				if (Exclusive.IsValid() && Exclusive->OwnerVehicle.Get() != Self)
				{
					return false;
				}
			}
			// A shared lock here is somebody's exclusive on an overlapping block.
			for (const auto& Shared : Block.SharedReservations)
			{
				const TSharedPtr<FVehiclePathBlockExclusiveReservation> Owner = Shared.IsValid() ? Shared->OwnerReservation.Pin() : nullptr;
				if (Owner.IsValid() && Owner->OwnerVehicle.Get() != Self)
				{
					return false;
				}
			}
			Overlapping.Append(Block.OverlappingBlocks);
		}
	}
	for (const FVehiclePathBlockReference& Over : Overlapping)
	{
		if (!IsValid(Over.Segment))
		{
			continue;
		}
		FReadScopeLock Lock(Over.Segment->mPathBlocksLock);
		const TArray<FVehiclePathBlock>& Blocks = Over.Segment->GetVehiclePathBlocks();
		if (!Blocks.IsValidIndex(Over.PathBlockIndex))
		{
			continue;
		}
		for (const auto& Exclusive : Blocks[Over.PathBlockIndex].ExclusiveReservations)
		{
			if (Exclusive.IsValid() && Exclusive->OwnerVehicle.Get() != Self)
			{
				return false;
			}
		}
	}
	return true;
}
