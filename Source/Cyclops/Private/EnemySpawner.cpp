#include "EnemySpawner.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "CyclopsEnemy.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "OpticBlastComponent.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogEnemySpawner, Log, All);

namespace
{
	// Dark plates behind the HUD texts (same look as the energy bar's border) so they read against bright sky/ground.
	// Created at runtime and found by name; owned by the spawner, attached to the HMD camera.
	const FName ScorePlateName(TEXT("HudScorePlate"));
	const FName LivesPlateName(TEXT("HudLivesPlate"));
	const FLinearColor HudPlateColour(0.015f, 0.015f, 0.02f);
	constexpr double HudPlatePadding = 1.2; // cm around the text
	constexpr double HudPlateBehind = 0.3;  // cm further from the eye than the text

	/** Sizes each plate to its text; text grows away from the bar from its anchor. */
	void LayoutHudPlates(AActor* Owner, UTextRenderComponent* Score, UTextRenderComponent* Lives, double TextSize)
	{
		auto Fit = [Owner, TextSize](UTextRenderComponent* Text, FName PlateName, double Direction)
		{
			UStaticMeshComponent* Plate = FindObjectFast<UStaticMeshComponent>(Owner, PlateName);
			if (!Plate)
			{
				return;
			}
			// Text runs away from the bar from its anchor: Direction -1 = towards the player's left.
			const FVector Size = Text->GetTextLocalSize();
			const double Width = FMath::Max(Size.X, Size.Y) + HudPlatePadding * 2.0;
			const double Height = TextSize + HudPlatePadding;
			Plate->SetRelativeLocation(Text->GetRelativeLocation() + FVector(HudPlateBehind, Direction * (Width * 0.5 - HudPlatePadding), 0.0));
			Plate->SetRelativeScale3D(FVector(0.002, Width / 100.0, Height / 100.0));
		};
		Fit(Score, ScorePlateName, -1.0);
		Fit(Lives, LivesPlateName, 1.0);
	}
}

AEnemySpawner::AEnemySpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// HUD texts: moved onto the HMD camera beside the energy bar once the pawn exists (AttachHudToCamera).
	// Unlit so the HUD ignores scene lighting and fog (the engine's default text material is lit).
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HudTextMaterial(TEXT("/Game/Cyclops/Materials/M_HudText.M_HudText"));
	auto MakeHudText = [this](FName Name, EHorizTextAligment Align)
	{
		UTextRenderComponent* Text = CreateDefaultSubobject<UTextRenderComponent>(Name);
		Text->SetupAttachment(RootComponent);
		Text->SetHorizontalAlignment(Align);
		Text->SetVerticalAlignment(EVRTA_TextCenter);
		Text->SetTextRenderColor(FColor(255, 200, 120));
		Text->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Text->SetCastShadow(false);
		Text->SetVisibility(false);
		if (HudTextMaterial.Succeeded())
		{
			Text->SetTextMaterial(HudTextMaterial.Object);
		}
		return Text;
	};
	Scoreboard = MakeHudText(TEXT("Scoreboard"), EHTA_Right);
	LivesText = MakeHudText(TEXT("LivesText"), EHTA_Left);
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
	bRunActive = true;
	RunStartTime = GetWorld()->GetTimeSeconds();
	TotalPausedTime = 0.0;
	OnRunStarted();
	SpawnTick();
}

void AEnemySpawner::StopSpawning()
{
	GetWorldTimerManager().ClearTimer(SpawnTimer);
}

void AEnemySpawner::ResetSpawner()
{
	StopSpawning();
	DestroyAliveEnemies();
	Kills = 0;
	HitsTaken = 0;
	Score = 0;
	Streak = 0;
	Lives = MaxLives;
	bRunActive = false;
	bGamePaused = false;
	OnRunStopped();
	UpdateScoreboard();
}

void AEnemySpawner::DestroyAliveEnemies()
{
	for (const TWeakObjectPtr<AActor>& Enemy : AliveEnemies)
	{
		if (Enemy.IsValid())
		{
			Enemy->Destroy();
		}
	}
	AliveEnemies.Reset();
}

bool AEnemySpawner::GetPlayerHead(FVector& OutLocation) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return false;
	}
	OutLocation = Camera->GetCameraLocation();
	return true;
}

void AEnemySpawner::SetGamePaused(bool bPaused)
{
	if (bGamePaused == bPaused)
	{
		return;
	}
	bGamePaused = bPaused;

	FTimerManager& Timers = GetWorldTimerManager();
	const double Now = GetWorld()->GetTimeSeconds();
	if (bPaused)
	{
		Timers.PauseTimer(SpawnTimer);
		PausedSince = Now;
	}
	else
	{
		Timers.UnPauseTimer(SpawnTimer);
		TotalPausedTime += Now - PausedSince;
	}

	for (const TWeakObjectPtr<AActor>& Enemy : AliveEnemies)
	{
		if (ICyclopsEnemy* AsEnemy = Cast<ICyclopsEnemy>(Enemy.Get()))
		{
			AsEnemy->SetFrozen(bPaused);
		}
	}
	OnPausedChanged(bPaused);
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

int32 AEnemySpawner::CountAliveOfClass(const UClass* Class) const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AActor>& Enemy : AliveEnemies)
	{
		const ICyclopsEnemy* AsEnemy = Cast<ICyclopsEnemy>(Enemy.Get());
		Count += AsEnemy && !AsEnemy->IsDead() && Class && Enemy->IsA(Class) ? 1 : 0;
	}
	return Count;
}

bool AEnemySpawner::PickSpawnLocation(TSubclassOf<AActor> Class, FVector& OutLocation, FRotator& OutRotation)
{
	FVector Head;
	if (!GetPlayerHead(Head))
	{
		return false;
	}
	OutRotation = FRotator::ZeroRotator;

	const float Yaw = GetActorRotation().Yaw + FMath::FRandRange(-SpawnYawHalfRange, SpawnYawHalfRange);
	const float Distance = FMath::FRandRange(SpawnDistanceMin, SpawnDistanceMax);
	const FVector Offset = FRotator(0.f, Yaw, 0.f).Vector() * Distance;

	OutLocation = FVector(Head.X + Offset.X, Head.Y + Offset.Y, GetActorLocation().Z + FMath::FRandRange(SpawnHeightMin, SpawnHeightMax));
	return true;
}

void AEnemySpawner::SpawnTick()
{
	AliveEnemies.RemoveAll([](const TWeakObjectPtr<AActor>& Enemy)
	{
		const ICyclopsEnemy* AsEnemy = Cast<ICyclopsEnemy>(Enemy.Get());
		return !AsEnemy || AsEnemy->IsDead();
	});

	FVector Location;
	FRotator Rotation;
	const TSubclassOf<AActor> Class = AliveEnemies.Num() < GetCurrentMaxAlive() ? PickEnemyClass() : nullptr;
	if (Class && PickSpawnLocation(Class, Location, Rotation))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = this;

		AActor* Enemy = GetWorld()->SpawnActor<AActor>(Class, Location, Rotation, Params);
		if (ICyclopsEnemy* AsEnemy = Cast<ICyclopsEnemy>(Enemy))
		{
			AsEnemy->InitEnemy(this, GetSpawnSpeed(Class));
			AliveEnemies.Add(Enemy);
			CurrentInterval = FMath::Max(MinSpawnInterval, CurrentInterval * IntervalMultiplier);
		}
	}

	ScheduleNextSpawn();
}

int32 AEnemySpawner::GetMultiplier() const
{
	// Consecutive kills without being hit: x2 from 5, x3 from 10, x4 from 20.
	return Streak >= 20 ? 4 : (Streak >= 10 ? 3 : (Streak >= 5 ? 2 : 1));
}

void AEnemySpawner::NotifyEnemyKilled(AActor* Enemy, bool bOneShot, bool bHeadshot)
{
	if (!bRunActive)
	{
		return;
	}
	++Kills;
	++Streak;
	Score += (PointsPerKill + (bOneShot ? OneShotBonus : 0) + (bHeadshot ? HeadshotBonus : 0)) * GetMultiplier();
	UpdateScoreboard();
}

void AEnemySpawner::NotifyEnemyReachedPlayer(AActor* Enemy)
{
	if (!bRunActive)
	{
		return;
	}
	++HitsTaken;
	Streak = 0;
	Lives = FMath::Max(0, Lives - 1);
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

	if (Lives <= 0)
	{
		EndRun();
	}
}

float AEnemySpawner::GetRunDuration() const
{
	const double Now = GetWorld()->GetTimeSeconds();
	const double Paused = TotalPausedTime + (bGamePaused ? Now - PausedSince : 0.0);
	return float(FMath::Max(0.0, Now - RunStartTime - Paused));
}

void AEnemySpawner::EndRun(bool bCompleted, int32 FinishBonus)
{
	FRunResult Result;
	Result.Score = Score;
	Result.Kills = Kills;
	Result.Duration = GetRunDuration();
	Result.bCompleted = bCompleted;
	Result.FinishBonus = FinishBonus;

	bRunActive = false;
	StopSpawning();
	DestroyAliveEnemies();
	OnRunStopped();

	OnRunEnded.Broadcast(Result);
}

void AEnemySpawner::CompleteRun(int32 FinishBonus)
{
	if (!bRunActive)
	{
		return;
	}
	Score += FinishBonus;
	UpdateScoreboard();
	EndRun(/*bCompleted*/ true, FinishBonus);
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
	bHudVisible = bVisible;
	AttachHudToCamera();
	const bool bShow = bHudVisible && bHudAttached;
	Scoreboard->SetVisibility(bShow);
	LivesText->SetVisibility(bShow);
	for (const FName Name : { ScorePlateName, LivesPlateName })
	{
		if (UStaticMeshComponent* Plate = FindObjectFast<UStaticMeshComponent>(this, Name))
		{
			Plate->SetVisibility(bShow);
		}
	}
}

bool AEnemySpawner::AttachHudToCamera()
{
	if (bHudAttached)
	{
		return true;
	}
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!Camera)
	{
		return false;
	}

	// Line up with the energy bar (camera space: X forward, Y right, Z up).
	FVector BarCentre(55.0, 0.0, -21.0);
	double BarHalfWidth = 11.0;
	const UOpticBlastComponent* Beam = nullptr;
	for (TActorIterator<AActor> It(GetWorld()); It && !Beam; ++It)
	{
		Beam = It->FindComponentByClass<UOpticBlastComponent>();
	}
	if (Beam)
	{
		BarCentre = Beam->EnergyBarOffset;
		BarHalfWidth = Beam->EnergyBarSize.X * 0.5;
	}

	// Yaw 180 turns the text to face the eye; right-aligned grows to the player's left, left-aligned to the right.
	const double Side = BarHalfWidth + HudGap;
	const FRotator FacingEye(0.f, 180.f, 0.f);
	for (UTextRenderComponent* Text : { Scoreboard.Get(), LivesText.Get() })
	{
		Text->AttachToComponent(Camera, FAttachmentTransformRules::KeepRelativeTransform);
		Text->SetRelativeRotation(FacingEye);
		Text->SetRelativeScale3D(FVector::OneVector);
		Text->SetWorldSize(HudTextSize);
	}
	Scoreboard->SetRelativeLocation(BarCentre + FVector(0.0, -Side, 0.0));
	LivesText->SetRelativeLocation(BarCentre + FVector(0.0, Side, 0.0));

	if (Beam && Beam->EnergyBarMesh)
	{
		for (const FName Name : { ScorePlateName, LivesPlateName })
		{
			UStaticMeshComponent* Plate = NewObject<UStaticMeshComponent>(this, Name);
			Plate->SetStaticMesh(Beam->EnergyBarMesh);
			Plate->SetMaterial(0, Beam->EnergyBarMaterial);
			Plate->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Plate->SetGenerateOverlapEvents(false);
			Plate->SetCastShadow(false);
			Plate->RegisterComponent();
			Plate->AttachToComponent(Camera, FAttachmentTransformRules::KeepRelativeTransform);
			Plate->SetCustomPrimitiveDataVector4(0, FVector4(HudPlateColour.R, HudPlateColour.G, HudPlateColour.B, 1.0));
		}
	}
	bHudAttached = true;
	LayoutHudPlates(this, Scoreboard, LivesText, HudTextSize);
	return true;
}

void AEnemySpawner::UpdateScoreboard()
{
	const int32 Multiplier = GetMultiplier();
	const FString Combo = Multiplier > 1 ? FString::Printf(TEXT("  x%d"), Multiplier) : FString();
	Scoreboard->SetText(FText::FromString(FString::Printf(TEXT("SCORE %d%s"), Score, *Combo)));
	LivesText->SetText(FText::FromString(FString::Printf(TEXT("LIVES %d"), Lives)));
	// Last life reads red.
	LivesText->SetTextRenderColor(Lives <= 1 ? FColor(255, 70, 50) : FColor(255, 200, 120));
	LayoutHudPlates(this, Scoreboard, LivesText, HudTextSize);
	if (!bHudAttached && AttachHudToCamera())
	{
		SetScoreboardVisible(bHudVisible);
	}
}
