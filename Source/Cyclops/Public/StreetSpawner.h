#pragma once

#include "CoreMinimal.h"
#include "EnemySpawner.h"
#include "StreetSpawner.generated.h"

class ULocomotionComponent;

/**
 * Rules for the walking "Streets" level: the player walks (ULocomotionComponent) along a directed route from this
 * actor's location to the exit, and walking enemies emerge from spawn points ahead of them, preferably out of sight.
 *
 * Level markers are found by actor tag (so any actor - e.g. a Target Point - can be one):
 *  - RoutePointTag: corners of the route's polyline (chained nearest-first from this actor). Measures progress.
 *  - SpawnPointTag: where enemies can appear (on the navmesh, facing doesn't matter).
 *  - ExitTag: the goal; reaching it ends the run with a finish bonus.
 *
 * Place this actor at the start of the route, on the ground, facing down the street: runs start there.
 */
UCLASS()
class CYCLOPS_API AStreetSpawner : public AEnemySpawner
{
	GENERATED_BODY()

public:
	AStreetSpawner();

	/** 0 at the start, 1 at the exit. */
	UFUNCTION(BlueprintPure, Category = "Streets")
	float GetProgress() const;

	// ---- Markers ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Streets|Markers")
	FName RoutePointTag = TEXT("Route");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Streets|Markers")
	FName SpawnPointTag = TEXT("EnemySpawn");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Streets|Markers")
	FName ExitTag = TEXT("LevelExit");

	// ---- Spawning ----

	/** Enemies appear between these distances further along the route than the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Spawning", meta = (Units = "cm"))
	float SpawnAheadMin = 1200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Spawning", meta = (Units = "cm"))
	float SpawnAheadMax = 4000.f;

	/** Never closer than this to the player (straight line), so nothing pops in at arm's length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Spawning", meta = (Units = "cm"))
	float MinSpawnDistance = 900.f;

	/** Up to this many extra enemies alive at once by the end of the route. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Spawning", meta = (ClampMin = "0"))
	int32 ExtraMaxAliveAtExit = 2;

	// ---- Versus ----

	/** Slot 1 starts this far down the street from slot 0 (this actor), facing back towards them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Versus", meta = (Units = "cm"))
	float VersusStartSeparation = 2500.f;

	virtual void BeginFreeRoam(int32 Slot) override;
	virtual void EndFreeRoam() override;
	virtual void RespawnFreeRoam(int32 Slot, const FVector& AvoidLocation) override;

	/**
	 * Versus respawns pick from the enemy spawn markers and the two starts, preferring ones the opponent can't see
	 * (no building or car in between) at this distance band from them, so you come back out of sight but near the fight.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Versus", meta = (Units = "cm"))
	float MinRespawnDistance = 1200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Versus", meta = (Units = "cm"))
	float MaxRespawnDistance = 4000.f;

	// ---- Throwers (walkers that stop and throw rocks from range) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Streets|Throwers", meta = (MustImplement = "/Script/Cyclops.CyclopsEnemy"))
	TSubclassOf<AActor> ThrowerClass;

	/** Chance that a walker spawn is a thrower (once the player has ThrowerMinKills). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Throwers", meta = (ClampMin = "0", ClampMax = "1"))
	float ThrowerChance = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Throwers", meta = (ClampMin = "0"))
	int32 ThrowerMinKills = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Throwers", meta = (ClampMin = "0"))
	int32 MaxThrowers = 2;

	/** Throwers move at this fraction of the walker speed (they hang back). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Throwers", meta = (ClampMin = "0.1", ClampMax = "2"))
	float ThrowerSpeedScale = 0.8f;

	// ---- Bats (occasional flyers among the walkers) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Streets|Bats", meta = (MustImplement = "/Script/Cyclops.CyclopsEnemy"))
	TSubclassOf<AActor> BatClass;

	/** Chance that a spawn is a bat (once the player has BatMinKills). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (ClampMin = "0", ClampMax = "1"))
	float BatChance = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (ClampMin = "0"))
	int32 BatMinKills = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (ClampMin = "0"))
	int32 MaxBats = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (Units = "cm/s"))
	float BatSpeed = 180.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (Units = "cm/s"))
	float BatSpeedPerKill = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (Units = "cm/s"))
	float MaxBatSpeed = 300.f;

	/** Bats appear this high above the street, over the route ahead (where the player can see them). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Bats", meta = (Units = "cm"))
	FVector2D BatHeight = FVector2D(400.f, 600.f);

	// ---- Finish ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Finish", meta = (Units = "cm"))
	float ExitRadius = 300.f;

	/** Bonus = Base + PerLife x lives left + PerSecond x seconds under par. Keep in sync with the server's cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Finish")
	int32 FinishBonusBase = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Finish")
	int32 FinishBonusPerLife = 500;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Finish", meta = (Units = "s"))
	float ParTime = 180.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streets|Finish")
	int32 FinishBonusPerSecondUnderPar = 10;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	virtual TSubclassOf<AActor> PickEnemyClass() override;
	virtual bool PickSpawnLocation(TSubclassOf<AActor> Class, FVector& OutLocation, FRotator& OutRotation) override;
	virtual float GetSpawnSpeed(TSubclassOf<AActor> Class) const override;
	virtual int32 GetCurrentMaxAlive() const override;
	virtual void OnRunStarted() override;
	virtual void OnRunStopped() override;
	virtual void OnPausedChanged(bool bPaused) override;
	/** Turns and moves the play space so the head is over Location (on the floor) facing Yaw. */
	void PlacePlayerAt(const FVector& Location, float Yaw);

private:
	struct FSpawnPoint
	{
		FVector Location;
		float RouteDistance = 0.f;
	};

	void GatherMarkers();
	/** Distance along the route of the closest point on it to Location (horizontal). */
	float RouteDistanceOf(const FVector& Location) const;
	float RouteLength() const { return RouteCumulative.Num() ? RouteCumulative.Last() : 0.f; }
	/** Point on the route at a distance along it (clamped to the ends). */
	FVector RoutePointAt(float Distance) const;
	bool PickWalkerLocation(FVector& OutLocation, FRotator& OutRotation) const;
	bool PickBatLocation(FVector& OutLocation, FRotator& OutRotation) const;
	bool IsBat(TSubclassOf<AActor> Class) const { return BatClass && Class && Class->IsChildOf(BatClass); }
	ULocomotionComponent* EnsureLocomotion();
	void UpdateLocomotion();
	void PlacePlayerAtStart();

	TArray<FVector> Route;
	TArray<float> RouteCumulative;
	TArray<FSpawnPoint> SpawnPoints;
	FVector ExitLocation = FVector::ZeroVector;
	bool bHasExit = false;
	bool bFreeRoam = false;
	TWeakObjectPtr<ULocomotionComponent> Locomotion;
};
