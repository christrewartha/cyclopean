#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EnemySpawner.generated.h"

class AFlyingEnemy;
class USoundBase;
class UTextRenderComponent;

/**
 * Spawns flying enemies around the player and keeps score.
 *
 * Enemies appear at SpawnDistance from the player's head, at SpawnHeight above this actor, within
 * +/-SpawnYawHalfRange of this actor's forward (so place it at the player start, facing into the arena).
 * Difficulty ramps: spawn interval shrinks per spawn, max-alive and enemy speed grow with kills.
 */
UCLASS()
class CYCLOPS_API AEnemySpawner : public AActor
{
	GENERATED_BODY()

public:
	AEnemySpawner();

	UFUNCTION(BlueprintCallable, Category = "Spawner")
	void StartSpawning();

	UFUNCTION(BlueprintCallable, Category = "Spawner")
	void StopSpawning();

	/** Stops, kills all live enemies and zeroes the score and difficulty. */
	UFUNCTION(BlueprintCallable, Category = "Spawner")
	void ResetSpawner();

	/** Soft pause: holds the spawn timer and freezes live enemies in place. */
	UFUNCTION(BlueprintCallable, Category = "Spawner")
	void SetGamePaused(bool bPaused);

	UFUNCTION(BlueprintPure, Category = "Spawner")
	bool IsGamePaused() const { return bGamePaused; }

	/** Hidden while menus are open so it doesn't clash with them. */
	UFUNCTION(BlueprintCallable, Category = "Spawner")
	void SetScoreboardVisible(bool bVisible);

	void NotifyEnemyKilled(AFlyingEnemy* Enemy);
	void NotifyEnemyReachedPlayer(AFlyingEnemy* Enemy);

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetKills() const { return Kills; }

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetHitsTaken() const { return HitsTaken; }

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner")
	TSubclassOf<AFlyingEnemy> EnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner", meta = (Units = "s"))
	float StartDelay = 3.f;

	// ---- Pacing ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "0.1", Units = "s"))
	float InitialSpawnInterval = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "0.1", Units = "s"))
	float MinSpawnInterval = 1.5f;

	/** Interval is multiplied by this after every spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "0.5", ClampMax = "1"))
	float IntervalMultiplier = 0.96f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "1"))
	int32 InitialMaxAlive = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "1"))
	int32 MaxAliveCap = 6;

	/** One more enemy may be alive at once for every this many kills. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (ClampMin = "1"))
	int32 KillsPerExtraEnemy = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (Units = "cm/s"))
	float InitialEnemySpeed = 140.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (Units = "cm/s"))
	float SpeedIncreasePerKill = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Pacing", meta = (Units = "cm/s"))
	float MaxEnemySpeed = 320.f;

	// ---- Placement ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Placement", meta = (Units = "cm"))
	float SpawnDistanceMin = 1200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Placement", meta = (Units = "cm"))
	float SpawnDistanceMax = 1800.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Placement", meta = (Units = "cm"))
	float SpawnHeightMin = 300.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Placement", meta = (Units = "cm"))
	float SpawnHeightMax = 650.f;

	/** Spawns within +/- this yaw of the spawner's forward. 180 = all around the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Placement", meta = (ClampMin = "0", ClampMax = "180", Units = "deg"))
	float SpawnYawHalfRange = 75.f;

	// ---- Feedback ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner|Feedback")
	TObjectPtr<USoundBase> PlayerHitSound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Feedback", meta = (ClampMin = "0", ClampMax = "1"))
	float HitHapticAmplitude = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Feedback", meta = (Units = "s"))
	float HitHapticDuration = 0.25f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void SpawnTick();
	void ScheduleNextSpawn();
	bool PickSpawnLocation(FVector& OutLocation) const;
	int32 GetCurrentMaxAlive() const;
	float GetCurrentEnemySpeed() const;
	void UpdateScoreboard();
	void StopHaptics();

	UPROPERTY(VisibleAnywhere, Category = "Spawner")
	TObjectPtr<UTextRenderComponent> Scoreboard;

	TArray<TWeakObjectPtr<AFlyingEnemy>> AliveEnemies;
	FTimerHandle SpawnTimer;
	FTimerHandle HapticTimer;
	float CurrentInterval = 0.f;
	int32 Kills = 0;
	int32 HitsTaken = 0;
	bool bGamePaused = false;
};
