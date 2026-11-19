#pragma once

#include "CoreMinimal.h"
#include "CyclopsEnemy.h"
#include "GameFramework/Character.h"
#include "WalkingEnemy.generated.h"

class UAudioComponent;
class UMaterialInterface;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EWalkingEnemyState : uint8
{
	Emerging,	// scales in at its spawn point + announce sound
	Walking,	// pathfinding towards the player
	Attacking,	// in reach: winds up, then strikes (costs a life)
	Throwing,	// thrower only: stopped at range, charging a rock (glows + howls), then throws it
	Dead,		// pieces tumbling under physics
};

/** How a walker approaches (Pac-Man ghost style), so a group doesn't arrive in single file. */
UENUM(BlueprintType)
enum class EWalkerStyle : uint8
{
	Chaser,		// straight at the player, with some sideways drift
	Flanker,	// aims for a point off to one side of the player
	Ambusher,	// aims ahead of where the player is looking, to cut them off
};

/**
 * Placeholder human-sized walking enemy: a one-eyed blocky figure built from primitive meshes (no skeletal mesh),
 * with procedural leg/arm swing driven by its speed.
 *
 * Walks to the player on the navmesh (AAIController::MoveToLocation, re-aimed every RepathInterval), using one of
 * three approach styles (EWalkerStyle) until within CommitDistance, with RVO avoidance between walkers. In reach it
 * snarls and strikes almost at once (AttackWindup), costing the player a life, then vanishes. Damaged by any UE damage event; point
 * damage landing above HeadshotHeight is a headshot (extra damage, and a headshot kill scores a bonus and pops the
 * head off). On death the parts become physics bodies knocked along the shot direction (same as AFlyingEnemy).
 */
UCLASS()
class CYCLOPS_API AWalkingEnemy : public ACharacter, public ICyclopsEnemy
{
	GENERATED_BODY()

public:
	AWalkingEnemy();

	// ICyclopsEnemy
	virtual void InitEnemy(AEnemySpawner* InSpawner, float InSpeed) override;
	virtual void SetFrozen(bool bInFrozen) override;
	virtual bool IsDead() const override { return State == EWalkingEnemyState::Dead; }

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	UFUNCTION(BlueprintPure, Category = "Enemy")
	EWalkingEnemyState GetState() const { return State; }

	// ---- Tuning ----

	/** 55 = a full-energy burst (60) one-shots it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy", meta = (ClampMin = "1"))
	float MaxHealth = 55.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy", meta = (Units = "s"))
	float EmergeTime = 0.7f;

	/** Hits landing at least this high above the feet count as headshots (the capsule's top hemisphere ~ head). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Headshot", meta = (Units = "cm"))
	float HeadshotHeight = 155.f;

	/** Damage multiplier for headshots (a sustained beam on the head kills ~2.5x faster). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Headshot", meta = (ClampMin = "1"))
	float HeadshotDamageMultiplier = 2.5f;

	/** The moan's pitch is randomised per walker within +/- this fraction, so a crowd doesn't moan in unison. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Assets", meta = (ClampMin = "0", ClampMax = "0.5"))
	float AnnouncePitchVariation = 0.12f;

	/** Strikes when its centre is this close to the player's head (horizontally). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Attack", meta = (Units = "cm"))
	float AttackRange = 120.f;

	/** Delay between getting in reach and the strike landing: near-instant, the snarl (AttackSound) is the only warning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Attack", meta = (Units = "s"))
	float AttackWindup = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Movement", meta = (Units = "s"))
	float RepathInterval = 0.4f;

	/** Each walker's speed is the spawner's speed +/- this fraction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Movement", meta = (ClampMin = "0", ClampMax = "0.5"))
	float SpeedVariation = 0.12f;

	// ---- Approach styles (picked per walker on spawn) ----

	/** Chance of a chaser (straight at the player, with drift); the rest split between flankers and ambushers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (ClampMin = "0", ClampMax = "1"))
	float ChaserWeight = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (ClampMin = "0", ClampMax = "1"))
	float FlankerWeight = 0.35f;

	/** Within this distance every style commits and comes straight in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (Units = "cm"))
	float CommitDistance = 450.f;

	/** How far to the side of the player a flanker aims (scaled 0.75-1.25 per walker). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (Units = "cm"))
	float FlankDistance = 400.f;

	/** How far ahead of the player (along their view) an ambusher aims. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (Units = "cm"))
	float AmbushLead = 500.f;

	/** Random sideways drift re-rolled every 1.5-3 s, so even chasers don't share one path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Approach", meta = (Units = "cm"))
	float MaxDrift = 250.f;

	// ---- Throwing (AThrowerEnemy turns this on) ----

	/** Stops at range and throws rocks (with a glow + howl warning) as well as attacking up close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Throw")
	bool bCanThrow = false;

	/** Throws when the player is between these distances (and in line of sight). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Throw", meta = (Units = "cm"))
	FVector2D ThrowRange = FVector2D(500.f, 1600.f);

	/** The warning: arm drawn back, rock glowing, body pulsing, charge howl - then the throw. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Throw", meta = (Units = "s"))
	float ThrowWindup = 1.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Throw", meta = (Units = "s"))
	float ThrowCooldown = 3.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Throw", meta = (Units = "cm/s"))
	float ThrowSpeed = 950.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Throw")
	TSubclassOf<class AEnemyRock> RockClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Throw")
	TObjectPtr<USoundBase> ChargeSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Throw")
	TObjectPtr<USoundBase> ReleaseSound;

	/** Glowing rock material (shown in the hand during the wind-up). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Throw")
	TObjectPtr<UMaterialInterface> RockMaterial;

	/** Stride length used for the walk cycle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Movement", meta = (Units = "cm"))
	float StrideLength = 70.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Death", meta = (Units = "cm/s"))
	float DeathKnockback = 450.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enemy|Death", meta = (Units = "s"))
	float CorpseLifeSpan = 4.f;

	// ---- Assets ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<UStaticMesh> SphereMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<UMaterialInterface> BodyMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<UMaterialInterface> EyeMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> AnnounceSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> DeathSound;

	/** Played on a headshot kill, on top of the death sound. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> HeadshotSound;

	/** Snarl as it lunges - the warning that a strike is landing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enemy|Assets")
	TObjectPtr<USoundBase> AttackSound;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool GetPlayerHead(FVector& OutLocation) const;
	void UpdateWalkCycle(float DeltaSeconds);
	void Strike();
	bool CanSeePlayer(const FVector& PlayerHead) const;
	void StartThrow();
	void ReleaseRock(const FVector& PlayerHead);
	void TickThrow(float DeltaSeconds, const FVector& PlayerHead, float Distance);
	void Die(const FVector& ShotDirection, bool bOneShot, bool bHeadshot);
	void SetFlash(float Value);
	void ApplyMeshesAndMaterials();
	UStaticMeshComponent* MakePart(FName Name, USceneComponent* Parent, const FVector& Location, const FVector& Scale);

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<USceneComponent> VisualRoot;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> Torso;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> Head;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> Eye;

	/** Left, right. */
	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TArray<TObjectPtr<USceneComponent>> ArmPivots;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TArray<TObjectPtr<UStaticMeshComponent>> Arms;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TArray<TObjectPtr<USceneComponent>> LegPivots;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TArray<TObjectPtr<UStaticMeshComponent>> Legs;

	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UAudioComponent> VoiceAudio;

	/** Rock held in the throwing hand during the wind-up (hidden otherwise). */
	UPROPERTY(VisibleAnywhere, Category = "Enemy")
	TObjectPtr<UStaticMeshComponent> HeldRock;

	float ThrowCooldownLeft = 0.f;

	TWeakObjectPtr<AEnemySpawner> Spawner;
	EWalkingEnemyState State = EWalkingEnemyState::Emerging;
	bool bFrozen = false;
	bool bHasBeenHit = false;
	float Health = 0.f;
	float StateTime = 0.f;
	float RepathTimer = 0.f;
	float WalkPhase = 0.f;
	float Flash = 0.f;

	// Approach.
	EWalkerStyle Style = EWalkerStyle::Chaser;
	float FlankSide = 1.f;    // which side a flanker comes from, x a per-walker distance scale
	float Drift = 0.f;        // current sideways drift (cm)
	float DetourTimer = 0.f;  // until the drift is re-rolled
	/** Where to path to this repath: the player, or a style-dependent point near them while still far away. */
	FVector PickApproachTarget(const FVector& PlayerFeet, float Distance);
};
