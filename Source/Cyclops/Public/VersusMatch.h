#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VersusMatch.generated.h"

class AEnemySpawner;
class AHandGun;
class ARemotePlayer;
class FJsonObject;
class UMatchSubsystem;
class USoundBase;
enum class EMatchStatus : uint8;

/** What changed, for the menus (AGameFlowManager). */
enum class EVersusEvent : uint8
{
	Status,       // connection / pairing / rematch requests changed
	Score,        // a kill happened (or hit counts changed)
	Died,         // we were killed
	Respawned,    // we're back
	RoundStarted, // fresh round: scores 0, full health, at our start
	RoundOver,
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnVersusEvent, EVersusEvent);

/**
 * One versus match on this headset (spawned by AGameFlowManager): joins a room through UMatchSubsystem, shows the
 * other player as an ARemotePlayer and runs the round rules.
 *
 * Authority is split the simple way: the SHOOTER decides whether a shot hit (what you saw is what counts) and sends
 * the damage; the VICTIM owns its own health, decides when it dies and tells the other side, then respawns itself.
 * Both sides count kills from the same messages, so the scores agree. Rounds start when both players are in the room
 * (and again on a rematch both have asked for) and end at KillsToWin kills or after RoundTime.
 *
 * Messages (relayed by the server unchanged):
 *   pose     {tm: sender seconds, p: [x,y,z] head, r: [pitch,yaw,roll]}   PoseRate per second
 *   shot     {a: [x,y,z] muzzle, b: [x,y,z] end}                           tracer for the other side
 *   hit      {d: damage, h: 1 if headshot}                                 shooter -> victim
 *   died     {}                                                            victim -> shooter (a kill for them)
 *   spawn    {}                                                            victim respawned (snap, don't slide)
 *   rematch  {}                                                            wants another round
 */
UCLASS()
class CYCLOPS_API AVersusMatch : public AActor
{
	GENERATED_BODY()

public:
	AVersusMatch();

	/** Connects and joins Room as Name. Spawner places the player and carries the HUD text. */
	void Begin(const FString& Room, const FString& Name, AEnemySpawner* InSpawner);

	/** Ask for another round after one has ended; starts once the other player asks too. */
	void RequestRematch();

	EMatchStatus GetStatus() const;
	bool IsPaired() const;
	int32 GetSlot() const;
	FString GetPeerName() const;
	/** Connection line for the menu: connecting / waiting / vs XYZ (ping) / error. */
	FString DescribeStatus() const;
	/** "YOU 2 : 1 BOT" */
	FString GetScoreLine() const;
	/** "YOU WIN" / "YOU LOSE" / "DRAW" for a finished round. */
	FString GetResultTitle() const;

	bool IsRoundOver() const { return bRoundOver; }
	bool IsDead() const { return bDead; }
	bool WantsRematch() const { return bWantRematch; }
	bool PeerWantsRematch() const { return bPeerWantsRematch; }

	FOnVersusEvent OnEvent;

	// ---- Rules ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "1"))
	float MaxHealth = 100.f;

	/** Damage our hits do (sent to the victim). 4 body shots or 2 headshots at 100 HP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "0"))
	float BodyDamage = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "0"))
	float HeadDamage = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "0", Units = "s"))
	float RespawnDelay = 3.f;

	/** After respawning, hits are ignored for this long (no dying the moment you reappear). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "0", Units = "s"))
	float SpawnProtection = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "1"))
	int32 KillsToWin = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "10", Units = "s"))
	float RoundTime = 180.f;

	/** Health at or below this reads red on the HUD. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus|Rules", meta = (ClampMin = "0"))
	float LowHealth = 30.f;

	// ---- Network / presentation ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Versus", meta = (ClampMin = "5", ClampMax = "60"))
	float PoseRate = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Versus")
	TSubclassOf<ARemotePlayer> RemotePlayerClass;

	/** Played when the other player hits us. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Versus")
	TObjectPtr<USoundBase> HitTakenSound;

	/** Played when we hit them (confirmation). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Versus")
	TObjectPtr<USoundBase> HitLandedSound;

	/** Played when we kill them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Versus")
	TObjectPtr<USoundBase> KillSound;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UMatchSubsystem* GetMatch() const;
	void Send(const TCHAR* Type, const TSharedRef<FJsonObject>& Message) const;
	void SendSimple(const TCHAR* Type) const;
	void HandleStatusChanged();
	void HandleMessage(const FString& Type, const TSharedRef<FJsonObject>& Message);
	void HandleLocalShot(const FVector& Muzzle, const FVector& End);
	void HandleRemoteShotByUs(float Damage, bool bHeadshot);
	void TakeHit(float Damage, bool bHeadshot);
	void Die();
	void Respawn();
	void StartRound();
	void EndRound();
	void CheckForWinner();
	void SendPose();
	void UpdateHud();
	void EnsureRemotePlayer();
	void BindGun();
	void SetGunEnabled(bool bEnabled);
	FVector GetOpponentLocation() const;
	double Now() const;

	UPROPERTY(Transient)
	TObjectPtr<ARemotePlayer> Remote;

	TWeakObjectPtr<AEnemySpawner> Spawner;
	TWeakObjectPtr<AHandGun> BoundGun;
	FDelegateHandle StatusHandle;
	FDelegateHandle MessageHandle;
	FDelegateHandle GunShotHandle;
	double LastPoseTime = 0.0;

	// Round state.
	float Health = 0.f;
	int32 MyKills = 0;
	int32 TheirKills = 0;
	double RoundEndTime = 0.0;
	double DeathTime = 0.0;
	double SpawnTime = 0.0;
	bool bRoundActive = false;  // a round has started (and the other player is here)
	bool bRoundOver = false;
	bool bDead = false;
	bool bWantRematch = false;
	bool bPeerWantsRematch = false;
	bool bWasPaired = false;
};
