#include "VersusMatch.h"

#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "EnemySpawner.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HandGun.h"
#include "Kismet/GameplayStatics.h"
#include "MatchSubsystem.h"
#include "RemotePlayer.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogVersus, Log, All);

namespace
{
	const FVector FarAway(1.0e7, 1.0e7, 0.0); // "no opponent to avoid"

	TArray<TSharedPtr<FJsonValue>> ToJson(const FVector& V)
	{
		// One decimal (mm) is plenty and keeps messages small.
		return { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.X * 10.0) / 10.0),
				 MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Y * 10.0) / 10.0),
				 MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V.Z * 10.0) / 10.0) };
	}

	bool FromJson(const TSharedRef<FJsonObject>& Message, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Message->TryGetArrayField(Field, Values) || Values->Num() != 3)
		{
			return false;
		}
		Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		return true;
	}
}

AVersusMatch::AVersusMatch()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the HMD has updated the camera, so the pose we send is this frame's.
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RemotePlayerClass = ARemotePlayer::StaticClass();

	static ConstructorHelpers::FObjectFinder<USoundBase> Taken(TEXT("/Game/Cyclops/Audio/SW_PlayerHit.SW_PlayerHit"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Landed(TEXT("/Game/Cyclops/Audio/SW_Headshot.SW_Headshot"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Kill(TEXT("/Game/Cyclops/Audio/SW_EnemyDeath.SW_EnemyDeath"));
	HitTakenSound = Taken.Object;
	HitLandedSound = Landed.Object;
	KillSound = Kill.Object;
}

UMatchSubsystem* AVersusMatch::GetMatch() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UMatchSubsystem>() : nullptr;
}

double AVersusMatch::Now() const
{
	return GetWorld()->GetTimeSeconds();
}

void AVersusMatch::Begin(const FString& Room, const FString& Name, AEnemySpawner* InSpawner)
{
	Spawner = InSpawner;
	UMatchSubsystem* Match = GetMatch();
	if (!Match)
	{
		return;
	}
	StatusHandle = Match->OnStatusChanged.AddUObject(this, &AVersusMatch::HandleStatusChanged);
	MessageHandle = Match->OnMessage.AddUObject(this, &AVersusMatch::HandleMessage);
	Match->Join(Room, Name);
}

void AVersusMatch::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UMatchSubsystem* Match = GetMatch())
	{
		Match->OnStatusChanged.Remove(StatusHandle);
		Match->OnMessage.Remove(MessageHandle);
		Match->Leave();
	}
	if (AHandGun* Gun = BoundGun.Get())
	{
		Gun->OnShot.Remove(GunShotHandle);
		Gun->SetCanFire(true);
	}
	if (AEnemySpawner* HudSpawner = Spawner.Get())
	{
		HudSpawner->SetHudOverride(false);
	}
	if (Remote)
	{
		Remote->Destroy();
		Remote = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

// ---- Status ----

EMatchStatus AVersusMatch::GetStatus() const
{
	const UMatchSubsystem* Match = GetMatch();
	return Match ? Match->GetStatus() : EMatchStatus::Offline;
}

bool AVersusMatch::IsPaired() const
{
	const UMatchSubsystem* Match = GetMatch();
	return Match && Match->IsPaired();
}

int32 AVersusMatch::GetSlot() const
{
	const UMatchSubsystem* Match = GetMatch();
	return Match ? Match->GetSlot() : -1;
}

FString AVersusMatch::GetPeerName() const
{
	const UMatchSubsystem* Match = GetMatch();
	return Match && !Match->GetPeerName().IsEmpty() ? Match->GetPeerName() : FString(TEXT("THEM"));
}

FString AVersusMatch::DescribeStatus() const
{
	const UMatchSubsystem* Match = GetMatch();
	if (!Match)
	{
		return TEXT("Not available");
	}
	switch (Match->GetStatus())
	{
	case EMatchStatus::Connecting: return FString::Printf(TEXT("Connecting to room %s..."), *Match->GetRoom());
	case EMatchStatus::Waiting:    return FString::Printf(TEXT("Room %s: waiting for an opponent"), *Match->GetRoom());
	case EMatchStatus::Paired:
	{
		const FString Ping = Match->GetRoundTripMs() >= 0.f ? FString::Printf(TEXT("   ping %.0f ms"), Match->GetRoundTripMs()) : FString();
		return FString::Printf(TEXT("Room %s vs %s%s\n%s"), *Match->GetRoom(), *Match->GetPeerName(), *Ping, *GetScoreLine());
	}
	case EMatchStatus::Error:      return TEXT("Error: ") + Match->GetLastError();
	default:                       return TEXT("Offline");
	}
}

FString AVersusMatch::GetScoreLine() const
{
	return FString::Printf(TEXT("YOU %d : %d %s"), MyKills, TheirKills, *GetPeerName());
}

FString AVersusMatch::GetResultTitle() const
{
	return MyKills > TheirKills ? TEXT("YOU WIN") : MyKills < TheirKills ? TEXT("YOU LOSE") : TEXT("DRAW");
}

void AVersusMatch::HandleStatusChanged()
{
	if (IsPaired())
	{
		EnsureRemotePlayer();
		if (!bWasPaired)
		{
			// Both players are here (first time, or they came back): a fresh round for both.
			bWasPaired = true;
			StartRound();
			return; // StartRound broadcasts
		}
	}
	else
	{
		bWasPaired = false;
		bRoundActive = false;
		if (Remote)
		{
			// They left (or we lost the connection): take the avatar away; a new one appears if they come back.
			Remote->Destroy();
			Remote = nullptr;
		}
	}
	OnEvent.Broadcast(EVersusEvent::Status);
}

void AVersusMatch::EnsureRemotePlayer()
{
	if (Remote || !RemotePlayerClass)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Remote = GetWorld()->SpawnActor<ARemotePlayer>(RemotePlayerClass, FTransform::Identity, Params);
	if (Remote)
	{
		Remote->SetPlayerName(GetPeerName());
		Remote->OnShot.AddUObject(this, &AVersusMatch::HandleRemoteShotByUs);
	}
}

// ---- Round rules ----

void AVersusMatch::StartRound()
{
	bRoundActive = true;
	bRoundOver = false;
	bDead = false;
	bWantRematch = false;
	bPeerWantsRematch = false;
	Health = MaxHealth;
	MyKills = 0;
	TheirKills = 0;
	RoundEndTime = Now() + RoundTime;
	SpawnTime = Now();
	SetGunEnabled(true);
	if (Remote)
	{
		Remote->SetDead(false); // they start their round alive too
	}
	if (AEnemySpawner* HudSpawner = Spawner.Get())
	{
		HudSpawner->RespawnFreeRoam(GetSlot(), FarAway);
	}
	SendSimple(TEXT("spawn"));
	UE_LOG(LogVersus, Log, TEXT("Round started vs %s"), *GetPeerName());
	OnEvent.Broadcast(EVersusEvent::RoundStarted);
}

void AVersusMatch::EndRound()
{
	if (bRoundOver)
	{
		return;
	}
	bRoundOver = true;
	// The results menu is operated by shooting it, and the round usually ends with someone dead: give the gun back
	// (hits don't count between rounds).
	SetGunEnabled(true);
	UE_LOG(LogVersus, Log, TEXT("Round over: %s (%s)"), *GetResultTitle(), *GetScoreLine());
	OnEvent.Broadcast(EVersusEvent::RoundOver);
}

void AVersusMatch::CheckForWinner()
{
	if (MyKills >= KillsToWin || TheirKills >= KillsToWin)
	{
		EndRound();
	}
}

void AVersusMatch::RequestRematch()
{
	if (!bRoundOver || bWantRematch)
	{
		return;
	}
	bWantRematch = true;
	SendSimple(TEXT("rematch"));
	if (bPeerWantsRematch)
	{
		StartRound();
		return;
	}
	OnEvent.Broadcast(EVersusEvent::Status);
}

void AVersusMatch::TakeHit(float Damage, bool bHeadshot)
{
	// The victim (us) decides: no damage between rounds, while dead, or just after respawning.
	if (!bRoundActive || bRoundOver || bDead || Now() < SpawnTime + SpawnProtection)
	{
		return;
	}
	Health = FMath::Max(0.f, Health - Damage);
	UGameplayStatics::PlaySound2D(this, HitTakenSound);
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		// Both hands buzz (harder for a headshot), like being hit by an enemy.
		const float Amplitude = bHeadshot ? 1.f : 0.6f;
		PC->SetHapticsByValue(1.f, Amplitude, EControllerHand::Left);
		PC->SetHapticsByValue(1.f, Amplitude, EControllerHand::Right);
		FTimerHandle Stop;
		GetWorldTimerManager().SetTimer(Stop, FTimerDelegate::CreateWeakLambda(PC, [PC]()
		{
			PC->SetHapticsByValue(0.f, 0.f, EControllerHand::Left);
			PC->SetHapticsByValue(0.f, 0.f, EControllerHand::Right);
		}), 0.25f, false);
	}
	UE_LOG(LogVersus, Log, TEXT("They hit us (%s, %.0f) -> %.0f HP"), bHeadshot ? TEXT("head") : TEXT("body"), Damage, Health);
	if (Health <= 0.f)
	{
		Die();
	}
}

void AVersusMatch::Die()
{
	bDead = true;
	DeathTime = Now();
	++TheirKills;
	SetGunEnabled(false);
	SendSimple(TEXT("died"));
	UE_LOG(LogVersus, Log, TEXT("We died (%s)"), *GetScoreLine());
	OnEvent.Broadcast(EVersusEvent::Died);
	CheckForWinner();
}

void AVersusMatch::Respawn()
{
	bDead = false;
	Health = MaxHealth;
	SpawnTime = Now();
	if (AEnemySpawner* HudSpawner = Spawner.Get())
	{
		HudSpawner->RespawnFreeRoam(GetSlot(), GetOpponentLocation());
	}
	SetGunEnabled(true);
	SendSimple(TEXT("spawn"));
	OnEvent.Broadcast(EVersusEvent::Respawned);
}

FVector AVersusMatch::GetOpponentLocation() const
{
	return Remote && !Remote->IsHidden() ? Remote->GetActorLocation() : FarAway;
}

void AVersusMatch::SetGunEnabled(bool bEnabled)
{
	BindGun();
	if (AHandGun* Gun = BoundGun.Get())
	{
		Gun->SetCanFire(bEnabled);
		if (bEnabled)
		{
			Gun->Refill(); // new round / respawn / results menu: start with a full magazine
		}
	}
}

// ---- Tick ----

void AVersusMatch::BindGun()
{
	if (BoundGun.IsValid())
	{
		return;
	}
	// The first (only) gun in the world.
	if (TActorIterator<AHandGun> It(GetWorld()); It)
	{
		BoundGun = *It;
		GunShotHandle = It->OnShot.AddUObject(this, &AVersusMatch::HandleLocalShot);
	}
}

void AVersusMatch::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	BindGun(); // the gun can be spawned after the match

	if (bRoundActive && !bRoundOver)
	{
		if (bDead && Now() >= DeathTime + RespawnDelay)
		{
			Respawn();
		}
		if (Now() >= RoundEndTime)
		{
			EndRound();
		}
	}
	UpdateHud();

	const double Wall = FPlatformTime::Seconds();
	if (Wall - LastPoseTime >= 1.0 / PoseRate)
	{
		LastPoseTime = Wall;
		SendPose();
	}
}

void AVersusMatch::UpdateHud()
{
	AEnemySpawner* HudSpawner = Spawner.Get();
	if (!HudSpawner)
	{
		return;
	}
	if (!bRoundActive)
	{
		HudSpawner->SetHudOverride(true, GetScoreLine(), TEXT("WAITING"));
		return;
	}
	const int32 Remaining = FMath::Max(0, FMath::CeilToInt(RoundEndTime - Now()));
	const FString Time = FString::Printf(TEXT("%d:%02d"), Remaining / 60, Remaining % 60);
	const FString Right = bRoundOver ? TEXT("ROUND OVER")
		: bDead ? FString(TEXT("DOWN   ")) + Time
		: FString::Printf(TEXT("HP %.0f   %s"), Health, *Time);
	HudSpawner->SetHudOverride(true, GetScoreLine(), Right, bDead || (!bRoundOver && Health <= LowHealth));
}

void AVersusMatch::SendPose()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!IsPaired() || !Camera)
	{
		return;
	}
	const FRotator Rotation = Camera->GetComponentRotation();
	const TSharedRef<FJsonObject> Pose = MakeShared<FJsonObject>();
	Pose->SetNumberField(TEXT("tm"), FMath::RoundToDouble(FPlatformTime::Seconds() * 1000.0) / 1000.0);
	Pose->SetArrayField(TEXT("p"), ToJson(Camera->GetComponentLocation()));
	Pose->SetArrayField(TEXT("r"), ToJson(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
	Send(TEXT("pose"), Pose);
}

// ---- Messages ----

void AVersusMatch::Send(const TCHAR* Type, const TSharedRef<FJsonObject>& Message) const
{
	if (UMatchSubsystem* Match = GetMatch())
	{
		Match->Send(Type, Message);
	}
}

void AVersusMatch::SendSimple(const TCHAR* Type) const
{
	Send(Type, MakeShared<FJsonObject>());
}

void AVersusMatch::HandleLocalShot(const FVector& Muzzle, const FVector& End)
{
	const TSharedRef<FJsonObject> Shot = MakeShared<FJsonObject>();
	Shot->SetArrayField(TEXT("a"), ToJson(Muzzle));
	Shot->SetArrayField(TEXT("b"), ToJson(End));
	Send(TEXT("shot"), Shot);
}

void AVersusMatch::HandleRemoteShotByUs(float GunDamage, bool bHeadshot)
{
	if (!bRoundActive || bRoundOver || bDead)
	{
		return;
	}
	// Versus uses its own damage (the gun's is tuned for enemies).
	const float Damage = bHeadshot ? HeadDamage : BodyDamage;
	UGameplayStatics::PlaySound2D(this, HitLandedSound, bHeadshot ? 1.f : 0.6f, bHeadshot ? 1.f : 1.3f);
	const TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
	Hit->SetNumberField(TEXT("d"), Damage);
	Hit->SetNumberField(TEXT("h"), bHeadshot ? 1 : 0);
	Send(TEXT("hit"), Hit);
	UE_LOG(LogVersus, Log, TEXT("We hit them (%s, %.0f)"), bHeadshot ? TEXT("head") : TEXT("body"), Damage);
}

void AVersusMatch::HandleMessage(const FString& Type, const TSharedRef<FJsonObject>& Message)
{
	if (Type == TEXT("pose"))
	{
		FVector Location, Angles;
		double SenderTime = 0.0;
		if (Remote && Message->TryGetNumberField(TEXT("tm"), SenderTime) && FromJson(Message, TEXT("p"), Location) && FromJson(Message, TEXT("r"), Angles))
		{
			Remote->PushPose(SenderTime, Location, FRotator(Angles.X, Angles.Y, Angles.Z));
		}
	}
	else if (Type == TEXT("shot"))
	{
		FVector From, To;
		if (Remote && FromJson(Message, TEXT("a"), From) && FromJson(Message, TEXT("b"), To))
		{
			Remote->ShowShot(From, To);
		}
	}
	else if (Type == TEXT("hit"))
	{
		double Damage = BodyDamage;
		Message->TryGetNumberField(TEXT("d"), Damage);
		TakeHit(static_cast<float>(Damage), Message->GetNumberField(TEXT("h")) > 0.0);
	}
	else if (Type == TEXT("died"))
	{
		if (!bRoundActive || bRoundOver)
		{
			return;
		}
		++MyKills;
		UGameplayStatics::PlaySound2D(this, KillSound);
		if (Remote)
		{
			Remote->SetDead(true);
		}
		UE_LOG(LogVersus, Log, TEXT("We killed them (%s)"), *GetScoreLine());
		OnEvent.Broadcast(EVersusEvent::Score);
		CheckForWinner();
	}
	else if (Type == TEXT("spawn"))
	{
		if (Remote)
		{
			Remote->SetDead(false); // snap to their new position on the next pose
		}
	}
	else if (Type == TEXT("rematch"))
	{
		bPeerWantsRematch = true;
		if (bRoundOver && bWantRematch)
		{
			StartRound();
			return;
		}
		OnEvent.Broadcast(EVersusEvent::Status);
	}
}
