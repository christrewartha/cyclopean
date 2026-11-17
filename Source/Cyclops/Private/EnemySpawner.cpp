#include "EnemySpawner.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "FlyingEnemy.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogEnemySpawner, Log, All);

AEnemySpawner::AEnemySpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// Scoreboard floats in front of the spawner, facing back towards it (text renders towards +X).
	Scoreboard = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Scoreboard"));
	Scoreboard->SetupAttachment(RootComponent);
	Scoreboard->SetRelativeLocation(FVector(600.f, 0.f, 330.f));
	Scoreboard->SetRelativeRotation(FRotator(0.f, 180.f, 0.f));
	Scoreboard->SetHorizontalAlignment(EHTA_Center);
	Scoreboard->SetWorldSize(28.f);
	Scoreboard->SetTextRenderColor(FColor(255, 200, 120));
	Scoreboard->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Scoreboard->SetCastShadow(false);
}

void AEnemySpawner::BeginPlay()
{
	Super::BeginPlay();

	UpdateScoreboard();
	if (bAutoStart)
	{
		GetWorldTimerManager().SetTimer(SpawnTimer, this, &AEnemySpawner::StartSpawning, FMath::Max(StartDelay, 0.01f), false);
	}
}

void AEnemySpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopHaptics();
	Super::EndPlay(EndPlayReason);
}

void AEnemySpawner::StartSpawning()
{
	if (!EnemyClass)
	{
		UE_LOG(LogEnemySpawner, Warning, TEXT("%s: EnemyClass not set."), *GetName());
		return;
	}
	CurrentInterval = InitialSpawnInterval;
	SpawnTick();
}

void AEnemySpawner::StopSpawning()
{
	GetWorldTimerManager().ClearTimer(SpawnTimer);
}

void AEnemySpawner::ResetSpawner()
{
	StopSpawning();
	for (const TWeakObjectPtr<AFlyingEnemy>& Enemy : AliveEnemies)
	{
		if (Enemy.IsValid())
		{
			Enemy->Destroy();
		}
	}
	AliveEnemies.Reset();
	Kills = 0;
	HitsTaken = 0;
	bGamePaused = false;
	UpdateScoreboard();
}

void AEnemySpawner::SetGamePaused(bool bPaused)
{
	if (bGamePaused == bPaused)
	{
		return;
	}
	bGamePaused = bPaused;

	FTimerManager& Timers = GetWorldTimerManager();
	if (bPaused)
	{
		Timers.PauseTimer(SpawnTimer);
	}
	else
	{
		Timers.UnPauseTimer(SpawnTimer);
	}

	for (const TWeakObjectPtr<AFlyingEnemy>& Enemy : AliveEnemies)
	{
		if (Enemy.IsValid())
		{
			Enemy->SetFrozen(bPaused);
		}
	}
}

int32 AEnemySpawner::GetCurrentMaxAlive() const
{
	return FMath::Min(MaxAliveCap, InitialMaxAlive + Kills / KillsPerExtraEnemy);
}

float AEnemySpawner::GetCurrentEnemySpeed() const
{
	return FMath::Min(MaxEnemySpeed, InitialEnemySpeed + SpeedIncreasePerKill * Kills);
}

void AEnemySpawner::ScheduleNextSpawn()
{
	GetWorldTimerManager().SetTimer(SpawnTimer, this, &AEnemySpawner::SpawnTick, CurrentInterval, false);
}

bool AEnemySpawner::PickSpawnLocation(FVector& OutLocation) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return false;
	}

	const FVector Head = Camera->GetCameraLocation();
	const float Yaw = GetActorRotation().Yaw + FMath::FRandRange(-SpawnYawHalfRange, SpawnYawHalfRange);
	const float Distance = FMath::FRandRange(SpawnDistanceMin, SpawnDistanceMax);
	const FVector Offset = FRotator(0.f, Yaw, 0.f).Vector() * Distance;

	OutLocation = FVector(Head.X + Offset.X, Head.Y + Offset.Y, GetActorLocation().Z + FMath::FRandRange(SpawnHeightMin, SpawnHeightMax));
	return true;
}

void AEnemySpawner::SpawnTick()
{
	AliveEnemies.RemoveAll([](const TWeakObjectPtr<AFlyingEnemy>& Enemy)
	{
		return !Enemy.IsValid() || Enemy->GetState() == EFlyingEnemyState::Dead;
	});

	FVector Location;
	if (AliveEnemies.Num() < GetCurrentMaxAlive() && PickSpawnLocation(Location))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = this;

		if (AFlyingEnemy* Enemy = GetWorld()->SpawnActor<AFlyingEnemy>(EnemyClass, Location, FRotator::ZeroRotator, Params))
		{
			Enemy->InitEnemy(this, GetCurrentEnemySpeed());
			AliveEnemies.Add(Enemy);
			CurrentInterval = FMath::Max(MinSpawnInterval, CurrentInterval * IntervalMultiplier);
		}
	}

	ScheduleNextSpawn();
}

void AEnemySpawner::NotifyEnemyKilled(AFlyingEnemy* Enemy)
{
	++Kills;
	UpdateScoreboard();
}

void AEnemySpawner::NotifyEnemyReachedPlayer(AFlyingEnemy* Enemy)
{
	++HitsTaken;
	UpdateScoreboard();

	if (PlayerHitSound)
	{
		UGameplayStatics::PlaySound2D(this, PlayerHitSound);
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->SetHapticsByValue(1.f, HitHapticAmplitude, EControllerHand::Left);
		PC->SetHapticsByValue(1.f, HitHapticAmplitude, EControllerHand::Right);
		GetWorldTimerManager().SetTimer(HapticTimer, this, &AEnemySpawner::StopHaptics, HitHapticDuration, false);
	}
}

void AEnemySpawner::StopHaptics()
{
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->SetHapticsByValue(0.f, 0.f, EControllerHand::Left);
		PC->SetHapticsByValue(0.f, 0.f, EControllerHand::Right);
	}
}

void AEnemySpawner::SetScoreboardVisible(bool bVisible)
{
	Scoreboard->SetVisibility(bVisible);
}

void AEnemySpawner::UpdateScoreboard()
{
	Scoreboard->SetText(FText::FromString(FString::Printf(TEXT("KILLS %d      HITS %d"), Kills, HitsTaken)));
}
