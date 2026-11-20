#include "HandGun.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Haptics/HapticFeedbackEffect_Base.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY(LogHandGun); // "log LogHandGun Verbose" traces every shot's aim solution

namespace
{
	constexpr int32 TracerPoolSize = 6;
	constexpr double MeshRadius = 50.0;  // engine BasicShapes
	constexpr double MeshLength = 100.0;
	/** The eye ray's hit lies exactly on the surface; aim this far past it so the muzzle ray reliably hits it too. */
	constexpr double AimOvershoot = 5.0;
	const FVector FlashScale(0.09, 0.05, 0.05); // ~9 x 5 cm, stretched down the barrel
	const FLinearColor FlashColour(1.f, 0.85f, 0.5f);

	/**
	 * Hides (or restores) the player's VR hands: the template pawn's HandLeft/HandRight mannequin hands and its
	 * controller models (XRDeviceVisualization). With an FPS-style gun the controllers are just buttons, so hands
	 * floating about would only distract. Uses HiddenInGame, which the template's own visibility toggles don't touch.
	 */
	void SetPawnHandsHidden(const APawn* Pawn, bool bHidden)
	{
		if (!Pawn)
		{
			return;
		}
		TArray<UPrimitiveComponent*> Primitives;
		Pawn->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			const FString Name = Primitive->GetName();
			if (Name.StartsWith(TEXT("Hand")) || Name.Contains(TEXT("XRDeviceVisualization")))
			{
				Primitive->SetHiddenInGame(bHidden, /*bPropagateToChildren*/ true);
			}
		}
	}
}

AHandGun::AHandGun()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the HMD has updated the camera, so the gun is posed against this frame's view.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Pistol(TEXT("/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Shot(TEXT("/Game/XRFramework/Audio/Fire_Cue.Fire_Cue"));
	static ConstructorHelpers::FObjectFinder<UHapticFeedbackEffect_Base> Kick(TEXT("/Game/XRFramework/Haptics/PistolFireHapticEffect.PistolFireHapticEffect"));
	static ConstructorHelpers::FObjectFinder<UInputAction> TriggerRight(TEXT("/Game/XRFramework/Input/Actions/IA_Shoot_Right.IA_Shoot_Right"));
	static ConstructorHelpers::FObjectFinder<UInputAction> TriggerLeft(TEXT("/Game/XRFramework/Input/Actions/IA_Shoot_Left.IA_Shoot_Left"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Glow(TEXT("/Game/Cyclops/Materials/M_HudBar.M_HudBar"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Hole(TEXT("/Game/Cyclops/Materials/M_ScorchDecal.M_ScorchDecal"));
	GunMesh = Pistol.Object;
	FireSound = Shot.Object;
	FireHaptic = Kick.Object;
	FireActionRight = TriggerRight.Object;
	FireActionLeft = TriggerLeft.Object;
	TracerMesh = Cylinder.Object;
	FlashMesh = Sphere.Object;
	GlowMaterial = Glow.Object;
	ImpactDecalMaterial = Hole.Object;

	// Root = the aiming pivot: rides on the camera at ViewOffset, its X axis turned onto the aim point.
	AimPivot = CreateDefaultSubobject<USceneComponent>(TEXT("AimPivot"));
	SetRootComponent(AimPivot);

	GunMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("GunMesh"));
	GunMeshComponent->SetupAttachment(AimPivot);
	GunMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GunMeshComponent->SetGenerateOverlapEvents(false);
	GunMeshComponent->SetCastShadow(false);

	MuzzlePoint = CreateDefaultSubobject<USceneComponent>(TEXT("Muzzle"));
	MuzzlePoint->SetupAttachment(GunMeshComponent);

	FlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("FlashLight"));
	FlashLight->SetupAttachment(MuzzlePoint);
	FlashLight->SetRelativeLocation(FVector(8.0, 0.0, 0.0));
	FlashLight->SetCastShadows(false);
	FlashLight->SetAttenuationRadius(350.f);
	FlashLight->SetLightColor(FlashColour);
	FlashLight->SetVisibility(false);
}

void AHandGun::BeginPlay()
{
	Super::BeginPlay();

	GunMeshComponent->SetStaticMesh(GunMesh);
	GunMeshComponent->SetRelativeTransform(MeshOnPivot);
	MuzzlePoint->SetRelativeTransform(MuzzleOnGun);
	FlashLight->SetIntensity(FlashLightIntensity);

	FlashComponent = MakeGlowMesh(TEXT("MuzzleFlash"), FlashMesh, FlashColour);
	if (FlashComponent)
	{
		FlashComponent->AttachToComponent(MuzzlePoint, FAttachmentTransformRules::KeepRelativeTransform);
		FlashComponent->SetUsingAbsoluteLocation(false);
		FlashComponent->SetUsingAbsoluteRotation(false);
		FlashComponent->SetUsingAbsoluteScale(false);
		FlashComponent->SetRelativeTransform(FTransform(FRotator::ZeroRotator, FVector(4.0, 0.0, 0.0), FlashScale));
	}
	for (int32 i = 0; i < TracerPoolSize; ++i)
	{
		FTracer& Tracer = Tracers.AddDefaulted_GetRef();
		Tracer.Mesh = MakeGlowMesh(*FString::Printf(TEXT("Tracer%d"), i), TracerMesh, TracerColour);
		if (Tracer.Mesh)
		{
			TracerMeshes.Add(Tracer.Mesh);
		}
	}

	TryAttachToView();
	TryBindInput();
}

void AHandGun::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (APlayerController* PC = BoundController.Get())
	{
		DisableInput(PC);
	}
	// Back to the beam (or leaving the level): give the player their hands back.
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && bAttached)
	{
		SetPawnHandsHidden(PC->GetPawn(), false);
	}
	for (const TWeakObjectPtr<UDecalComponent>& Decal : ActiveDecals)
	{
		if (UDecalComponent* Live = Decal.Get())
		{
			Live->DestroyComponent();
		}
	}
	ActiveDecals.Reset();
	Super::EndPlay(EndPlayReason);
}

UStaticMeshComponent* AHandGun::MakeGlowMesh(FName Name, UStaticMesh* Mesh, const FLinearColor& Colour)
{
	if (!Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this, Name);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(AimPivot);
	// Tracers live in world space; the flash switches back to relative after creation.
	Component->SetUsingAbsoluteLocation(true);
	Component->SetUsingAbsoluteRotation(true);
	Component->SetUsingAbsoluteScale(true);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCastShadow(false);
	Component->SetStaticMesh(Mesh);
	if (GlowMaterial)
	{
		Component->SetMaterial(0, GlowMaterial);
	}
	Component->SetHiddenInGame(true);
	Component->RegisterComponent();
	// Custom primitive data is runtime-only, so set it after registering.
	Component->SetCustomPrimitiveDataVector4(0, FVector4(Colour.R, Colour.G, Colour.B, 1.0));
	return Component;
}

bool AHandGun::TryAttachToView()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!Camera)
	{
		return false;
	}
	// Head-locked: rides on the HMD camera (so it also gets the camera's late update and never lags the view).
	AttachToComponent(Camera, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	SetPawnHandsHidden(Pawn, true);
	AimPivot->SetRelativeLocationAndRotation(ViewOffset, FRotator::ZeroRotator);
	ViewCamera = Camera;
	Retract = 0.f;
	bHasAimRotation = false;
	bAttached = true;
	UE_LOG(LogHandGun, Log, TEXT("Gun attached to %s"), *Camera->GetName());
	return true;
}

bool AHandGun::TryBindInput()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || (!FireActionRight && !FireActionLeft))
	{
		return false;
	}
	EnableInput(PC);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (!Input)
	{
		UE_LOG(LogHandGun, Error, TEXT("Input component is not a UEnhancedInputComponent; triggers won't fire the gun."));
		return false;
	}
	if (FireActionRight)
	{
		Input->BindAction(FireActionRight, ETriggerEvent::Started, this, &AHandGun::HandleFireRight);
	}
	if (FireActionLeft)
	{
		Input->BindAction(FireActionLeft, ETriggerEvent::Started, this, &AHandGun::HandleFireLeft);
	}
	BoundController = PC;
	return true;
}

void AHandGun::HandleFireRight(const FInputActionValue& Value)
{
	FireFrom(EControllerHand::Right);
}

void AHandGun::HandleFireLeft(const FInputActionValue& Value)
{
	FireFrom(EControllerHand::Left);
}

void AHandGun::UpdatePose(float DeltaSeconds)
{
	const USceneComponent* Camera = ViewCamera.Get();
	if (!Camera)
	{
		return;
	}
	const FTransform View = Camera->GetComponentTransform();
	const FVector Eye = View.GetLocation();
	const UWorld* World = GetWorld();
	const FCollisionQueryParams Params = MakeQueryParams();

	// Pull back from walls: trace from the eyes to just past where the muzzle would be at full reach. Snap back
	// instantly (never show the gun inside a wall), ease out again slowly.
	const FVector MuzzleOnPivotNow = AimPivot->GetComponentTransform().InverseTransformPosition(GetMuzzleLocation());
	const FVector FullReachPivot = View.TransformPosition(ViewOffset);
	const FVector FullReachMuzzle = AimPivot->GetComponentTransform().GetRotation().RotateVector(MuzzleOnPivotNow) + FullReachPivot;
	const FVector ToMuzzle = FullReachMuzzle - Eye;
	const double Reach = ToMuzzle.Size();
	float WantedRetract = 0.f;
	FHitResult WallHit;
	if (Reach > UE_KINDA_SMALL_NUMBER
		&& World->LineTraceSingleByChannel(WallHit, Eye, Eye + ToMuzzle * ((Reach + WallGap) / Reach), TraceChannel, Params))
	{
		WantedRetract = FMath::Min<float>(Reach + WallGap - WallHit.Distance, ViewOffset.X - MinViewDistance);
	}
	Retract = WantedRetract > Retract ? WantedRetract : FMath::FInterpTo(Retract, WantedRetract, DeltaSeconds, 6.f);
	AimPivot->SetRelativeLocation(ViewOffset - FVector(FMath::Max(0.f, Retract), 0.0, 0.0));

	// Turn the barrel onto the aim point (what's under the centre of view), upright relative to the head. Done in
	// camera space and smoothed, so turning the head keeps it rigid and only near/far aim changes ease in.
	const FVector LookDirection = View.GetUnitAxis(EAxis::X);
	FHitResult LookHit;
	const double AimDistance = World->LineTraceSingleByChannel(LookHit, Eye, Eye + LookDirection * MaxRange, TraceChannel, Params)
		? LookHit.Distance : MaxRange;
	const FVector AimPoint = Eye + LookDirection * FMath::Max<double>(AimDistance, MinConvergeDistance);
	const FVector AimInView = View.InverseTransformPosition(AimPoint);
	const FVector MuzzleInView = View.InverseTransformPosition(GetMuzzleLocation());
	const FVector Barrel = AimInView - MuzzleInView;
	if (Barrel.SizeSquared() < 1.0)
	{
		return;
	}
	const FRotator Wanted = FRotationMatrix::MakeFromXZ(Barrel, FVector::UpVector).Rotator();
	const FRotator Current = bHasAimRotation ? AimPivot->GetRelativeRotation() : Wanted;
	AimPivot->SetRelativeRotation(FMath::RInterpTo(Current, Wanted, DeltaSeconds, AimTurnSpeed));
	bHasAimRotation = true;
}

void AHandGun::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The pawn / controller may not exist yet when the gun is spawned (or may be replaced).
	if (!bAttached || !GetAttachParentActor())
	{
		bAttached = false;
		TryAttachToView();
	}
	if (!BoundController.IsValid())
	{
		TryBindInput();
	}
	UpdatePose(DeltaSeconds);

	if (FlashRemaining > 0.f)
	{
		FlashRemaining -= DeltaSeconds;
		if (FlashRemaining <= 0.f)
		{
			if (FlashComponent)
			{
				FlashComponent->SetHiddenInGame(true);
			}
			FlashLight->SetVisibility(false);
		}
	}
	UpdateTracers(DeltaSeconds);
}

FVector AHandGun::GetMuzzleLocation() const
{
	return MuzzlePoint->GetComponentLocation();
}

bool AHandGun::GetEye(FVector& OutLocation, FVector& OutDirection) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	// The pawn's camera carries the live HMD pose (and follows any play-space shift made this frame).
	if (const UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr)
	{
		OutLocation = Camera->GetComponentLocation();
		OutDirection = Camera->GetForwardVector();
		return true;
	}
	if (PC && PC->PlayerCameraManager)
	{
		OutLocation = PC->PlayerCameraManager->GetCameraLocation();
		OutDirection = PC->PlayerCameraManager->GetCameraRotation().Vector();
		return true;
	}
	return false;
}

FCollisionQueryParams AHandGun::MakeQueryParams() const
{
	// Never hit the shooter: the pawn, the gun, and everything riding on the pawn (hands, wrist panel, HUD).
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HandGunShot), /*bTraceComplex*/ false, this);
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->GetPawn())
	{
		APawn* Pawn = PC->GetPawn();
		Params.AddIgnoredActor(Pawn);
		TArray<AActor*> Attached;
		Pawn->GetAttachedActors(Attached, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
		Params.AddIgnoredActors(Attached);
	}
	return Params;
}

bool AHandGun::TraceShot(const FVector& Eye, const FVector& LookDirection, const FVector& Muzzle, FHitResult& OutHit, FVector& OutEnd) const
{
	const UWorld* World = GetWorld();
	const FCollisionQueryParams Params = MakeQueryParams();

	// 1. The aim point: what's under the centre of the player's view.
	FHitResult LookHit;
	const bool bLookHit = World->LineTraceSingleByChannel(LookHit, Eye, Eye + LookDirection * MaxRange, TraceChannel, Params);
	const FVector AimPoint = bLookHit ? FVector(LookHit.ImpactPoint) : Eye + LookDirection * MaxRange;

	// 2. Gun poked through a wall (or round a corner the eye can't see past): the bullet stops in that wall.
	//    Without this the muzzle trace would start on the far side and shoot through it.
	FHitResult Blocked;
	if (World->LineTraceSingleByChannel(Blocked, Eye, Muzzle, TraceChannel, Params))
	{
		OutHit = Blocked;
		OutEnd = Blocked.ImpactPoint;
		UE_LOG(LogHandGun, Verbose, TEXT("Shot: muzzle hidden from the eye by %s"), *GetNameSafe(Blocked.GetActor()));
		return true;
	}

	// 3. From the muzzle through the aim point. If nothing is in the gun's way this hits exactly what was looked at.
	const FVector ToAim = AimPoint - Muzzle;
	const double AimDistance = ToAim.Size();
	if (AimDistance < UE_KINDA_SMALL_NUMBER)
	{
		OutHit = LookHit;
		OutEnd = AimPoint;
		return bLookHit;
	}
	const FVector ShotDirection = ToAim / AimDistance;
	const FVector ShotEnd = bLookHit ? AimPoint + ShotDirection * AimOvershoot : AimPoint;
	if (World->LineTraceSingleByChannel(OutHit, Muzzle, ShotEnd, TraceChannel, Params))
	{
		OutEnd = OutHit.ImpactPoint;
		UE_LOG(LogHandGun, Verbose, TEXT("Shot: aim %s on %s, hit %s on %s (%.1f cm from aim, convergence %.1f deg)"),
			*AimPoint.ToCompactString(), *GetNameSafe(LookHit.GetActor()), *OutEnd.ToCompactString(), *GetNameSafe(OutHit.GetActor()),
			FVector::Dist(OutEnd, AimPoint), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(ShotDirection | LookDirection, -1.0, 1.0))));
		return true;
	}
	OutEnd = ShotEnd;
	UE_LOG(LogHandGun, Verbose, TEXT("Shot: aim %s, missed everything"), *AimPoint.ToCompactString());
	return false;
}

void AHandGun::Fire()
{
	FireFrom(EControllerHand::Right);
}

void AHandGun::FireFrom(EControllerHand TriggerHand)
{
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now - LastShotTime < MinShotInterval)
	{
		return;
	}
	FVector Eye, LookDirection;
	if (!GetEye(Eye, LookDirection))
	{
		return;
	}
	LastShotTime = Now;

	const FVector Muzzle = GetMuzzleLocation();
	FHitResult Hit;
	FVector End;
	const bool bHit = TraceShot(Eye, LookDirection, Muzzle, Hit, End);
	const FVector ShotDirection = (End - Muzzle).GetSafeNormal(UE_SMALL_NUMBER, LookDirection);

	// Feedback first, so it's the same whether or not anything is hit.
	if (FlashComponent)
	{
		FlashComponent->SetRelativeRotation(FRotator(0.f, 0.f, FMath::FRandRange(0.f, 360.f)));
		FlashComponent->SetHiddenInGame(false);
	}
	FlashLight->SetVisibility(FlashLightIntensity > 0.f);
	FlashRemaining = FlashTime;
	if (FireSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, FireSound, Muzzle, FireVolume);
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && FireHaptic)
	{
		PC->PlayHapticEffect(FireHaptic, TriggerHand); // kick in the hand that pulled the trigger
	}
	SpawnTracer(Muzzle, End);

	if (!bHit)
	{
		return;
	}
	if (AActor* HitActor = Hit.GetActor(); HitActor && Damage > 0.f)
	{
		UGameplayStatics::ApplyPointDamage(HitActor, Damage, ShotDirection, Hit, PC, this, UDamageType::StaticClass());
	}
	UPrimitiveComponent* HitComponent = Hit.GetComponent();
	if (HitComponent && HitComponent->IsSimulatingPhysics(Hit.BoneName))
	{
		HitComponent->AddVelocityChangeImpulseAtLocation(ShotDirection * Knockback, Hit.ImpactPoint, Hit.BoneName);
	}
	else
	{
		StampImpact(Hit);
	}
}

void AHandGun::SpawnTracer(const FVector& Start, const FVector& End)
{
	const FVector Path = End - Start;
	const double Length = Path.Size();
	if (Length < 1.0 || Tracers.IsEmpty())
	{
		return;
	}
	// Reuse a free tracer, else the one furthest along (it's nearly done anyway).
	FTracer* Slot = Tracers.FindByPredicate([](const FTracer& Tracer) { return !Tracer.bActive; });
	if (!Slot)
	{
		Slot = &Tracers[0];
		for (FTracer& Tracer : Tracers)
		{
			if (Tracer.Travelled / Tracer.Length > Slot->Travelled / Slot->Length)
			{
				Slot = &Tracer;
			}
		}
	}
	Slot->Start = Start;
	Slot->Direction = Path / Length;
	Slot->Length = Length;
	Slot->Travelled = 0.0;
	Slot->bActive = Slot->Mesh != nullptr;
}

void AHandGun::UpdateTracers(float DeltaSeconds)
{
	for (FTracer& Tracer : Tracers)
	{
		if (!Tracer.bActive)
		{
			continue;
		}
		Tracer.Travelled += TracerSpeed * DeltaSeconds;
		// The streak's head runs ahead to the hit, its tail follows TracerLength behind; done once the tail arrives.
		const double Head = FMath::Min(Tracer.Travelled, Tracer.Length);
		const double Tail = FMath::Clamp(Tracer.Travelled - TracerLength, 0.0, Tracer.Length);
		if (Tail >= Tracer.Length - 0.5 || Head - Tail < 0.5)
		{
			Tracer.bActive = Tail < Tracer.Length - 0.5;
			Tracer.Mesh->SetHiddenInGame(true);
			continue;
		}
		const FVector Centre = Tracer.Start + Tracer.Direction * ((Head + Tail) * 0.5);
		const double Radius = TracerRadius / MeshRadius;
		Tracer.Mesh->SetWorldTransform(FTransform(FRotationMatrix::MakeFromZ(Tracer.Direction).ToQuat(), Centre,
			FVector(Radius, Radius, (Head - Tail) / MeshLength)));
		Tracer.Mesh->SetHiddenInGame(false);
	}
}

void AHandGun::StampImpact(const FHitResult& Hit)
{
	if (!ImpactDecalMaterial)
	{
		return;
	}
	ActiveDecals.RemoveAll([](const TWeakObjectPtr<UDecalComponent>& Decal) { return !Decal.IsValid(); });
	while (ActiveDecals.Num() >= MaxImpactDecals)
	{
		if (UDecalComponent* Oldest = ActiveDecals[0].Get())
		{
			Oldest->DestroyComponent();
		}
		ActiveDecals.RemoveAt(0);
	}
	// Decals project along X: point it into the surface, random roll.
	FRotator Rotation = (-Hit.ImpactNormal).Rotation();
	Rotation.Roll = FMath::FRandRange(0.f, 360.f);
	const FVector Size(3.0, ImpactDecalSize, ImpactDecalSize);
	UDecalComponent* Decal = nullptr;
	if (USceneComponent* HitComponent = Hit.GetComponent())
	{
		Decal = UGameplayStatics::SpawnDecalAttached(ImpactDecalMaterial, Size, HitComponent, Hit.BoneName, Hit.ImpactPoint, Rotation,
			EAttachLocation::KeepWorldPosition, ImpactDecalLifeSpan);
	}
	else
	{
		Decal = UGameplayStatics::SpawnDecalAtLocation(this, ImpactDecalMaterial, Size, Hit.ImpactPoint, Rotation, ImpactDecalLifeSpan);
	}
	if (Decal)
	{
		ActiveDecals.Add(Decal);
	}
}
