#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocomotionComponent.generated.h"

class UInputAction;
class UInputMappingContext;
struct FCollisionQueryParams;

/**
 * Smooth thumbstick walking + snap turn for the VR pawn (added at runtime by levels that want it, e.g. AStreetSpawner).
 *
 * Left stick walks relative to where the head is looking; right stick snap-turns around the head. Movement is a
 * collision sweep of a body-sized capsule under the HMD, sliding along walls and following the ground (steps/ramps),
 * so it works with the template pawn without a CharacterMovementComponent.
 *
 * While active, the template's own mapping context (teleport + left-stick snap turn) is swapped out for
 * IMC_Locomotion, and restored when the component is removed (the input subsystem survives level travel).
 */
UCLASS(ClassGroup = (Cyclops), meta = (BlueprintSpawnableComponent))
class CYCLOPS_API ULocomotionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULocomotionComponent();

	/** Off while menus are open / the game is paused (turning stays available). */
	void SetMovementEnabled(bool bEnabled) { bMovementEnabled = bEnabled; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion", meta = (Units = "cm/s"))
	float MoveSpeed = 220.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion", meta = (ClampMin = "0", ClampMax = "0.9"))
	float StickDeadzone = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion", meta = (Units = "deg"))
	float SnapTurnAngle = 45.f;

	/** Stick deflection that triggers a snap turn; it must return below half this before the next one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion", meta = (ClampMin = "0.1", ClampMax = "1"))
	float SnapTurnThreshold = 0.7f;

	/** Body capsule used for wall collision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion|Collision", meta = (Units = "cm"))
	float BodyRadius = 22.f;

	/** Ledges up to this high are stepped onto; anything taller blocks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Locomotion|Collision", meta = (Units = "cm"))
	float StepHeight = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Input")
	TObjectPtr<UInputMappingContext> LocomotionContext;

	/** Swapped out while this component is active (the template's teleport / snap turn). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Input")
	TObjectPtr<UInputMappingContext> SuppressedContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Input")
	TObjectPtr<UInputAction> TurnAction;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	class UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem() const;
	bool GetHead(FVector& OutLocation, FRotator& OutRotation) const;
	void Walk(const FVector2D& Stick, float DeltaTime);
	void SnapTurn(float Direction);
	/** Walkable floor under a point within a step of CurrentFloorZ (StepHeight up, MaxStepDown down). */
	bool FindFloor(const FVector& Point, float CurrentFloorZ, float& OutFloorZ) const;
	/** Query params ignoring the player and everything attached to them. */
	FCollisionQueryParams MakeQueryParams(FName Stat) const;

	/** Diagnostics: logs any change in the player's height that this component didn't make. */
	void CheckForExternalHeightChange();

	bool bMovementEnabled = false;
	float LastPawnZ = 0.f;
	bool bHasLastPawnZ = false;
	bool bTurnLatched = false;
	bool bContextsSwapped = false;
};
