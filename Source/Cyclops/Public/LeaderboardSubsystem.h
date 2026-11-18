#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "LeaderboardSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FLeaderboardEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Leaderboard") int32 Rank = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Leaderboard") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Leaderboard") int32 Score = 0;
};

/** Result of a fetch or submit. bOnline is false if the service couldn't be reached / isn't configured. */
struct FLeaderboardResult
{
	bool bOnline = false;
	TArray<FLeaderboardEntry> Top;
	int32 Rank = 0;   // submit only: the player's rank among players' bests
};

DECLARE_DELEGATE_OneParam(FOnLeaderboardResult, const FLeaderboardResult&);

/**
 * Talks to the Cyclopean leaderboard service (Server/leaderboard). Submissions are signed with
 * HMAC-SHA1 of the request body using CYCLOPS_LEADERBOARD_KEY. Runs that can't be sent (offline)
 * are queued in the save game and retried on the next submit or fetch.
 */
UCLASS()
class CYCLOPS_API ULeaderboardSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	bool IsConfigured() const;

	/** Top 10 (one entry per player, their best). */
	void FetchTop(FOnLeaderboardResult OnDone);

	/** Submits a finished run (queues it if offline), then reports the updated top 10. */
	void SubmitScore(const FString& Name, int32 Score, int32 Kills, float Duration, FOnLeaderboardResult OnDone);

private:
	void SendPendingScores();
	void PostScore(const FString& Name, int32 Score, int32 Kills, float Duration, TFunction<void(bool bAccepted, const FLeaderboardResult&)> OnDone);
	static bool ParseTop(const TSharedPtr<class FJsonObject>& Json, TArray<FLeaderboardEntry>& OutTop);
	static FString Sign(const FString& Body);

	bool bFlushingPending = false;
};
