#include "FlyingEnemy.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnemySpawner.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"

namespace
{
	void ConfigureSpatialAudio(UAudioComponent* Audio, float FalloffDistance)
	{
		Audio->bAutoActivate = false;
		Audio->bOverrideAttenuation = true;
		Audio->AttenuationOverrides.bAttenuate = true;
		Audio->AttenuationOverrides.bSpatialize = true;
		Audio->AttenuationOverrides.AttenuationShape = EAttenuationShape::Sphere;
		Audio->AttenuationOverrides.AttenuationShapeExtents = FVector(150.f);
		Audio->AttenuationOverrides.FalloffDistance = FalloffDistance;
	}

	UStaticMeshComponent* CreatePart(AActor* Owner, FName Name, USceneComponent* Parent, const FVector& Location, const FVector& Scale)
	{
		UStaticMeshComponent* Part = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(Parent);
		Part->SetRelativeLocation(Location);
		Part->SetRelativeScale3D(Scale);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCastShadow(false);
		return Part;
	}
}

AFlyingEnemy::AFlyingEnemy()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	// Root: query-only sphere that blocks the beam's trace channel and nothing else, so enemies
	// pass through scenery and the player but can be shot.
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(30.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Collision->SetGenerateOverlapEvents(false);
	RootComponent = Collision;

	// Sizes assume the engine's 100 cm basic shapes. Forward is +X.
	Body = CreatePart(this, TEXT("Body"), Collision, FVector::ZeroVector, FVector(0.45f, 0.35f, 0.3f));
	EyeLeft = CreatePart(this, TEXT("EyeLeft"), Collision, FVector(19.f, -7.f, 5.f), FVector(0.07f));
	EyeRight = CreatePart(this, TEXT("EyeRight"), Collision, FVector(19.f, 7.f, 5.f), FVector(0.07f));

	WingPivotLeft = CreateDefaultSubobject<USceneComponent>(TEXT("WingPivotLeft"));
	WingPivotLeft->SetupAttachment(Collision);
	WingPivotLeft->SetRelativeLocation(FVector(0.f, -12.f, 4.f));
	WingPivotRight = CreateDefaultSubobject<USceneComponent>(TEXT("WingPivotRight"));
	WingPivotRight->SetupAttachment(Collision);
	WingPivotRight->SetRelativeLocation(FVector(0.f, 12.f, 4.f));

	WingLeft = CreatePart(this, TEXT("WingLeft"), WingPivotLeft, FVector(-4.f, -24.f, 0.f), FVector(0.3f, 0.45f, 0.02f));
	WingRight = CreatePart(this, TEXT("WingRight"), WingPivotRight, FVector(-4.f, 24.f, 0.f), FVector(0.3f, 0.45f, 0.02f));

	FlapAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("FlapAudio"));
	FlapAudio->SetupAttachment(Collision);
	ConfigureSpatialAudio(FlapAudio, 2500.f);

	VoiceAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("VoiceAudio"));
	VoiceAudio->SetupAttachment(Collision);
	ConfigureSpatialAudio(VoiceAudio, 4000.f);
}

void AFlyingEnemy::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	Body->SetStaticMesh(BodyMesh);
	Body->SetMaterial(0, BodyMaterial);
	for (UStaticMeshComponent* Eye : { EyeLeft.Get(), EyeRight.Get() })
	{
		Eye->SetStaticMesh(BodyMesh);
		Eye->SetMaterial(0, EyeMaterial);
	}
	for (UStaticMeshComponent* Wing : { WingLeft.Get(), WingRight.Get() })
	{
		Wing->SetStaticMesh(WingMesh);
		Wing->SetMaterial(0, WingMaterial);
	}
	FlapAudio->SetSound(FlapLoopSound);
}

void AFlyingEnemy::SetFrozen(bool bInFrozen)
{
	bFrozen = bInFrozen;
	FlapAudio->SetPaused(bInFrozen);
	VoiceAudio->SetPaused(bInFrozen);
}

void AFlyingEnemy::InitEnemy(AEnemySpawner* InSpawner, float InSpeed)
{
	Spawner = InSpawner;
	Speed = InSpeed;
}

void AFlyingEnemy::BeginPlay()
{
	Super::BeginPlay();

	Health = MaxHealth;
	BasePosition = GetActorLocation();
	LastLocation = BasePosition;
	WeavePhaseH = FMath::FRandRange(0.f, 2.f * PI);
	WeavePhaseV = FMath::FRandRange(0.f, 2.f * PI);
	WeaveSide = FMath::RandBool() ? 1.f : -1.f;
	SetActorScale3D(FVector(0.01f));

	if (AnnounceSound)
	{
		VoiceAudio->SetSound(AnnounceSound);
		VoiceAudio->Play();
	}
	if (FlapLoopSound)
	{
		FlapAudio->Play();
	}
}

bool AFlyingEnemy::GetHeadLocation(FVector& OutLocation) const
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

void AFlyingEnemy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (State == EFlyingEnemyState::Dead || bFrozen)
	{
		return;
	}

	StateTime += DeltaSeconds;

	FVector Head;
	if (GetHeadLocation(Head))
	{
		TickFlight(DeltaSeconds, Head);
	}
	TickLook(DeltaSeconds);
}

void AFlyingEnemy::TickFlight(float DeltaSeconds, const FVector& HeadLocation)
{
	FVector NewLocation = GetActorLocation();

	switch (State)
	{
	case EFlyingEnemyState::Appearing:
	{
		const float Alpha = FMath::Clamp(StateTime / FMath::Max(AppearTime, UE_KINDA_SMALL_NUMBER), 0.f, 1.f);
		SetActorScale3D(FVector(FMath::Max(0.01f, FMath::SmoothStep(0.f, 1.f, Alpha))));
		if (Alpha >= 1.f)
		{
			State = EFlyingEnemyState::Hovering;
			StateTime = 0.f;
		}
		break;
	}
	case EFlyingEnemyState::Hovering:
		// Bob and sway gently in place so it reads as alive while the player locates it.
		NewLocation = BasePosition + FVector(0.f, 8.f * WeaveSide * FMath::Sin(StateTime * 2.3f), 12.f * FMath::Sin(StateTime * 5.f));
		if (StateTime >= HoverTime)
		{
			// Start the attack path exactly where the hover left off, so nothing jumps.
			BasePosition = NewLocation;
			State = EFlyingEnemyState::Attacking;
			StateTime = 0.f;
		}
		break;

	case EFlyingEnemyState::Attacking:
	{
		// Base path homes straight in; the visible position weaves around it.
		const FVector ToHead = HeadLocation - BasePosition;
		const double BaseDistance = ToHead.Size();
		const FVector Dir = BaseDistance > UE_KINDA_SMALL_NUMBER ? ToHead / BaseDistance : GetActorForwardVector();

		// Ease out of the hover: speed builds up and the weave grows from zero, so the first attacking
		// frame lands on the hover position instead of snapping to a random point on the weave.
		const float SpeedRamp = FMath::Lerp(0.15f, 1.f, FMath::SmoothStep(0.f, 1.f, StateTime / FMath::Max(SpeedRampTime, UE_KINDA_SMALL_NUMBER)));
		const float WeaveRamp = FMath::SmoothStep(0.f, 1.f, StateTime / FMath::Max(WeaveRampTime, UE_KINDA_SMALL_NUMBER));
		BasePosition += Dir * FMath::Min<double>(Speed * SpeedRamp * DeltaSeconds, BaseDistance);

		FVector Right = FVector::CrossProduct(Dir, FVector::UpVector);
		Right = Right.IsNearlyZero() ? FVector::RightVector : Right.GetSafeNormal();
		const FVector Up = FVector::CrossProduct(Right, Dir);

		const float DistanceFactor = FMath::Lerp(WeaveMinFraction, 1.f, FMath::Clamp(float(BaseDistance) / WeaveFadeDistance, 0.f, 1.f));
		const float CloseFactor = FMath::Clamp(float(BaseDistance) / 150.f, 0.f, 1.f); // settle onto the target at the end
		const float Amplitude = WeaveAmplitude * DistanceFactor * CloseFactor * WeaveRamp;
		const float W = 2.f * PI * WeaveFrequency;
		const float OffsetH = WeaveSide * Amplitude * FMath::Sin(W * StateTime + WeavePhaseH);
		const float OffsetV = 0.5f * Amplitude * FMath::Sin(W * 1.37f * StateTime + WeavePhaseV);

		NewLocation = BasePosition + Right * OffsetH + Up * OffsetV;

		if (FVector::Dist(NewLocation, HeadLocation) <= AttackRadius || BaseDistance <= AttackRadius * 0.5)
		{
			ReachPlayer();
			return;
		}
		break;
	}
	default:
		break;
	}

	// Face the direction of travel; while not moving much, face the player.
	const FVector Velocity = NewLocation - LastLocation;
	const FVector FaceDir = (State == EFlyingEnemyState::Attacking && Velocity.SizeSquared() > 0.01) ? Velocity : HeadLocation - NewLocation;
	const FRotator TargetRotation = FaceDir.Rotation();
	SetActorLocationAndRotation(NewLocation, FMath::RInterpTo(GetActorRotation(), TargetRotation, DeltaSeconds, 6.f));
	LastLocation = NewLocation;
}

void AFlyingEnemy::TickLook(float DeltaSeconds)
{
	const float Rate = State == EFlyingEnemyState::Attacking ? FlapRate * 1.3f : FlapRate;
	const float Angle = FlapAngle * FMath::Sin(GetWorld()->GetTimeSeconds() * Rate * 2.f * PI + WeavePhaseH);
	WingPivotLeft->SetRelativeRotation(FRotator(0.f, 0.f, -Angle));
	WingPivotRight->SetRelativeRotation(FRotator(0.f, 0.f, Angle));

	if (Flash > 0.f)
	{
		SetFlash(FMath::Max(0.f, Flash - DeltaSeconds * 6.f));
	}
}

void AFlyingEnemy::SetFlash(float Value)
{
	Flash = Value;
	for (UStaticMeshComponent* Part : { Body.Get(), WingLeft.Get(), WingRight.Get() })
	{
		Part->SetCustomPrimitiveDataFloat(0, Value);
	}
}

float AFlyingEnemy::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (State == EFlyingEnemyState::Dead || bFrozen)
	{
		return 0.f;
	}

	const float Applied = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	Health -= Applied;
	SetFlash(1.f);
	const bool bFirstHit = !bHasBeenHit;
	bHasBeenHit = true;

	if (Health <= 0.f)
	{
		FVector ShotDirection = GetActorForwardVector() * -1.f;
		if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
		{
			ShotDirection = static_cast<const FPointDamageEvent&>(DamageEvent).ShotDirection;
		}
		Die(ShotDirection, bFirstHit);
	}
	return Applied;
}

void AFlyingEnemy::MakePhysicsPiece(UStaticMeshComponent* Piece, const FVector& Impulse)
{
	Piece->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	Piece->SetCollisionObjectType(ECC_PhysicsBody);
	Piece->SetCollisionResponseToAllChannels(ECR_Block);
	Piece->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Piece->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Piece->SetSimulatePhysics(true);
	Piece->SetUseCCD(true);
	Piece->AddImpulse(Impulse, NAME_None, /*bVelChange*/ true);
	Piece->AddAngularImpulseInDegrees(FMath::VRand() * 720.f, NAME_None, /*bVelChange*/ true);
}

void AFlyingEnemy::Die(const FVector& ShotDirection, bool bOneShot)
{
	State = EFlyingEnemyState::Dead;
	SetFlash(1.f);

	FlapAudio->Stop();
	if (DeathSound)
	{
		VoiceAudio->SetSound(DeathSound);
		VoiceAudio->Play();
	}

	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	const FVector Knock = ShotDirection.GetSafeNormal() * DeathKnockback + FVector(0.f, 0.f, 150.f);
	for (UStaticMeshComponent* Piece : { Body.Get(), WingLeft.Get(), WingRight.Get(), EyeLeft.Get(), EyeRight.Get() })
	{
		MakePhysicsPiece(Piece, Knock + FMath::VRand() * 150.f);
	}

	SetLifeSpan(CorpseLifeSpan);

	if (AEnemySpawner* OwningSpawner = Spawner.Get())
	{
		OwningSpawner->NotifyEnemyKilled(this, bOneShot);
	}
}

void AFlyingEnemy::ReachPlayer()
{
	if (AEnemySpawner* OwningSpawner = Spawner.Get())
	{
		OwningSpawner->NotifyEnemyReachedPlayer(this);
	}
	Destroy();
}
