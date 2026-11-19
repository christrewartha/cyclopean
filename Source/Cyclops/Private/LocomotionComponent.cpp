#include "LocomotionComponent.h"

#include "Camera/PlayerCameraManager.h"
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
	const FVector Start(Head.X, Head.Y, FloorZ + StepHeight + BodyHalfHeight + 1.f);

	FVector Moved = Desired;
	FHitResult Hit;
	if (GetWorld()->SweepSingleByObjectType(Hit, Start, Start + Desired, FQuat::Identity, BlockingObjects(), Body, Params)
		&& !Hit.bStartPenetrating) // already inside something (room-scale): let them walk out
	{
		// Move up to the wall, then slide along it with what's left.
		Moved = Desired * FMath::Max(0.f, Hit.Time - 0.01f);
		FVector WallNormal = Hit.ImpactNormal;
		WallNormal.Z = 0.0;
		const FVector Slide = FVector::VectorPlaneProject(Desired - Moved, WallNormal.GetSafeNormal());
		FHitResult SlideHit;
		const FVector SlideStart = Start + Moved;
		if (GetWorld()->SweepSingleByObjectType(SlideHit, SlideStart, SlideStart + Slide, FQuat::Identity, BlockingObjects(), Body, Params))
		{
			Moved += Slide * FMath::Max(0.f, SlideHit.Time - 0.01f);
		}
		else
		{
			Moved += Slide;
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
