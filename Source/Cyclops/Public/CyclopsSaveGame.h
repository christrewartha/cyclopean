#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "CyclopsSaveGame.generated.h"

/** A finished run that hasn't reached the leaderboard yet (offline at the time). */
USTRUCT()
struct FPendingScore
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() int32 Score = 0;
	UPROPERTY() int32 Kills = 0;
	UPROPERTY() float Duration = 0.f;
	UPROPERTY() FString Mode;   // empty = arena (saves from before levels existed)
};

/** Per-headset progress: personal best, arcade initials and unsent scores. */
UCLASS()
class CYCLOPS_API UCyclopsSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("Cyclopean");

	/** Loads the save, or creates an empty one. */
	static UCyclopsSaveGame* LoadOrCreate();
	void Save();

	/** Personal best for a level (leaderboard mode). */
	int32 GetBest(const FString& Mode) const;
	void SetBest(const FString& Mode, int32 Score);

	/** Arena best from saves before per-level bests; read through GetBest("arena"). */
	UPROPERTY() int32 BestScore = 0;

	UPROPERTY() TMap<FString, int32> BestScores;

	/** 3 characters A-Z / 0-9; empty until the player first enters them. */
	UPROPERTY() FString Initials;

	UPROPERTY() TArray<FPendingScore> PendingScores;

	/** Last versus room code typed (offered again next time). */
	UPROPERTY() FString LastRoom;

	/** Last weapon chosen on a level that offers the hand gun (true = gun, false = optic blast). */
	UPROPERTY() bool bPreferGun = false;
};
