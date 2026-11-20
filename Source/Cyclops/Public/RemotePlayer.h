#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RemotePlayer.generated.h"

class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class UTextRenderComponent;

/** The local player shot this avatar. bHeadshot = the head (rather than the body) was hit. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnRemotePlayerShot, float /*Damage*/, bool /*bHeadshot*/);

/**
 * The other player in a versus match, as seen here: a head (with a red cyclops visor), a body hanging below it and
 * the head-locked gun, all driven by the head poses they send.
 *
 * Poses arrive ~30 times a second with network jitter, so they're buffered and played back InterpDelay behind the
 * newest one, interpolating between the two around that time. Sender clocks differ from ours, so the clock offset is
 * estimated from the fastest-arriving pose.
 *
 * Head and body block the Visibility channel, so the local gun can shoot them (point damage -> OnShot, which the
 * match sends to the other player as a hit).
 */
UCLASS()
class CYCLOPS_API ARemotePlayer : public AActor
{
	GENERATED_BODY()

public:
	ARemotePlayer();

	/** A head pose from the other player; SenderTime is their clock (seconds). */
	void PushPose(double SenderTime, const FVector& HeadLocation, const FRotator& HeadRotation);

	/** Draws one of their shots (a tracer from their muzzle). */
	void ShowShot(const FVector& From, const FVector& To);

	void SetPlayerName(const FString& Name);

	/** They died: hide them (and their hitbox) and ignore poses until they respawn. */
	void SetDead(bool bInDead);

	/** They teleported (respawn): forget buffered poses so they snap to the next one instead of sliding there. */
	void ResetPoses();

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	FOnRemotePlayerShot OnShot;

	/** How far behind the newest pose playback runs: absorbs network jitter at the cost of that much extra lag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Remote Player", meta = (ClampMin = "0", Units = "s"))
	float InterpDelay = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player")
	FLinearColor BodyColour = FLinearColor(0.12f, 0.17f, 0.26f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player")
	FLinearColor VisorColour = FLinearColor(1.f, 0.12f, 0.04f);

	/** Same placement as the local AHandGun, so their gun is where they see it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player")
	FVector GunOffset = FVector(30.0, 13.0, -17.0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player|Assets")
	TObjectPtr<UStaticMesh> SphereMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player|Assets")
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player|Assets")
	TObjectPtr<UStaticMesh> CylinderMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player|Assets")
	TObjectPtr<UStaticMesh> GunMesh;

	/** Unlit, colour from custom primitive data 0-3 (M_HudBar). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Player|Assets")
	TObjectPtr<UMaterialInterface> ColourMaterial;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FSnapshot
	{
		double Time = 0.0; // sender clock
		FVector Location = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
	};

	void ApplyPose(const FVector& HeadLocation, const FQuat& HeadRotation);
	void SetColour(UStaticMeshComponent* Mesh, const FLinearColor& Colour) const;
	void FaceNameTagToViewer();

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<USceneComponent> HeadRoot;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UStaticMeshComponent> Head;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UStaticMeshComponent> Visor;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UStaticMeshComponent> Gun;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UTextRenderComponent> NameTag;

	UPROPERTY(VisibleAnywhere, Category = "Remote Player")
	TObjectPtr<UStaticMeshComponent> Tracer;

	TArray<FSnapshot> Snapshots;
	double ClockOffset = 0.0;       // our clock - their clock, for the fastest-arriving pose
	bool bHasClockOffset = false;
	FVector TracerStart = FVector::ZeroVector;
	FVector TracerEnd = FVector::ZeroVector;
	float TracerAge = -1.f;
	float HitFlash = 0.f;
	bool bDead = false;
};
