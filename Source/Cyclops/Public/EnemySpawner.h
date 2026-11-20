#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EnemySpawner.generated.h"

class USoundBase;
class UBodyLockComponent;
class UTextRenderComponent;

/** Final stats of a run, broadcast when the player runs out of lives. */
struct FRunResult
{
	int32 Score = 0;        // includes FinishBonus
	int32 Kills = 0;
	float Duration = 0.f;   // seconds of play, excluding pauses
	bool bCompleted = false; // reached the level's goal (vs ran out of lives)
	int32 FinishBonus = 0;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnRunEnded, const FRunResult&);

/**
 * Runs a level's game: spawns enemies (anything implementing ICyclopsEnemy), keeps score and lives, drives the
 * head-locked HUD and reports the end of the run. This base class is the arena rules (flying enemies around the
 * player); subclasses such as AStreetSpawner override the spawn placement / difficulty hooks.
 *
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

	/** bOneShot: killed by a single hit (the opening burst) - earns the accuracy bonus. bHeadshot: the killing hit was a headshot. */
	void NotifyEnemyKilled(AActor* Enemy, bool bOneShot, bool bHeadshot = false);
	void NotifyEnemyReachedPlayer(AActor* Enemy);

	UFUNCTION(BlueprintPure, Category = "Spawner")
	bool IsRunActive() const { return bRunActive; }

	/** Fires once when lives reach zero; the spawner has already stopped and cleared enemies. */
	FOnRunEnded OnRunEnded;

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetKills() const { return Kills; }

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetHitsTaken() const { return HitsTaken; }

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetScore() const { return Score; }

	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetLives() const { return Lives; }

	/** Current combo multiplier (1-4) from consecutive kills without being hit. */
	UFUNCTION(BlueprintPure, Category = "Spawner")
	int32 GetMultiplier() const;

	// ---- Level ----

	/** Leaderboard / personal-best key for this level ("arena", "streets"). Must match the server's MODES. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner|Level")
	FString LeaderboardMode = TEXT("arena");

	/** Shown on menus (title, leaderboard header). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner|Level")
	FString ModeName = TEXT("ARENA");

	/**
	 * Whether the title offers the hand gun as an alternative to the optic blast. Gun runs get their own board:
	 * LeaderboardMode + "_gun" (which must also be in the server's MODES).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner|Level")
	bool bAllowGun = false;

	// ---- Rules ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Rules", meta = (ClampMin = "1"))
	int32 MaxLives = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Rules", meta = (ClampMin = "0"))
	int32 PointsPerKill = 100;

	/** Extra points for a one-shot kill.
	 *  Keep (PointsPerKill + OneShotBonus + HeadshotBonus) x 4 (max combo) in sync with the server's MAX_POINTS_PER_KILL. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Rules", meta = (ClampMin = "0"))
	int32 OneShotBonus = 50;

	/** Extra points when the killing hit was a headshot (walkers). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|Rules", meta = (ClampMin = "0"))
	int32 HeadshotBonus = 100;

	/** Any actor class implementing ICyclopsEnemy. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawner", meta = (MustImplement = "/Script/Cyclops.CyclopsEnemy"))
	TSubclassOf<AActor> EnemyClass;

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

	// ---- HUD (body-locked: low in front of the player, glance down to read; see UBodyLockComponent) ----

	/** Text height (the HUD sits ~80 cm from the eyes). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|HUD", meta = (Units = "cm"))
	float HudTextSize = 3.2f;

	/** Gap either side of the HUD's centre line between score (left) and lives (right). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner|HUD", meta = (Units = "cm"))
	float HudGap = 2.f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- Hooks for level-specific rules ----

	/** Which enemy to spawn next (default: EnemyClass). Null = skip this spawn tick. */
	virtual TSubclassOf<AActor> PickEnemyClass() { return EnemyClass; }
	/** Where (and facing which way) the next enemy of that class appears; false = skip this spawn tick. */
	virtual bool PickSpawnLocation(TSubclassOf<AActor> Class, FVector& OutLocation, FRotator& OutRotation);
	virtual int32 GetCurrentMaxAlive() const;
	virtual float GetCurrentEnemySpeed() const;
	/** Speed handed to a newly spawned enemy of this class (default: GetCurrentEnemySpeed). */
	virtual float GetSpawnSpeed(TSubclassOf<AActor> Class) const { return GetCurrentEnemySpeed(); }
	/** Live (not dead) enemies of a class or its subclasses. */
	int32 CountAliveOfClass(const UClass* Class) const;
	virtual void OnRunStarted() {}
	/** Run ended (lives gone / goal reached) or reset to the title. */
	virtual void OnRunStopped() {}
	virtual void OnPausedChanged(bool bPaused) {}

	/** Ends the run as a success, adding FinishBonus to the score. */
	void CompleteRun(int32 FinishBonus);
	float GetRunDuration() const;
	/** Player's eye position, or false if there's no camera yet. */
	bool GetPlayerHead(FVector& OutLocation) const;

	int32 Kills = 0;
	int32 Score = 0;
	int32 Lives = 5;
	bool bRunActive = false;
	bool bGamePaused = false;

private:
	void SpawnTick();
	void ScheduleNextSpawn();
	void UpdateScoreboard();
	void StopHaptics();
	/** Moves the HUD texts onto the body-locked anchor; false until the pawn exists. */
	bool AttachHudToCamera();

	/** Score + combo, left of centre. */
	UPROPERTY(VisibleAnywhere, Category = "Spawner")
	TObjectPtr<UTextRenderComponent> Scoreboard;

	/** Lives, right of centre. */
	UPROPERTY(VisibleAnywhere, Category = "Spawner")
	TObjectPtr<UTextRenderComponent> LivesText;

	/** Follows the player low and in front; the HUD texts and plates ride on it. */
	UPROPERTY(VisibleAnywhere, Category = "Spawner")
	TObjectPtr<UBodyLockComponent> HudAnchor;

	bool bHudAttached = false;
	bool bHudVisible = true;

	TArray<TWeakObjectPtr<AActor>> AliveEnemies;
	FTimerHandle SpawnTimer;
	FTimerHandle HapticTimer;
	float CurrentInterval = 0.f;
	int32 HitsTaken = 0;
	int32 Streak = 0;          // kills since last hit
	double RunStartTime = 0.0;
	double PausedSince = 0.0;
	double TotalPausedTime = 0.0;

	void EndRun(bool bCompleted = false, int32 FinishBonus = 0);
	void DestroyAliveEnemies();
};
