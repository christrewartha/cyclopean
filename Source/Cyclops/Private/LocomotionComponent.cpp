#include "LocomotionComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "CollisionQueryParams.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogLocomotion, Log, All); // "log LogLocomotion Verbose" (or VeryVerbose) to trace input/moves

namespace
{
	// Above the template's contexts (0) and the optic blast's (1).
	constexpr int32 LocomotionContextPriority = 2;
	constexpr float BodyHalfHeight = 60.f;
	constexpr float MaxStepDown = 60.f;      // lowest floor change accepted in one move (stepping off a kerb)
	constexpr float MinFloorNormalZ = 0.7f; // ~45 degree slopes
	constexpr float WallSkin = 0.5f;        // gap left between the body and a wall after a blocked move / push-out
	constexpr int32 MaxPushIterations = 4;  // corners need one pass per wall

	/** Level geometry only: walls, cars, ground. Moving things (menu buttons, beams, props) never block or lift. */
	FCollisionObjectQueryParams BlockingObjects()
	{
		return FCollisionObjectQueryParams(ECC_WorldStatic);
	}
}

ULocomotionComponent::ULocomotionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the HMD has updated the camera this frame.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> Context(TEXT("/Game/Cyclops/Input/IMC_Locomotion.IMC_Locomotion"));
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> Template(TEXT("/Game/XRFramework/Input/IMC_Default.IMC_Default"));
	static ConstructorHelpers::FObjectFinder<UInputAction> Move(TEXT("/Game/Cyclops/Input/IA_LocoMove.IA_LocoMove"));
	static ConstructorHelpers::FObjectFinder<UInputAction> Turn(TEXT("/Game/Cyclops/Input/IA_LocoTurn.IA_LocoTurn"));
	LocomotionContext = Context.Object;
	SuppressedContext = Template.Object;
	MoveAction = Move.Object;
	TurnAction = Turn.Object;
}

UEnhancedInputLocalPlayerSubsystem* ULocomotionComponent::GetInputSubsystem() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	return PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr;
}

void ULocomotionComponent::BeginPlay()
{
	Super::BeginPlay();
	if (UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(); Input && LocomotionContext)
	{
		if (SuppressedContext)
		{
			Input->RemoveMappingContext(SuppressedContext);
		}
		Input->AddMappingContext(LocomotionContext, LocomotionContextPriority);
		bContextsSwapped = true;
	}
}

void ULocomotionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The local player's input subsystem outlives the level, so put the template's controls back.
	if (UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem(); Input && bContextsSwapped)
	{
		Input->RemoveMappingContext(LocomotionContext);
		if (SuppressedContext)
		{
			Input->AddMappingContext(SuppressedContext, 0);
		}
		bContextsSwapped = false;
	}
	Super::EndPlay(EndPlayReason);
}

bool ULocomotionComponent::GetHead(FVector& OutLocation, FRotator& OutRotation) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	// The pawn's camera holds the HMD pose relative to the play space, so it follows any shift made earlier this
	// frame (push-out, then walk). The camera manager's cached view would still be where the head was before it.
	if (const UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr)
	{
		OutLocation = Camera->GetComponentLocation();
		OutRotation = Camera->GetComponentRotation();
		return true;
	}
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!PC || !PC->PlayerCameraManager)
	{
		return false;
	}
	OutLocation = PC->PlayerCameraManager->GetCameraLocation();
	OutRotation = PC->PlayerCameraManager->GetCameraRotation();
	return true;
}

void ULocomotionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	CheckForExternalHeightChange();
	// Always, even with movement disabled (menus): physically leaning into a wall must never put the head inside it.
	PushOutOfWalls();

	const UEnhancedInputLocalPlayerSubsystem* Input = GetInputSubsystem();
	const UEnhancedPlayerInput* PlayerInput = Input ? Input->GetPlayerInput() : nullptr;
	if (!PlayerInput)
	{
		UE_LOG(LogLocomotion, Verbose, TEXT("No enhanced player input (controller/local player missing)"));
		return;
	}

	if (TurnAction)
	{
		const float Turn = PlayerInput->GetActionValue(TurnAction).Get<float>();
		if (!bTurnLatched && FMath::Abs(Turn) >= SnapTurnThreshold)
		{
			SnapTurn(FMath::Sign(Turn));
			bTurnLatched = true;
		}
		else if (bTurnLatched && FMath::Abs(Turn) < SnapTurnThreshold * 0.5f)
		{
			bTurnLatched = false;
		}
	}

	if (bMovementEnabled && MoveAction)
	{
		FVector2D Stick = PlayerInput->GetActionValue(MoveAction).Get<FVector2D>();
		const float Magnitude = Stick.Size();
		UE_LOG(LogLocomotion, VeryVerbose, TEXT("Move stick (%.2f, %.2f) enabled=%d"), Stick.X, Stick.Y, bMovementEnabled);
		if (Magnitude > StickDeadzone)
		{
			// Rescale so movement starts from zero at the deadzone edge, capped at full speed.
			Stick *= FMath::Min(1.f, (Magnitude - StickDeadzone) / (1.f - StickDeadzone)) / Magnitude;
			Walk(Stick, DeltaTime);
		}
	}
}

FCollisionQueryParams ULocomotionComponent::MakeQueryParams(const FName Stat) const
{
	// Ignore the player and everything riding on them (hands, beam, wrist panel, HUD): looking down at your own
	// hand must never read as "floor".
	FCollisionQueryParams Params(Stat, false, GetOwner());
	TArray<AActor*> Attached;
	GetOwner()->GetAttachedActors(Attached, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
	Params.AddIgnoredActors(Attached);
	return Params;
}

bool ULocomotionComponent::FindFloor(const FVector& Point, float CurrentFloorZ, float& OutFloorZ) const
{
	// Only floors within a step of where the player already stands: up to StepHeight higher (kerbs, ramps) or
	// MaxStepDown lower. Anything else (a car roof under a leaning head, a hole) isn't walkable from here.
	FHitResult Hit;
	const FVector Start(Point.X, Point.Y, CurrentFloorZ + StepHeight + 1.f);
	const FVector End(Point.X, Point.Y, CurrentFloorZ - MaxStepDown);
	// Room-scale lets the head lean into a wall or car. A trace starting inside geometry reports an "initial overlap"
	// hit at its start point with an upward normal - which once read as a floor 41 cm up, every frame, climbing the
	// player up the inside of a building. Ignore whatever the trace starts in, and never accept a start-point hit.
	FCollisionQueryParams Params = MakeQueryParams(SCENE_QUERY_STAT_NAME_ONLY(LocomotionFloor));
	Params.bFindInitialOverlaps = false;
	const bool bHit = GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, BlockingObjects(), Params);
	if (bHit && Hit.bStartPenetrating)
	{
		// Still started inside something: no information about the floor here - keep the current one.
		OutFloorZ = CurrentFloorZ;
		return true;
	}
	if (bHit && Hit.Time > KINDA_SMALL_NUMBER && Hit.ImpactNormal.Z >= MinFloorNormalZ)
	{
		OutFloorZ = Hit.ImpactPoint.Z;
		return true;
	}
	return false;
}

void ULocomotionComponent::Walk(const FVector2D& Stick, float DeltaTime)
{
	AActor* Pawn = GetOwner();
	FVector Head;
	FRotator HeadRotation;
	if (!Pawn || !GetHead(Head, HeadRotation))
	{
		return;
	}

	// Stick Y = forward, X = right, relative to where the head faces (flattened).
	const FRotator Yaw(0.f, HeadRotation.Yaw, 0.f);
	const FVector Desired = (Yaw.Vector() * Stick.Y + FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y) * Stick.X) * MoveSpeed * DeltaTime;

	// The play-space origin is the floor the player stands on. (Tracing down from the head instead picks up car
	// roofs, the player's own hands, etc. and then teleports them up or under the ground.)
	const float FloorZ = Pawn->GetActorLocation().Z;

	// Body capsule sits above step height, so low ledges don't block and are stepped onto below.
	const FCollisionShape Body = FCollisionShape::MakeCapsule(BodyRadius, BodyHalfHeight);
	const FCollisionQueryParams Params = MakeQueryParams(SCENE_QUERY_STAT_NAME_ONLY(LocomotionMove));
	const FVector Start = BodyCenter(Head, FloorZ);

	// Distance a sweep may travel before it hits, minus a small skin so the body never ends up touching the wall
	// (a capsule resting exactly on a wall starts the next sweep "penetrating", which used to let it through).
	auto Allowed = [](const FVector& Delta, const FHitResult& SweepHit)
	{
		return Delta.GetSafeNormal() * FMath::Max(0.f, SweepHit.Distance - WallSkin);
	};
	auto Horizontal = [](FVector V) { V.Z = 0.0; return V.GetSafeNormal(); };

	FVector Moved = Desired;
	FHitResult Hit;
	if (GetWorld()->SweepSingleByObjectType(Hit, Start, Start + Desired, FQuat::Identity, BlockingObjects(), Body, Params))
	{
		if (Hit.bStartPenetrating)
		{
			// Still inside something PushOutOfWalls couldn't resolve: only allow the part of the move heading out.
			const FVector Out = Horizontal(Hit.Normal);
			Moved = Out * FMath::Max(0.0, Desired | Out);
		}
		else
		{
			// Move up to the wall, then slide along it with what's left.
			Moved = Allowed(Desired, Hit);
			const FVector Slide = FVector::VectorPlaneProject(Desired - Moved, Horizontal(Hit.Normal));
			FHitResult SlideHit;
			const FVector SlideStart = Start + Moved;
			if (!Slide.IsNearlyZero()
				&& GetWorld()->SweepSingleByObjectType(SlideHit, SlideStart, SlideStart + Slide, FQuat::Identity, BlockingObjects(), Body, Params))
			{
				Moved += SlideHit.bStartPenetrating ? FVector::ZeroVector : Allowed(Slide, SlideHit);
			}
			else
			{
				Moved += Slide;
			}
		}
	}

	// Follow the ground (ramps, steps); no ground = don't walk off the edge.
	float NewFloorZ;
	if (!FindFloor(Head + Moved, FloorZ, NewFloorZ))
	{
		UE_LOG(LogLocomotion, Verbose, TEXT("Blocked: no floor within a step at (%.0f, %.0f) around z=%.0f"), Head.X + Moved.X, Head.Y + Moved.Y, FloorZ);
		return;
	}
	UE_LOG(LogLocomotion, VeryVerbose, TEXT("Walk %.1f cm (wanted %.1f), floor %.0f -> %.0f"), Moved.Size2D(), Desired.Size2D(), FloorZ, NewFloorZ);
	if (FMath::Abs(NewFloorZ - FloorZ) > 5.f)
	{
		UE_LOG(LogLocomotion, Log, TEXT("Step %+.0f cm at (%.0f, %.0f): floor %.0f -> %.0f (head z %.0f)"),
			NewFloorZ - FloorZ, Head.X + Moved.X, Head.Y + Moved.Y, FloorZ, NewFloorZ, Head.Z);
	}
	Pawn->AddActorWorldOffset(FVector(Moved.X, Moved.Y, NewFloorZ - FloorZ));
	LastPawnZ = Pawn->GetActorLocation().Z;
}

FVector ULocomotionComponent::BodyCenter(const FVector& Head, const float FloorZ) const
{
	return FVector(Head.X, Head.Y, FloorZ + StepHeight + BodyHalfHeight + 1.f);
}

void ULocomotionComponent::PushOutOfWalls()
{
	AActor* Pawn = GetOwner();
	FVector Head;
	FRotator HeadRotation;
	if (!Pawn || !GetHead(Head, HeadRotation))
	{
		return;
	}

	const float FloorZ = Pawn->GetActorLocation().Z;
	const FVector Center = BodyCenter(Head, FloorZ);
	const FCollisionShape Body = FCollisionShape::MakeCapsule(BodyRadius + WallSkin, BodyHalfHeight);
	const FCollisionQueryParams Params = MakeQueryParams(SCENE_QUERY_STAT_NAME_ONLY(LocomotionPushOut));

	// Horizontal only: the floor is handled by FindFloor, and the capsule sits above step height so only walls
	// (buildings, cars) reach it. Re-query after each push so corners resolve against both walls.
	FVector Push = FVector::ZeroVector;
	for (int32 Iteration = 0; Iteration < MaxPushIterations; ++Iteration)
	{
		TArray<FOverlapResult> Overlaps;
		if (!GetWorld()->OverlapMultiByObjectType(Overlaps, Center + Push, FQuat::Identity, BlockingObjects(), Body, Params))
		{
			break;
		}
		bool bPushed = false;
		for (const FOverlapResult& Overlap : Overlaps)
		{
			UPrimitiveComponent* Component = Overlap.GetComponent();
			FMTDResult MTD;
			if (!Component || !Component->ComputePenetration(MTD, Body, Center + Push, FQuat::Identity))
			{
				continue;
			}
			// Escaping horizontally from a slanted push takes Distance / cos(angle to the horizontal).
			const double HorizontalFraction = MTD.Direction.Size2D();
			if (HorizontalFraction < 0.3 || MTD.Distance <= 0.f)
			{
				continue; // something mostly above/below the body, not a wall
			}
			const FVector Direction(MTD.Direction.X / HorizontalFraction, MTD.Direction.Y / HorizontalFraction, 0.0);
			Push += Direction * (MTD.Distance / HorizontalFraction + 0.1f);
			bPushed = true;
		}
		if (!bPushed)
		{
			break;
		}
	}

	if (Push.IsNearlyZero(0.01))
	{
		return;
	}
	UE_CLOG(Push.Size2D() > 10.f, LogLocomotion, Log, TEXT("Pushed out of wall by %.0f cm at (%.0f, %.0f)"), Push.Size2D(), Head.X, Head.Y);

	// Follow the ground where we land (normally unchanged); if there's none, still keep the head out of the wall.
	float NewFloorZ = FloorZ;
	FindFloor(Head + Push, FloorZ, NewFloorZ);
	Pawn->AddActorWorldOffset(FVector(Push.X, Push.Y, NewFloorZ - FloorZ));
	LastPawnZ = Pawn->GetActorLocation().Z;
}

void ULocomotionComponent::CheckForExternalHeightChange()
{
	const AActor* Pawn = GetOwner();
	if (!Pawn)
	{
		return;
	}
	const float Z = Pawn->GetActorLocation().Z;
	if (bHasLastPawnZ && FMath::Abs(Z - LastPawnZ) > 2.f)
	{
		FVector Head;
		FRotator HeadRotation;
		GetHead(Head, HeadRotation);
		UE_LOG(LogLocomotion, Warning, TEXT("Player height changed OUTSIDE locomotion: %.0f -> %.0f (head %.0f, %.0f, %.0f)"),
			LastPawnZ, Z, Head.X, Head.Y, Head.Z);
	}
	LastPawnZ = Z;
	bHasLastPawnZ = true;
}

void ULocomotionComponent::SnapTurn(float Direction)
{
	AActor* Pawn = GetOwner();
	FVector Head;
	FRotator HeadRotation;
	if (!Pawn || !GetHead(Head, HeadRotation))
	{
		return;
	}
	// Rotate the pawn about the head so the player turns on the spot instead of swinging round the play-space origin.
	const FRotator Turn(0.f, SnapTurnAngle * Direction, 0.f);
	const FVector Offset = Pawn->GetActorLocation() - Head;
	Pawn->SetActorLocationAndRotation(Head + Turn.RotateVector(Offset), Pawn->GetActorRotation() + Turn);
	LastPawnZ = Pawn->GetActorLocation().Z;
}
