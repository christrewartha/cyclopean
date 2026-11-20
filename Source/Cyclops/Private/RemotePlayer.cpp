#include "RemotePlayer.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr int32 MaxSnapshots = 60;           // 2 s at 30 Hz
	constexpr double ClockCreepPerSecond = 0.002; // lets the offset estimate recover from one unusually fast pose
	constexpr double BodyDrop = 55.0;             // body centre below the head centre
	constexpr double NameTagHeight = 30.0;
	constexpr float HitFlashTime = 0.25f;
	constexpr float TracerSpeed = 15000.f;        // cm/s, same look as the local gun
	constexpr float TracerLength = 120.f;
	constexpr float TracerRadius = 0.6f;
	const FLinearColor TracerColour(1.f, 0.45f, 0.3f); // a little redder than ours

	void MakeHittable(UStaticMeshComponent* Mesh)
	{
		// Query-only: the local gun's Visibility traces hit it, nothing else (walking, physics) notices it.
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Mesh->SetCollisionObjectType(ECC_WorldDynamic);
		Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Mesh->SetCastShadow(false);
	}

	void MakeDecor(UStaticMeshComponent* Mesh)
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCastShadow(false);
	}
}

ARemotePlayer::ARemotePlayer()
{
	PrimaryActorTick.bCanEverTick = true;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Pistol(TEXT("/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Colour(TEXT("/Game/Cyclops/Materials/M_HudBar.M_HudBar"));
	SphereMesh = Sphere.Object;
	CubeMesh = Cube.Object;
	CylinderMesh = Cylinder.Object;
	GunMesh = Pistol.Object;
	ColourMaterial = Colour.Object;

	// Root = their head (location and full rotation).
	HeadRoot = CreateDefaultSubobject<USceneComponent>(TEXT("HeadRoot"));
	SetRootComponent(HeadRoot);

	Head = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Head"));
	Head->SetupAttachment(HeadRoot);
	Head->SetStaticMesh(SphereMesh);
	Head->SetRelativeScale3D(FVector(0.21, 0.17, 0.23)); // engine sphere is 100 cm across
	MakeHittable(Head);

	Visor = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visor"));
	Visor->SetupAttachment(HeadRoot);
	Visor->SetStaticMesh(CubeMesh);
	Visor->SetRelativeLocation(FVector(10.0, 0.0, 2.0));
	Visor->SetRelativeScale3D(FVector(0.02, 0.15, 0.035));
	MakeDecor(Visor);

	// Pistol mesh barrel is +Y; turn it onto the head's forward and offset like the local viewmodel.
	Gun = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Gun"));
	Gun->SetupAttachment(HeadRoot);
	Gun->SetStaticMesh(GunMesh);
	Gun->SetRelativeLocationAndRotation(GunOffset + FVector(-7.8, 0.0, -3.2), FRotator(0.f, -90.f, 0.f));
	MakeDecor(Gun);

	// Body hangs upright below the head (yaw only), so it's placed in world space each frame.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(HeadRoot);
	Body->SetUsingAbsoluteLocation(true);
	Body->SetUsingAbsoluteRotation(true);
	Body->SetStaticMesh(CylinderMesh);
	Body->SetRelativeScale3D(FVector(0.36, 0.36, 0.65));
	MakeHittable(Body);

	NameTag = CreateDefaultSubobject<UTextRenderComponent>(TEXT("NameTag"));
	NameTag->SetupAttachment(HeadRoot);
	NameTag->SetUsingAbsoluteLocation(true);
	NameTag->SetUsingAbsoluteRotation(true);
	NameTag->SetHorizontalAlignment(EHTA_Center);
	NameTag->SetVerticalAlignment(EVRTA_TextBottom);
	NameTag->SetWorldSize(9.f);
	NameTag->SetTextRenderColor(FColor(255, 200, 120));
	NameTag->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NameTag->SetCastShadow(false);

	Tracer = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Tracer"));
	Tracer->SetupAttachment(HeadRoot);
	Tracer->SetUsingAbsoluteLocation(true);
	Tracer->SetUsingAbsoluteRotation(true);
	Tracer->SetUsingAbsoluteScale(true);
	Tracer->SetStaticMesh(CylinderMesh);
	MakeDecor(Tracer);
}

void ARemotePlayer::BeginPlay()
{
	Super::BeginPlay();
	for (UStaticMeshComponent* Mesh : { Head.Get(), Visor.Get(), Body.Get(), Tracer.Get() })
	{
		Mesh->SetMaterial(0, ColourMaterial);
	}
	// Custom primitive data is runtime-only: set colours here, not in the constructor.
	SetColour(Head, BodyColour);
	SetColour(Body, BodyColour);
	SetColour(Visor, VisorColour);
	SetColour(Tracer, TracerColour);
	Tracer->SetHiddenInGame(true);
	// Until their first pose arrives. Hidden actors still collide, so the hitbox goes too.
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void ARemotePlayer::SetDead(bool bInDead)
{
	bDead = bInDead;
	if (bDead)
	{
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
	}
	else
	{
		ResetPoses();
	}
}

void ARemotePlayer::ResetPoses()
{
	Snapshots.Reset();
	SetActorHiddenInGame(true); // reappears (with its hitbox) on the next pose
	SetActorEnableCollision(false);
}

void ARemotePlayer::SetColour(UStaticMeshComponent* Mesh, const FLinearColor& Colour) const
{
	Mesh->SetCustomPrimitiveDataVector4(0, FVector4(Colour.R, Colour.G, Colour.B, 1.0));
}

void ARemotePlayer::SetPlayerName(const FString& Name)
{
	NameTag->SetText(FText::FromString(Name));
}

void ARemotePlayer::PushPose(double SenderTime, const FVector& HeadLocation, const FRotator& HeadRotation)
{
	if (bDead)
	{
		return;
	}
	// The fastest pose to arrive best shows the real difference between the clocks (others were delayed more).
	const double Offset = FPlatformTime::Seconds() - SenderTime;
	if (!bHasClockOffset || Offset < ClockOffset)
	{
		ClockOffset = Offset;
		bHasClockOffset = true;
	}
	if (!Snapshots.IsEmpty() && SenderTime <= Snapshots.Last().Time)
	{
		return; // out of order (shouldn't happen over one WebSocket, but cheap to guard)
	}
	Snapshots.Add({ SenderTime, HeadLocation, HeadRotation.Quaternion() });
	if (Snapshots.Num() > MaxSnapshots)
	{
		Snapshots.RemoveAt(0, Snapshots.Num() - MaxSnapshots);
	}
	if (IsHidden())
	{
		ApplyPose(HeadLocation, HeadRotation.Quaternion());
		SetActorHiddenInGame(false);
		SetActorEnableCollision(true);
		Tracer->SetHiddenInGame(true);
	}
}

void ARemotePlayer::ApplyPose(const FVector& HeadLocation, const FQuat& HeadRotation)
{
	SetActorLocationAndRotation(HeadLocation, HeadRotation);
	const float Yaw = HeadRotation.Rotator().Yaw;
	Body->SetWorldLocationAndRotation(HeadLocation - FVector(0.0, 0.0, BodyDrop), FRotator(0.f, Yaw, 0.f));
	NameTag->SetWorldLocation(HeadLocation + FVector(0.0, 0.0, NameTagHeight));
}

void ARemotePlayer::FaceNameTagToViewer()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && PC->PlayerCameraManager)
	{
		const FVector ToViewer = PC->PlayerCameraManager->GetCameraLocation() - NameTag->GetComponentLocation();
		NameTag->SetWorldRotation(FRotator(0.f, ToViewer.Rotation().Yaw, 0.f));
	}
}

void ARemotePlayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!Snapshots.IsEmpty())
	{
		ClockOffset += DeltaSeconds * ClockCreepPerSecond;
		const double RenderTime = FPlatformTime::Seconds() - ClockOffset - InterpDelay;
		if (RenderTime <= Snapshots[0].Time)
		{
			ApplyPose(Snapshots[0].Location, Snapshots[0].Rotation);
		}
		else if (RenderTime >= Snapshots.Last().Time)
		{
			ApplyPose(Snapshots.Last().Location, Snapshots.Last().Rotation); // starved: hold the newest
		}
		else
		{
			int32 Next = 1;
			while (Snapshots[Next].Time <= RenderTime)
			{
				++Next;
			}
			const FSnapshot& A = Snapshots[Next - 1];
			const FSnapshot& B = Snapshots[Next];
			const double Alpha = (RenderTime - A.Time) / FMath::Max(B.Time - A.Time, 1e-6);
			ApplyPose(FMath::Lerp(A.Location, B.Location, Alpha), FQuat::Slerp(A.Rotation, B.Rotation, Alpha));
			if (Next > 1)
			{
				Snapshots.RemoveAt(0, Next - 1); // older than what we're playing
			}
		}
	}
	FaceNameTagToViewer();

	if (HitFlash > 0.f)
	{
		HitFlash = FMath::Max(0.f, HitFlash - DeltaSeconds);
		const FLinearColor Colour = FMath::Lerp(BodyColour, FLinearColor::White, HitFlash / HitFlashTime);
		SetColour(Head, Colour);
		SetColour(Body, Colour);
	}

	if (TracerAge >= 0.f)
	{
		TracerAge += DeltaSeconds;
		const FVector Path = TracerEnd - TracerStart;
		const double Length = Path.Size();
		const double HeadDist = FMath::Min<double>(TracerAge * TracerSpeed, Length);
		const double TailDist = FMath::Clamp<double>(TracerAge * TracerSpeed - TracerLength, 0.0, Length);
		if (TailDist >= Length - 0.5 || Length < 1.0)
		{
			TracerAge = -1.f;
			Tracer->SetHiddenInGame(true);
		}
		else
		{
			const FVector Direction = Path / Length;
			const double Radius = TracerRadius / 50.0;
			Tracer->SetWorldTransform(FTransform(FRotationMatrix::MakeFromZ(Direction).ToQuat(),
				TracerStart + Direction * ((HeadDist + TailDist) * 0.5), FVector(Radius, Radius, (HeadDist - TailDist) / 100.0)));
			Tracer->SetHiddenInGame(false);
		}
	}
}

void ARemotePlayer::ShowShot(const FVector& From, const FVector& To)
{
	TracerStart = From;
	TracerEnd = To;
	TracerAge = 0.f;
}

float ARemotePlayer::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	const float Applied = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	bool bHeadshot = false;
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		bHeadshot = static_cast<const FPointDamageEvent&>(DamageEvent).HitInfo.GetComponent() == Head;
	}
	HitFlash = HitFlashTime;
	OnShot.Broadcast(DamageAmount, bHeadshot);
	return Applied;
}
