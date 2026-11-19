#include "WalkingEnemy.h"

#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnemyRock.h"
#include "EnemySpawner.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Body layout (cm), relative to the feet. ~185 cm tall.
	constexpr float CapsuleRadius = 34.f;
	constexpr float CapsuleHalfHeight = 92.f;
	constexpr float HipHeight = 95.f;
	constexpr float HipSpacing = 12.f;
	constexpr float ShoulderHeight = 150.f;
	constexpr float ShoulderSpacing = 27.f;
	constexpr float LegLength = 90.f;
	constexpr float ArmLength = 65.f;

	constexpr float MaxLegSwing = 32.f;   // degrees at full walk speed
	constexpr float MaxArmSwing = 26.f;
	constexpr float WindupArmPitch = 150.f; // arms raised forward and overhead (positive pitch swings a hanging arm forward)
	constexpr float StrikeReachFactor = 1.5f; // still in range if within this x AttackRange when the wind-up ends

	template <typename T>
	T* FindAsset(const TCHAR* Path)
	{
		ConstructorHelpers::FObjectFinder<T> Finder(Path);
		return Finder.Object;
	}
}

AWalkingEnemy::AWalkingEnemy()
{
	PrimaryActorTick.bCanEverTick = true;

	CubeMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Cube.Cube"));
	SphereMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	BodyMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_EnemyBody.MI_EnemyBody"));
	EyeMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_EnemyEyes.MI_EnemyEyes"));
	AnnounceSound = FindAsset<USoundBase>(TEXT("/Game/Cyclops/Audio/SW_WalkerMoan.SW_WalkerMoan"));
	DeathSound = FindAsset<USoundBase>(TEXT("/Game/Cyclops/Audio/SW_EnemyDeath.SW_EnemyDeath"));
	HeadshotSound = FindAsset<USoundBase>(TEXT("/Game/Cyclops/Audio/SW_Headshot.SW_Headshot"));
	AttackSound = FindAsset<USoundBase>(TEXT("/Game/Cyclops/Audio/SW_WalkerAttack.SW_WalkerAttack"));

	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
	// The optic blast traces the Visibility channel.
	Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

	// No skeletal mesh: the body is built from primitives below.
	GetMesh()->SetVisibility(false);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.f, 300.f, 0.f);
	Movement->MaxWalkSpeed = 150.f;
	// Steer round each other instead of queuing in single file through a corner.
	Movement->bUseRVOAvoidance = true;
	Movement->AvoidanceConsiderationRadius = 350.f;
	Movement->AvoidanceWeight = 0.5f;
	bUseControllerRotationYaw = false;

	AIControllerClass = AAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	VisualRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VisualRoot"));
	VisualRoot->SetupAttachment(Capsule);
	VisualRoot->SetRelativeLocation(FVector(0.f, 0.f, -CapsuleHalfHeight)); // feet

	Torso = MakePart(TEXT("Torso"), VisualRoot, FVector(0.f, 0.f, 125.f), FVector(0.24f, 0.40f, 0.60f));
	Head = MakePart(TEXT("Head"), VisualRoot, FVector(0.f, 0.f, 172.f), FVector(0.30f));
	Eye = MakePart(TEXT("Eye"), VisualRoot, FVector(13.f, 0.f, 174.f), FVector(0.11f));

	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Sign = Side == 0 ? -1.f : 1.f;

		USceneComponent* LegPivot = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("LegPivot%d"), Side));
		LegPivot->SetupAttachment(VisualRoot);
		LegPivot->SetRelativeLocation(FVector(0.f, HipSpacing * Sign, HipHeight));
		LegPivots.Add(LegPivot);
		Legs.Add(MakePart(*FString::Printf(TEXT("Leg%d"), Side), LegPivot, FVector(0.f, 0.f, -LegLength * 0.5f), FVector(0.14f, 0.14f, LegLength / 100.f)));

		USceneComponent* ArmPivot = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("ArmPivot%d"), Side));
		ArmPivot->SetupAttachment(VisualRoot);
		ArmPivot->SetRelativeLocation(FVector(0.f, ShoulderSpacing * Sign, ShoulderHeight));
		ArmPivots.Add(ArmPivot);
		Arms.Add(MakePart(*FString::Printf(TEXT("Arm%d"), Side), ArmPivot, FVector(0.f, 0.f, -ArmLength * 0.5f), FVector(0.11f, 0.11f, ArmLength / 100.f)));
	}

	// Rock in the right hand, only shown by throwers while winding up.
	HeldRock = MakePart(TEXT("HeldRock"), ArmPivots[1], FVector(0.f, 0.f, -ArmLength - 8.f), FVector(0.28f));
	HeldRock->SetVisibility(false);

	VoiceAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("VoiceAudio"));
	VoiceAudio->SetupAttachment(Head);
	VoiceAudio->bAutoActivate = false;
}

UStaticMeshComponent* AWalkingEnemy::MakePart(FName Name, USceneComponent* Parent, const FVector& Location, const FVector& Scale)
{
	UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
	Part->SetupAttachment(Parent);
	Part->SetRelativeLocation(Location);
	Part->SetRelativeScale3D(Scale);
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision); // the capsule takes the hits
	Part->SetGenerateOverlapEvents(false);
	Part->SetCastShadow(false);
	return Part;
}

void AWalkingEnemy::ApplyMeshesAndMaterials()
{
	for (UStaticMeshComponent* Part : { Torso.Get(), Legs[0].Get(), Legs[1].Get(), Arms[0].Get(), Arms[1].Get() })
	{
		Part->SetStaticMesh(CubeMesh);
		Part->SetMaterial(0, BodyMaterial);
	}
	Head->SetStaticMesh(SphereMesh);
	Head->SetMaterial(0, BodyMaterial);
	Eye->SetStaticMesh(SphereMesh);
	Eye->SetMaterial(0, EyeMaterial);
	HeldRock->SetStaticMesh(SphereMesh);
	HeldRock->SetMaterial(0, RockMaterial ? RockMaterial.Get() : EyeMaterial.Get());
}

void AWalkingEnemy::BeginPlay()
{
	Super::BeginPlay();
	ApplyMeshesAndMaterials();
	Health = MaxHealth;
	State = EWalkingEnemyState::Emerging;
	StateTime = 0.f;
	VisualRoot->SetRelativeScale3D(FVector(0.05f));
	SetFlash(1.f);
	if (AnnounceSound)
	{
		VoiceAudio->SetSound(AnnounceSound);
		VoiceAudio->SetPitchMultiplier(FMath::FRandRange(1.f - AnnouncePitchVariation, 1.f + AnnouncePitchVariation));
		VoiceAudio->Play();
	}
}

void AWalkingEnemy::InitEnemy(AEnemySpawner* InSpawner, float InSpeed)
{
	Spawner = InSpawner;
	// A little speed spread so a group doesn't move as one block.
	GetCharacterMovement()->MaxWalkSpeed = InSpeed * FMath::FRandRange(1.f - SpeedVariation, 1.f + SpeedVariation);
	// Different enemies step out of sync.
	WalkPhase = FMath::FRandRange(0.f, 2.f * PI);

	// Approach style, a bit like the Pac-Man ghosts: chase, flank, or cut the player off.
	const float Roll = FMath::FRand();
	Style = Roll < ChaserWeight ? EWalkerStyle::Chaser : (Roll < ChaserWeight + FlankerWeight ? EWalkerStyle::Flanker : EWalkerStyle::Ambusher);
	FlankSide = (FMath::RandBool() ? 1.f : -1.f) * FMath::FRandRange(0.75f, 1.25f);
	DetourTimer = 0.f;
}

bool AWalkingEnemy::GetPlayerHead(FVector& OutLocation) const
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

void AWalkingEnemy::SetFrozen(bool bInFrozen)
{
	if (bFrozen == bInFrozen || State == EWalkingEnemyState::Dead)
	{
		return;
	}
	bFrozen = bInFrozen;
	if (bFrozen)
	{
		if (AAIController* AI = Cast<AAIController>(GetController()))
		{
			AI->StopMovement();
		}
		GetCharacterMovement()->StopMovementImmediately();
	}
	RepathTimer = 0.f; // re-aim straight away when unfrozen
	VoiceAudio->SetPaused(bFrozen);
}

void AWalkingEnemy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bFrozen || State == EWalkingEnemyState::Dead)
	{
		return;
	}

	StateTime += DeltaSeconds;
	if (Flash > 0.f)
	{
		SetFlash(FMath::Max(0.f, Flash - DeltaSeconds * 5.f));
	}

	FVector PlayerHead;
	if (!GetPlayerHead(PlayerHead))
	{
		return;
	}
	const FVector ToPlayer = FVector(PlayerHead.X, PlayerHead.Y, 0.f) - FVector(GetActorLocation().X, GetActorLocation().Y, 0.f);
	const float Distance = ToPlayer.Size();

	switch (State)
	{
	case EWalkingEnemyState::Emerging:
	{
		const float Alpha = FMath::Clamp(StateTime / EmergeTime, 0.f, 1.f);
		VisualRoot->SetRelativeScale3D(FVector(FMath::InterpEaseOut(0.05f, 1.f, Alpha, 3.f)));
		SetActorRotation(FRotator(0.f, ToPlayer.Rotation().Yaw, 0.f));
		if (Alpha >= 1.f)
		{
			State = EWalkingEnemyState::Walking;
			StateTime = 0.f;
			RepathTimer = 0.f;
			ThrowCooldownLeft = FMath::FRandRange(0.8f, 2.f); // don't throw the instant it appears
		}
		break;
	}
	case EWalkingEnemyState::Throwing:
		TickThrow(DeltaSeconds, PlayerHead, Distance);
		break;
	case EWalkingEnemyState::Walking:
		if (Distance <= AttackRange)
		{
			if (AAIController* AI = Cast<AAIController>(GetController()))
			{
				AI->StopMovement();
			}
			State = EWalkingEnemyState::Attacking;
			StateTime = 0.f;
			// The only warning is audible: by the time you hear it, it's too late to line up a shot.
			if (AttackSound)
			{
				UGameplayStatics::PlaySoundAtLocation(this, AttackSound, Head->GetComponentLocation(), 1.f, FMath::FRandRange(0.9f, 1.1f));
			}
			break;
		}
		ThrowCooldownLeft -= DeltaSeconds;
		if (bCanThrow && RockClass && ThrowCooldownLeft <= 0.f && Distance >= ThrowRange.X && Distance <= ThrowRange.Y
			&& CanSeePlayer(PlayerHead))
		{
			StartThrow();
			break;
		}
		RepathTimer -= DeltaSeconds;
		DetourTimer -= DeltaSeconds;
		if (RepathTimer <= 0.f)
		{
			RepathTimer = RepathInterval;
			if (AAIController* AI = Cast<AAIController>(GetController()))
			{
				const FVector Feet(PlayerHead.X, PlayerHead.Y, GetActorLocation().Z - CapsuleHalfHeight);
				const FVector Goal = PickApproachTarget(Feet, Distance);
				const EPathFollowingRequestResult::Type Result = AI->MoveToLocation(Goal, AttackRange * 0.5f, /*bStopOnOverlap*/ false,
					/*bUsePathfinding*/ true, /*bProjectDestinationToNavigation*/ true, /*bCanStrafe*/ false);
				if (Result == EPathFollowingRequestResult::Failed && !Goal.Equals(Feet))
				{
					// Offset point inside a building / off the navmesh: just come straight in this time.
					AI->MoveToLocation(Feet, AttackRange * 0.5f, false, true, true, false);
				}
			}
		}
		UpdateWalkCycle(DeltaSeconds);
		break;

	case EWalkingEnemyState::Attacking:
	{
		// Face the player and swing both arms up; the strike lands almost immediately.
		SetActorRotation(FMath::RInterpTo(GetActorRotation(), FRotator(0.f, ToPlayer.Rotation().Yaw, 0.f), DeltaSeconds, 20.f));
		const float Alpha = FMath::Clamp(StateTime / AttackWindup, 0.f, 1.f);
		for (USceneComponent* Pivot : ArmPivots)
		{
			Pivot->SetRelativeRotation(FRotator(WindupArmPitch * Alpha, 0.f, 0.f));
		}
		for (USceneComponent* Pivot : LegPivots)
		{
			Pivot->SetRelativeRotation(FRotator::ZeroRotator);
		}
		SetFlash(FMath::Max(Flash, 0.5f * Alpha * (0.5f + 0.5f * FMath::Sin(StateTime * 30.f))));
		if (Alpha >= 1.f)
		{
			if (Distance <= AttackRange * StrikeReachFactor)
			{
				Strike();
				return;
			}
			// Player backed off: chase again.
			State = EWalkingEnemyState::Walking;
			StateTime = 0.f;
			RepathTimer = 0.f;
		}
		break;
	}
	default:
		break;
	}
}

bool AWalkingEnemy::CanSeePlayer(const FVector& PlayerHead) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WalkerSight), false, this);
	return !GetWorld()->LineTraceTestByObjectType(Head->GetComponentLocation(), PlayerHead, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
}

void AWalkingEnemy::StartThrow()
{
	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		AI->StopMovement();
	}
	State = EWalkingEnemyState::Throwing;
	StateTime = 0.f;
	HeldRock->SetVisibility(true);
	if (ChargeSound)
	{
		VoiceAudio->SetSound(ChargeSound);
		VoiceAudio->SetPitchMultiplier(1.f);
		VoiceAudio->Play();
	}
}

void AWalkingEnemy::TickThrow(float DeltaSeconds, const FVector& PlayerHead, float Distance)
{
	// Player rushed in: forget the rock and hit them.
	if (Distance <= AttackRange)
	{
		HeldRock->SetVisibility(false);
		VoiceAudio->Stop();
		State = EWalkingEnemyState::Attacking;
		StateTime = 0.f;
		if (AttackSound)
		{
			UGameplayStatics::PlaySoundAtLocation(this, AttackSound, Head->GetComponentLocation());
		}
		return;
	}

	const FVector ToPlayer = PlayerHead - GetActorLocation();
	SetActorRotation(FMath::RInterpTo(GetActorRotation(), FRotator(0.f, ToPlayer.Rotation().Yaw, 0.f), DeltaSeconds, 8.f));

	// Warning: throwing arm drawn back over the head, the rock swells and glows, the body pulses (red-orange on the
	// thrower's material) faster and faster.
	const float Alpha = FMath::Clamp(StateTime / ThrowWindup, 0.f, 1.f);
	ArmPivots[1]->SetRelativeRotation(FRotator(-160.f * FMath::InterpEaseOut(0.f, 1.f, Alpha, 2.f), 0.f, 0.f));
	ArmPivots[0]->SetRelativeRotation(FRotator(35.f * Alpha, 0.f, 0.f)); // other arm forward, aiming
	for (USceneComponent* Pivot : LegPivots)
	{
		Pivot->SetRelativeRotation(FRotator::ZeroRotator);
	}
	HeldRock->SetRelativeScale3D(FVector(0.28f * (0.6f + 0.5f * Alpha) * (1.f + 0.12f * FMath::Sin(StateTime * 30.f))));
	const float PulseRate = 8.f + 26.f * Alpha; // rad/s
	SetFlash(FMath::Max(Flash, 0.3f + 0.7f * Alpha * (0.5f + 0.5f * FMath::Sin(StateTime * PulseRate))));

	if (Alpha >= 1.f)
	{
		ReleaseRock(PlayerHead);
		ArmPivots[1]->SetRelativeRotation(FRotator(50.f, 0.f, 0.f)); // follow-through
		HeldRock->SetVisibility(false);
		ThrowCooldownLeft = ThrowCooldown;
		State = EWalkingEnemyState::Walking;
		StateTime = 0.f;
		RepathTimer = 0.f;
	}
}

void AWalkingEnemy::ReleaseRock(const FVector& PlayerHead)
{
	const FVector Start = HeldRock->GetComponentLocation();
	const AEnemyRock* RockDefaults = RockClass->GetDefaultObject<AEnemyRock>();
	// Ballistic aim at the head under the rock's (light) gravity: v = d/T + g*T/2 upwards.
	const float FlightTime = FMath::Max(0.2f, FVector::Dist(Start, PlayerHead) / ThrowSpeed);
	FVector Velocity = (PlayerHead - Start) / FlightTime;
	Velocity.Z += 0.5f * RockDefaults->Gravity * FlightTime;

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AEnemyRock* Rock = GetWorld()->SpawnActor<AEnemyRock>(RockClass, Start, Velocity.Rotation(), Params))
	{
		Rock->Launch(Spawner.Get(), Velocity);
	}
	if (ReleaseSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, ReleaseSound, Start);
	}
}

FVector AWalkingEnemy::PickApproachTarget(const FVector& PlayerFeet, float Distance)
{
	// Close in: everyone goes straight for the player.
	if (Distance <= CommitDistance)
	{
		return PlayerFeet;
	}

	const FVector ToPlayer = (PlayerFeet - GetActorLocation()).GetSafeNormal2D();
	const FVector Side = FVector::CrossProduct(FVector::UpVector, ToPlayer); // perpendicular, horizontal
	// Offsets shrink as they close in, so they converge rather than orbit.
	const float Fade = FMath::Clamp((Distance - CommitDistance) / 600.f, 0.25f, 1.f);

	// Every walker re-rolls a small sideways drift now and then, so even chasers don't share one line.
	if (DetourTimer <= 0.f)
	{
		DetourTimer = FMath::FRandRange(1.5f, 3.f);
		Drift = FMath::FRandRange(-1.f, 1.f) * MaxDrift;
	}

	switch (Style)
	{
	case EWalkerStyle::Flanker:
		// Come in from one side of the player.
		return PlayerFeet + Side * (FlankSide * FlankDistance * Fade) + Side * Drift * 0.5f;

	case EWalkerStyle::Ambusher:
	{
		// Aim for where the player is heading (their view direction), to cut them off.
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const float ViewYaw = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraRotation().Yaw : ToPlayer.Rotation().Yaw;
		const FVector Ahead = FRotator(0.f, ViewYaw, 0.f).Vector() * FMath::Min(AmbushLead, Distance * 0.5f);
		return PlayerFeet + Ahead + Side * Drift * 0.5f;
	}
	default: // Chaser
		return PlayerFeet + Side * Drift * Fade;
	}
}

void AWalkingEnemy::UpdateWalkCycle(float DeltaSeconds)
{
	const float Speed = GetVelocity().Size2D();
	const float Amount = FMath::Clamp(Speed / 150.f, 0.f, 1.f);
	// One full cycle (two steps) per two strides; strides lengthen when fast so a 3 m/s walker runs, not shuffles.
	const float Stride = StrideLength * FMath::Max(1.f, Speed / 200.f);
	WalkPhase = FMath::Fmod(WalkPhase + DeltaSeconds * Speed / Stride * PI, 2.f * PI);
	const float Swing = FMath::Sin(WalkPhase);

	LegPivots[0]->SetRelativeRotation(FRotator(MaxLegSwing * Amount * Swing, 0.f, 0.f));
	LegPivots[1]->SetRelativeRotation(FRotator(-MaxLegSwing * Amount * Swing, 0.f, 0.f));
	ArmPivots[0]->SetRelativeRotation(FRotator(-MaxArmSwing * Amount * Swing, 0.f, 0.f));
	ArmPivots[1]->SetRelativeRotation(FRotator(MaxArmSwing * Amount * Swing, 0.f, 0.f));

	// Bob twice per cycle (once per step).
	VisualRoot->SetRelativeLocation(FVector(0.f, 0.f, -CapsuleHalfHeight + 3.f * Amount * FMath::Abs(FMath::Cos(WalkPhase))));
}

void AWalkingEnemy::Strike()
{
	if (AEnemySpawner* OwningSpawner = Spawner.Get())
	{
		OwningSpawner->NotifyEnemyReachedPlayer(this);
	}
	Destroy();
}

void AWalkingEnemy::SetFlash(float Value)
{
	Flash = Value;
	for (UStaticMeshComponent* Part : { Torso.Get(), Head.Get(), Legs[0].Get(), Legs[1].Get(), Arms[0].Get(), Arms[1].Get() })
	{
		Part->SetCustomPrimitiveDataFloat(0, Value);
	}
}

float AWalkingEnemy::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (State == EWalkingEnemyState::Dead || bFrozen)
	{
		return 0.f;
	}

	// Headshot: the hit landed on the top of the capsule, where the head is.
	FVector ShotDirection = -GetActorForwardVector();
	bool bHeadshot = false;
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		const FPointDamageEvent& Point = static_cast<const FPointDamageEvent&>(DamageEvent);
		ShotDirection = Point.ShotDirection;
		const float FeetZ = GetActorLocation().Z - CapsuleHalfHeight;
		bHeadshot = Point.HitInfo.bBlockingHit && Point.HitInfo.ImpactPoint.Z - FeetZ >= HeadshotHeight;
	}

	const float Applied = Super::TakeDamage(DamageAmount * (bHeadshot ? HeadshotDamageMultiplier : 1.f), DamageEvent, EventInstigator, DamageCauser);
	Health -= Applied;
	SetFlash(1.f);
	const bool bFirstHit = !bHasBeenHit;
	bHasBeenHit = true;

	if (Health <= 0.f)
	{
		Die(ShotDirection, bFirstHit, bHeadshot);
	}
	return Applied;
}

void AWalkingEnemy::Die(const FVector& ShotDirection, bool bOneShot, bool bHeadshot)
{
	State = EWalkingEnemyState::Dead;
	SetFlash(1.f);

	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		AI->StopMovement();
	}
	DetachFromControllerPendingDestroy();
	GetCharacterMovement()->DisableMovement();
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	if (DeathSound)
	{
		VoiceAudio->SetSound(DeathSound);
		VoiceAudio->Play();
	}

	// A rock being wound up dies with it (it hangs off the arm pivot, which doesn't fall).
	HeldRock->SetVisibility(false);

	// Fall apart along the shot.
	const FVector Knock = ShotDirection.GetSafeNormal() * DeathKnockback + FVector(0.f, 0.f, 120.f);
	for (UStaticMeshComponent* Piece : { Torso.Get(), Head.Get(), Eye.Get(), Legs[0].Get(), Legs[1].Get(), Arms[0].Get(), Arms[1].Get() })
	{
		Piece->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		Piece->SetCollisionObjectType(ECC_PhysicsBody);
		Piece->SetCollisionResponseToAllChannels(ECR_Block);
		Piece->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Piece->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore); // don't soak up the beam
		Piece->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Piece->SetSimulatePhysics(true);
		Piece->AddImpulse(Knock + FMath::VRand() * 150.f, NAME_None, /*bVelChange*/ true);
		Piece->AddAngularImpulseInDegrees(FMath::VRand() * 540.f, NAME_None, /*bVelChange*/ true);
	}
	if (bHeadshot)
	{
		// The head pops off: much harder knock, mostly up and along the shot.
		const FVector HeadPop = ShotDirection.GetSafeNormal() * DeathKnockback * 1.6f + FVector(0.f, 0.f, 520.f);
		for (UStaticMeshComponent* Piece : { Head.Get(), Eye.Get() })
		{
			Piece->AddImpulse(HeadPop, NAME_None, /*bVelChange*/ true);
		}
		if (HeadshotSound)
		{
			UGameplayStatics::PlaySoundAtLocation(this, HeadshotSound, Head->GetComponentLocation());
		}
	}
	SetLifeSpan(CorpseLifeSpan);

	if (AEnemySpawner* OwningSpawner = Spawner.Get())
	{
		OwningSpawner->NotifyEnemyKilled(this, bOneShot, bHeadshot);
	}
}
