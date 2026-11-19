#include "EnemyRock.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnemySpawner.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float RockRadius = 14.f;
}

AEnemyRock::AEnemyRock()
{
	PrimaryActorTick.bCanEverTick = true;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(RockRadius);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block); // smashes on walls / ground
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);  // the optic blast can shoot it down
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetGenerateOverlapEvents(false);
	RootComponent = Collision;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetRelativeScale3D(FVector(RockRadius * 2.f / 100.f));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Glow(TEXT("/Game/Cyclops/Materials/MI_ThrowerRock.MI_ThrowerRock"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Impact(TEXT("/Game/Cyclops/Audio/SW_RockImpact.SW_RockImpact"));
	Mesh->SetStaticMesh(Sphere.Object);
	Mesh->SetMaterial(0, Glow.Object);
	ImpactSound = Impact.Object;
}

void AEnemyRock::Launch(AEnemySpawner* InSpawner, const FVector& InVelocity)
{
	Spawner = InSpawner;
	Velocity = InVelocity;
}

void AEnemyRock::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const AEnemySpawner* OwningSpawner = Spawner.Get();
	if (!OwningSpawner || !OwningSpawner->IsRunActive())
	{
		Destroy(); // run over (game over / back to title)
		return;
	}
	if (OwningSpawner->IsGamePaused() || bSmashed)
	{
		return;
	}

	Age += DeltaSeconds;
	if (Age > MaxLifetime)
	{
		Destroy();
		return;
	}

	// Gentle arc; sweep so it smashes on walls rather than passing through.
	Velocity.Z -= Gravity * DeltaSeconds;
	FHitResult Hit;
	SetActorLocation(GetActorLocation() + Velocity * DeltaSeconds, /*bSweep*/ true, &Hit);
	if (Hit.bBlockingHit)
	{
		Smash();
		return;
	}

	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && PC->PlayerCameraManager && FVector::Dist(PC->PlayerCameraManager->GetCameraLocation(), GetActorLocation()) <= HitRadius)
	{
		if (AEnemySpawner* Target = Spawner.Get())
		{
			Target->NotifyEnemyReachedPlayer(this);
		}
		Smash();
	}
}

float AEnemyRock::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// Any beam hit knocks it out of the air.
	if (!bSmashed && DamageAmount > 0.f)
	{
		Smash();
	}
	return DamageAmount;
}

void AEnemyRock::Smash()
{
	if (bSmashed)
	{
		return;
	}
	bSmashed = true;
	if (ImpactSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, ImpactSound, GetActorLocation());
	}
	Destroy();
}
