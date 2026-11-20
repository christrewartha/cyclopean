#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "BodyLockComponent.generated.h"

/**
 * Body-locked anchor for VR HUD elements: stays a fixed distance in front of and below the player's eyes, following
 * their position exactly but their facing lazily - it only swings round once the head has turned more than
 * FollowAngle away, so the player can look around freely and glance down to read it.
 *
 * Its X axis points from the eyes to the anchor (like camera space), so things laid out for the camera (text turned
 * yaw 180 to face the eye, +X = further away) can be attached here unchanged.
 */
UCLASS(ClassGroup = (Cyclops), meta = (BlueprintSpawnableComponent))
class CYCLOPS_API UBodyLockComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UBodyLockComponent();

	/** Horizontal distance in front of the eyes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lock", meta = (Units = "cm"))
	float ForwardDistance = 60.f;

	/** How far below the eyes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lock", meta = (Units = "cm"))
	float DropBelowEyes = 55.f;

	/** Head turns up to this far away are ignored; beyond it the anchor swings round to face the player again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lock", meta = (ClampMin = "0", ClampMax = "180", Units = "deg"))
	float FollowAngle = 40.f;

	/** How quickly it swings round once following (higher = faster). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lock", meta = (ClampMin = "0.1"))
	float FollowSpeed = 4.f;

	/** Immediately re-centre in front of the player's current facing. */
	UFUNCTION(BlueprintCallable, Category = "Body Lock")
	void Recenter() { bHasYaw = false; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	TWeakObjectPtr<USceneComponent> ViewCamera;
	float Yaw = 0.f;
	bool bHasYaw = false;
	bool bFollowing = false;
};
