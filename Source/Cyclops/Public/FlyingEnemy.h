#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FlyingEnemy.generated.h"

class AEnemySpawner;
class UAudioComponent;
class UMaterialInterface;
class USoundBase;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EFlyingEnemyState : uint8
{
	Appearing,	// scaling in + announce sound
	Hovering,	// brief pause so the player can locate it
	Attacking,	// weaving flight towards the player's head
	Dead,		// pieces tumbling under physics
};

/**
 * Placeholder flying enemy ("bat"): a body, two eyes and two flapping wings built from primitive meshes,
 * so no skeletal mesh or animation is needed.
 *
 * Flies slowly at the player's head along a base path that re-aims every frame, plus two out-of-phase
 * sine offsets (horizontal/vertical) whose amplitude shrinks as it closes in, so it weaves but always
 * arrives. Damaged by any UE damage event (the optic blast applies point damage). On death the parts
 * become simulating physics bodies and get knocked along the shot direction.
 */
UCLASS()
class CYCLOPS_API AFlyingEnemy : public AActor
{
	GENERATED_BODY()

public:
	AFlyingEnemy();

	/** Called by the spawner right after spawning. */
	void InitEnemy(AEnemySpawner* InSpawner, float InSpeed);

	UFUNCTION(BlueprintPure, Category = "Enemy")
	EFlyingEnemyState GetState() const { return State; }

	/** Freeze in place (game paused): no movement, no damage, sounds paused. */
	void SetFrozen(bool bInFrozen);

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	// ---- Tuning ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy", meta = (ClampMin = "1"))
	float MaxHealth = 50.f;

	/** Base flight speed towards the player (cm/s). The spawner may override this per spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (ClampMin = "0", Units = "cm/s"))
	float Speed = 150.f;

	/** Sideways weave amplitude when far away. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (Units = "cm"))
	float WeaveAmplitude = 180.f;

	/** Weave amplitude scales down to this fraction as it reaches WeaveFadeDistance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (ClampMin = "0", ClampMax = "1"))
	float WeaveMinFraction = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (Units = "cm"))
	float WeaveFadeDistance = 900.f;

	/** Weave frequency (Hz). Horizontal and vertical use slightly different rates so the path never repeats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (ClampMin = "0"))
	float WeaveFrequency = 0.45f;

	/** After the hover, the weave grows from zero to full over this time (avoids a snap into the weave). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (ClampMin = "0.01", Units = "s"))
	float WeaveRampTime = 1.2f;

	/** After the hover, flight speed builds from 15% to full over this time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (ClampMin = "0.01", Units = "s"))
	float SpeedRampTime = 0.6f;

	/** Within this distance of the HMD it counts as having reached the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (Units = "cm"))
	float AttackRadius = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (Units = "s"))
	float AppearTime = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Flight", meta = (Units = "s"))
	float HoverTime = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Look")
	float FlapRate = 7.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Look", meta = (Units = "deg"))
	float FlapAngle = 40.f;

	/** Impulse given to the falling pieces along the killing shot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Death", meta = (Units = "cm/s"))
	float DeathKnockback = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Death", meta = (Units = "s"))
	float CorpseLifeSpan = 4.f;

	// ---- Assets (set in the Blueprint subclass) ----

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<UStaticMesh> BodyMesh;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<UStaticMesh> WingMesh;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<UMaterialInterface> BodyMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<UMaterialInterface> WingMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<UMaterialInterface> EyeMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> AnnounceSound;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> FlapLoopSound;

	UPROPERTY(EditDefaultsOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> DeathSound;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void TickFlight(float DeltaSeconds, const FVector& HeadLocation);
	void TickLook(float DeltaSeconds);
	void Die(const FVector& ShotDirection);
	void ReachPlayer();
	bool GetHeadLocation(FVector& OutLocation) const;
	void SetFlash(float Value);
	void MakePhysicsPiece(UStaticMeshComponent* Piece, const FVector& Impulse);

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> EyeLeft;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> EyeRight;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<USceneComponent> WingPivotLeft;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<USceneComponent> WingPivotRight;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> WingLeft;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> WingRight;

	/** Looping flaps, spatialised so the player can hear where it is. */
	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UAudioComponent> FlapAudio;

	/** One-shots (announce, death), spatialised. */
	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UAudioComponent> VoiceAudio;

	TWeakObjectPtr<AEnemySpawner> Spawner;

	EFlyingEnemyState State = EFlyingEnemyState::Appearing;
	bool bFrozen = false;
	float StateTime = 0.f;
	float Health = 0.f;
	float Flash = 0.f;
	float WeavePhaseH = 0.f;
	float WeavePhaseV = 0.f;
	float WeaveSide = 1.f;
	FVector BasePosition = FVector::ZeroVector;
	FVector LastLocation = FVector::ZeroVector;
};
