#include "StreetSpawner.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "LocomotionComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "ThrowerEnemy.h"
#include "WalkingEnemy.h"

DEFINE_LOG_CATEGORY_STATIC(LogStreetSpawner, Log, All);

namespace
{
	/** Spawn points are on the ground; the walker's capsule centre sits this far above it. */
	constexpr float SpawnHeightAboveGround = 95.f;
	/** Line-of-sight checks aim at roughly chest height on the spawn point. */
	constexpr float SightCheckHeight = 120.f;
}

AStreetSpawner::AStreetSpawner()
{
	PrimaryActorTick.bCanEverTick = true;

	LeaderboardMode = TEXT("streets");
	ModeName = TEXT("STREETS");
	bAllowGun = true;
	EnemyClass = AWalkingEnemy::StaticClass();
	bAutoStart = false;

	// Walkers come from cover, start just under the player's walk speed (220) and end up outrunning them.
	InitialSpawnInterval = 2.6f;
	MinSpawnInterval = 1.0f;
	IntervalMultiplier = 0.94f;
	InitialMaxAlive = 3;
	MaxAliveCap = 7;
	KillsPerExtraEnemy = 4;
	InitialEnemySpeed = 200.f;
	SpeedIncreasePerKill = 8.f;
	MaxEnemySpeed = 340.f;
	SpawnAheadMin = 1000.f;
	MinSpawnDistance = 800.f;

	static ConstructorHelpers::FClassFinder<AActor> Bat(TEXT("/Game/Cyclops/Blueprints/BP_FlyingEnemy"));
	BatClass = Bat.Class;
	ThrowerClass = AThrowerEnemy::StaticClass();
}

TSubclassOf<AActor> AStreetSpawner::PickEnemyClass()
{
	if (BatClass && Kills >= BatMinKills && CountAliveOfClass(BatClass) < MaxBats && FMath::FRand() < BatChance)
	{
		return BatClass;
	}
	if (ThrowerClass && Kills >= ThrowerMinKills && CountAliveOfClass(ThrowerClass) < MaxThrowers && FMath::FRand() < ThrowerChance)
	{
		return ThrowerClass;
	}
	return EnemyClass;
}

float AStreetSpawner::GetSpawnSpeed(TSubclassOf<AActor> Class) const
{
	if (IsBat(Class))
	{
		return FMath::Min(MaxBatSpeed, BatSpeed + BatSpeedPerKill * Kills);
	}
	const bool bThrower = ThrowerClass && Class && Class->IsChildOf(ThrowerClass);
	return GetCurrentEnemySpeed() * (bThrower ? ThrowerSpeedScale : 1.f);
}

FVector AStreetSpawner::RoutePointAt(float Distance) const
{
	if (Route.Num() < 2)
	{
		return GetActorLocation();
	}
	Distance = FMath::Clamp(Distance, 0.f, RouteLength());
	for (int32 i = 1; i < Route.Num(); ++i)
	{
		if (Distance <= RouteCumulative[i] || i == Route.Num() - 1)
		{
			const float Segment = RouteCumulative[i] - RouteCumulative[i - 1];
			const float T = Segment > KINDA_SMALL_NUMBER ? (Distance - RouteCumulative[i - 1]) / Segment : 0.f;
			return FMath::Lerp(Route[i - 1], Route[i], FMath::Clamp(T, 0.f, 1.f));
		}
	}
	return Route.Last();
}

void AStreetSpawner::BeginPlay()
{
	Super::BeginPlay();
	GatherMarkers();
}

void AStreetSpawner::GatherMarkers()
{
	TArray<AActor*> RouteActors;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->ActorHasTag(RoutePointTag))
		{
			RouteActors.Add(Actor);
		}
		else if (Actor->ActorHasTag(SpawnPointTag))
		{
			SpawnPoints.Add({ Actor->GetActorLocation() });
		}
		else if (Actor->ActorHasTag(ExitTag))
		{
			ExitLocation = Actor->GetActorLocation();
			bHasExit = true;
		}
	}

	// Route: this actor (the start), then the tagged points chained nearest-first (fine for any street that doesn't
	// loop back on itself, and needs no naming convention), then the exit.
	Route.Add(GetActorLocation());
	TArray<FVector> Remaining;
	for (const AActor* Actor : RouteActors)
	{
		Remaining.Add(Actor->GetActorLocation());
	}
	while (Remaining.Num())
	{
		int32 Nearest = 0;
		for (int32 i = 1; i < Remaining.Num(); ++i)
		{
			if (FVector::DistSquared2D(Remaining[i], Route.Last()) < FVector::DistSquared2D(Remaining[Nearest], Route.Last()))
			{
				Nearest = i;
			}
		}
		Route.Add(Remaining[Nearest]);
		Remaining.RemoveAtSwap(Nearest);
	}
	if (bHasExit)
	{
		Route.Add(ExitLocation);
	}

	RouteCumulative.Add(0.f);
	for (int32 i = 1; i < Route.Num(); ++i)
	{
		RouteCumulative.Add(RouteCumulative.Last() + FVector::Dist2D(Route[i - 1], Route[i]));
	}

	for (FSpawnPoint& Point : SpawnPoints)
	{
		Point.RouteDistance = RouteDistanceOf(Point.Location);
	}

	UE_LOG(LogStreetSpawner, Log, TEXT("%s: route %d points (%.0f m), %d spawn points, exit %s"),
		*GetName(), Route.Num(), RouteLength() / 100.f, SpawnPoints.Num(), bHasExit ? TEXT("found") : TEXT("MISSING"));
}

float AStreetSpawner::RouteDistanceOf(const FVector& Location) const
{
	float Best = 0.f;
	float BestDistSq = TNumericLimits<float>::Max();
	const FVector2D P(Location);
	for (int32 i = 1; i < Route.Num(); ++i)
	{
		const FVector2D A(Route[i - 1]);
		const FVector2D B(Route[i]);
		const FVector2D AB = B - A;
		const float LenSq = AB.SizeSquared();
		const float T = LenSq > KINDA_SMALL_NUMBER ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.f, 1.f) : 0.f;
		const float DistSq = FVector2D::DistSquared(P, A + AB * T);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = RouteCumulative[i - 1] + FMath::Sqrt(LenSq) * T;
		}
	}
	return Best;
}

float AStreetSpawner::GetProgress() const
{
	FVector Head;
	const float Length = RouteLength();
	return Length > 0.f && GetPlayerHead(Head) ? FMath::Clamp(RouteDistanceOf(Head) / Length, 0.f, 1.f) : 0.f;
}

int32 AStreetSpawner::GetCurrentMaxAlive() const
{
	return FMath::Min(MaxAliveCap, Super::GetCurrentMaxAlive() + FMath::FloorToInt(GetProgress() * ExtraMaxAliveAtExit));
}

bool AStreetSpawner::PickSpawnLocation(TSubclassOf<AActor> Class, FVector& OutLocation, FRotator& OutRotation)
{
	return IsBat(Class) ? PickBatLocation(OutLocation, OutRotation) : PickWalkerLocation(OutLocation, OutRotation);
}

bool AStreetSpawner::PickBatLocation(FVector& OutLocation, FRotator& OutRotation) const
{
	FVector Head;
	if (!GetPlayerHead(Head) || Route.Num() < 2)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(StreetBatSight), false);
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		Params.AddIgnoredActor(PC->GetPawn());
	}

	// Above the street ahead, nearest-visible last: bats fly straight at the head, so they need a clear line or
	// they'd come through the buildings. Round a corner there may be no such spot; then skip this spawn.
	const float PlayerDistance = RouteDistanceOf(Head);
	for (const float Ahead : { 2600.f, 2100.f, 1600.f })
	{
		const FVector Ground = RoutePointAt(PlayerDistance + Ahead);
		const FVector Candidate = Ground + FVector(FMath::FRandRange(-250.f, 250.f), FMath::FRandRange(-250.f, 250.f), FMath::FRandRange(BatHeight.X, BatHeight.Y));
		if (FVector::Dist2D(Candidate, Head) < MinSpawnDistance)
		{
			continue;
		}
		if (!GetWorld()->LineTraceTestByObjectType(Head, Candidate, FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			OutLocation = Candidate;
			OutRotation = FRotator(0.f, (Head - Candidate).Rotation().Yaw, 0.f);
			return true;
		}
	}
	return false;
}

bool AStreetSpawner::PickWalkerLocation(FVector& OutLocation, FRotator& OutRotation) const
{
	FVector Head;
	if (!GetPlayerHead(Head) || SpawnPoints.IsEmpty())
	{
		return false;
	}
	const float PlayerDistance = RouteDistanceOf(Head);

	// Prefer points the player can't currently see (round corners, inside doorways) - that's the surprise.
	TArray<const FSpawnPoint*> Hidden;
	TArray<const FSpawnPoint*> Visible;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(StreetSpawnSight), false);
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		Params.AddIgnoredActor(PC->GetPawn());
	}
	for (const FSpawnPoint& Point : SpawnPoints)
	{
		const float Ahead = Point.RouteDistance - PlayerDistance;
		if (Ahead < SpawnAheadMin || Ahead > SpawnAheadMax || FVector::Dist2D(Point.Location, Head) < MinSpawnDistance)
		{
			continue;
		}
		const bool bBlocked = GetWorld()->LineTraceTestByObjectType(Head, Point.Location + FVector(0.0, 0.0, SightCheckHeight),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params);
		(bBlocked ? Hidden : Visible).Add(&Point);
	}

	const TArray<const FSpawnPoint*>& Pool = Hidden.Num() ? Hidden : Visible;
	if (Pool.IsEmpty())
	{
		return false;
	}
	const FSpawnPoint* Chosen = Pool[FMath::RandRange(0, Pool.Num() - 1)];
	OutLocation = Chosen->Location + FVector(0.0, 0.0, SpawnHeightAboveGround);
	OutRotation = FRotator(0.f, (Head - Chosen->Location).Rotation().Yaw, 0.f);
	return true;
}

void AStreetSpawner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The pawn may not exist yet at BeginPlay; attach walking controls as soon as it does.
	if (!Locomotion.IsValid())
	{
		EnsureLocomotion();
		UpdateLocomotion();
	}

	FVector Head;
	if (bRunActive && !bGamePaused && bHasExit && GetPlayerHead(Head) && FVector::Dist2D(Head, ExitLocation) <= ExitRadius)
	{
		const float UnderPar = FMath::Max(0.f, ParTime - GetRunDuration());
		const int32 Bonus = FinishBonusBase + FinishBonusPerLife * Lives + FMath::FloorToInt(UnderPar) * FinishBonusPerSecondUnderPar;
		CompleteRun(Bonus);
	}
}

ULocomotionComponent* AStreetSpawner::EnsureLocomotion()
{
	if (Locomotion.IsValid())
	{
		return Locomotion.Get();
	}
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return nullptr;
	}
	ULocomotionComponent* Component = Pawn->FindComponentByClass<ULocomotionComponent>();
	if (!Component)
	{
		Component = NewObject<ULocomotionComponent>(Pawn, TEXT("StreetLocomotion"));
		Component->RegisterComponent(); // begins play straight away (the pawn already has)
	}
	Locomotion = Component;
	return Component;
}

void AStreetSpawner::UpdateLocomotion()
{
	if (ULocomotionComponent* Component = Locomotion.Get())
	{
		Component->SetMovementEnabled(bRunActive && !bGamePaused);
	}
}

void AStreetSpawner::PlacePlayerAtStart()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Head = PC->PlayerCameraManager->GetCameraLocation();
	const float HeadYaw = PC->PlayerCameraManager->GetCameraRotation().Yaw;

	// Turn the play space so the player faces down the street, then move it so the head is over the start.
	const FRotator Turn(0.f, GetActorRotation().Yaw - HeadYaw, 0.f);
	FVector PawnLocation = Head + Turn.RotateVector(Pawn->GetActorLocation() - Head);
	PawnLocation += FVector(GetActorLocation().X - Head.X, GetActorLocation().Y - Head.Y, 0.0);
	PawnLocation.Z = GetActorLocation().Z; // play-space origin is the floor
	Pawn->SetActorLocationAndRotation(PawnLocation, Pawn->GetActorRotation() + Turn);
}

void AStreetSpawner::OnRunStarted()
{
	EnsureLocomotion();
	PlacePlayerAtStart();
	UpdateLocomotion();
}

void AStreetSpawner::OnRunStopped()
{
	UpdateLocomotion();
}

void AStreetSpawner::OnPausedChanged(bool bPaused)
{
	UpdateLocomotion();
}
