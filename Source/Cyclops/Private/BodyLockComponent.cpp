#include "BodyLockComponent.h"

#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
	constexpr float SettledAngle = 1.5f; // stop following once this close to the head's facing
}

UBodyLockComponent::UBodyLockComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the HMD has updated the camera for this frame.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	// Placed in world space every tick; the owner's transform is irrelevant.
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
}

void UBodyLockComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const USceneComponent* Camera = ViewCamera.Get();
	if (!Camera)
	{
		// The pawn may arrive after this component (or be replaced): look it up until found.
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr;
		ViewCamera = const_cast<USceneComponent*>(Camera);
		if (!Camera)
		{
			return;
		}
		bHasYaw = false;
	}

	const FVector Eye = Camera->GetComponentLocation();
	const float HeadYaw = Camera->GetComponentRotation().Yaw;
	if (!bHasYaw)
	{
		Yaw = HeadYaw;
		bHasYaw = true;
		bFollowing = false;
	}

	// Lazy follow: ignore small head turns; once past FollowAngle, ease all the way back to straight ahead.
	const float Delta = FMath::FindDeltaAngleDegrees(Yaw, HeadYaw);
	if (!bFollowing && FMath::Abs(Delta) > FollowAngle)
	{
		bFollowing = true;
	}
	if (bFollowing)
	{
		Yaw = FRotator::NormalizeAxis(Yaw + Delta * FMath::Min(1.f, DeltaTime * FollowSpeed));
		bFollowing = FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw, HeadYaw)) > SettledAngle;
	}

	// Position follows the eyes exactly (walking, crouching); X points from the eyes to here, so it faces them.
	const FVector Location = Eye + FRotator(0.f, Yaw, 0.f).Vector() * ForwardDistance - FVector(0.0, 0.0, DropBelowEyes);
	SetWorldLocationAndRotation(Location, (Location - Eye).Rotation());
}
