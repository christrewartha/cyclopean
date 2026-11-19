#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EnemyRock.generated.h"

class AEnemySpawner;
class UMaterialInterface;
class USoundBase;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Glowing rock thrown by a thrower walker. Flies on a gentle arc (light gravity); hitting the player's head costs a
 * life, hitting level geometry smashes it, and the optic blast can shoot it down. Freezes while the spawner is paused
 * and disappears when the run ends.
 */
UCLASS()
class CYCLOPS_API AEnemyRock : public AActor
{
	GENERATED_BODY()

public:
	AEnemyRock();

	void Launch(AEnemySpawner* InSpawner, const FVector& InVelocity);

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** Downward acceleration (cm/s^2) - much lighter than real gravity so the arc stays readable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rock", meta = (Units = "cm/s"))
	float Gravity = 400.f;

	/** Counts as a hit when this close to the player's eyes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rock", meta = (Units = "cm"))
	float HitRadius = 38.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rock", meta = (Units = "s"))
	float MaxLifetime = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rock|Assets")
	TObjectPtr<USoundBase> ImpactSound;

protected:
	virtual void Tick(float DeltaSeconds) override;

private:
	void Smash();

	UPROPERTY(VisibleAnywhere, Category = "Rock")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "Rock")
	TObjectPtr<UStaticMeshComponent> Mesh;

	TWeakObjectPtr<AEnemySpawner> Spawner;
	FVector Velocity = FVector::ZeroVector;
	float Age = 0.f;
	bool bSmashed = false;
};
