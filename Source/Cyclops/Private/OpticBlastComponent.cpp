#include "OpticBlastComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpticBlast, Log, All);

namespace
{
	const FLinearColor BarFullColour(0.15f, 0.85f, 1.f);
	const FLinearColor BarLowColour(1.f, 0.12f, 0.04f);
	const FLinearColor BarBackColour(0.015f, 0.015f, 0.02f);
}

UOpticBlastComponent::UOpticBlastComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Run after the player controller has updated the camera manager for this frame.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UOpticBlastComponent::BeginPlay()
{
	Super::BeginPlay();

	CreateBeamVisuals();

	APlayerController* PC = GetPlayerController();
	if (!PC)
	{
		UE_LOG(LogOpticBlast, Warning, TEXT("%s: no local player controller at BeginPlay; optic blast input disabled."), *GetPathName());
		return;
	}
	SetupInput(*PC);
}

void UOpticBlastComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopFiring();

	if (APlayerController* PC = CachedPC.Get())
	{
		GetOwner()->DisableInput(PC);
	}

	Super::EndPlay(EndPlayReason);
}

APlayerController* UOpticBlastComponent::GetPlayerController() const
{
	if (APlayerController* PC = CachedPC.Get())
	{
		return PC;
	}
	const UWorld* World = GetWorld();
	return World ? World->GetFirstPlayerController() : nullptr;
}

void UOpticBlastComponent::SetupInput(APlayerController& PC)
{
	CachedPC = &PC;

	if (MappingContext)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC.GetLocalPlayer()))
		{
			if (!Subsystem->HasMappingContext(MappingContext))
			{
				Subsystem->AddMappingContext(MappingContext, MappingContextPriority);
			}
		}
	}

	if (!FireAction)
	{
		UE_LOG(LogOpticBlast, Warning, TEXT("%s: FireAction not set; only bDebugAutoFire / StartFiring() will fire the beam."), *GetPathName());
		return;
	}

	AActor* Owner = GetOwner();
	Owner->EnableInput(&PC);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(Owner->InputComponent);
	if (!Input)
	{
		UE_LOG(LogOpticBlast, Error, TEXT("%s: owner input component is not a UEnhancedInputComponent; check DefaultInputComponentClass."), *GetPathName());
		return;
	}

	Input->BindAction(FireAction, ETriggerEvent::Started, this, &UOpticBlastComponent::HandleFireStarted);
	Input->BindAction(FireAction, ETriggerEvent::Completed, this, &UOpticBlastComponent::HandleFireStopped);
	Input->BindAction(FireAction, ETriggerEvent::Canceled, this, &UOpticBlastComponent::HandleFireStopped);
}

UStaticMeshComponent* UOpticBlastComponent::CreateBeamMesh(FName Name, UMaterialInterface* Material)
{
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(GetOwner(), Name);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetUsingAbsoluteLocation(true);
	Mesh->SetUsingAbsoluteRotation(true);
	Mesh->SetUsingAbsoluteScale(true);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetStaticMesh(BeamMesh);
	if (Material)
	{
		Mesh->SetMaterial(0, Material);
	}
	Mesh->SetCustomPrimitiveDataFloat(0, 1.f);
	Mesh->SetHiddenInGame(true);
	Mesh->RegisterComponent();
	return Mesh;
}

void UOpticBlastComponent::CreateBeamVisuals()
{
	AActor* Owner = GetOwner();

	if (!BeamMesh)
	{
		UE_LOG(LogOpticBlast, Warning, TEXT("%s: BeamMesh not set; the beam will be invisible."), *GetPathName());
	}

	BeamMeshComponent = CreateBeamMesh(TEXT("OpticBeamMesh"), BeamMaterial);
	if (BeamGlowMaterial)
	{
		BeamGlowComponent = CreateBeamMesh(TEXT("OpticBeamGlow"), BeamGlowMaterial);
	}

	if (bUseImpactLight)
	{
		ImpactLightComponent = NewObject<UPointLightComponent>(Owner, TEXT("OpticBeamImpactLight"));
		ImpactLightComponent->SetMobility(EComponentMobility::Movable);
		ImpactLightComponent->SetUsingAbsoluteLocation(true);
		ImpactLightComponent->SetIntensityUnits(ELightUnits::Candelas);
		ImpactLightComponent->SetIntensity(ImpactLightIntensity);
		ImpactLightComponent->SetLightColor(ImpactLightColor);
		ImpactLightComponent->SetAttenuationRadius(ImpactLightRadius);
		ImpactLightComponent->SetCastShadows(false);
		ImpactLightComponent->SetHiddenInGame(true);
		ImpactLightComponent->RegisterComponent();
	}

	if (BeamLoopSound)
	{
		LoopAudio = NewObject<UAudioComponent>(Owner, TEXT("OpticBeamLoopAudio"));
		LoopAudio->bAutoActivate = false;
		LoopAudio->bAllowSpatialization = false;
		LoopAudio->SetSound(BeamLoopSound);
		LoopAudio->RegisterComponent();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Firing / energy

void UOpticBlastComponent::StartFiring()
{
	if (bIsFiring)
	{
		return;
	}
	bIsFiring = true;

	if (bEnergyFrozen)
	{
		// Paused: fire at full power for the menu without touching stored energy.
		bDepleted = false;
		bBurstPending = true;
		BurstEnergy = 1.f;
		BurstFlash = BurstVisualTime;
		PlaySound2D(BurstSound, 0.6f);
		if (LoopAudio)
		{
			LoopAudio->Play();
		}
		return;
	}

	if (Energy < MinEnergyToFire)
	{
		// Not enough charge: dry sputter, no beam until released and recharged.
		bDepleted = true;
		PlaySound2D(FizzleSound, 0.5f);
		return;
	}

	bDepleted = false;
	bBurstPending = true;
	BurstEnergy = Energy;
	BurstFlash = BurstVisualTime;
	PlaySound2D(BurstSound, FMath::Lerp(0.35f, 1.f, Energy));
	if (LoopAudio)
	{
		LoopAudio->Play();
	}
}

void UOpticBlastComponent::StopFiring()
{
	if (!bIsFiring)
	{
		return;
	}
	bIsFiring = false;
	bBurstPending = false;
	TimeSinceRelease = 0.f;
	if (LoopAudio)
	{
		LoopAudio->Stop();
	}
	SetHaptics(0.f);
}

void UOpticBlastComponent::Deplete()
{
	Energy = 0.f;
	bDepleted = true;
	if (LoopAudio)
	{
		LoopAudio->Stop();
	}
	PlaySound2D(FizzleSound);
	SetBeamVisible(false);
	SetImpactLightVisible(false);
	SetHaptics(0.f);
}

void UOpticBlastComponent::HandleFireStarted(const FInputActionValue& Value)
{
	StartFiring();
}

void UOpticBlastComponent::HandleFireStopped(const FInputActionValue& Value)
{
	StopFiring();
}

void UOpticBlastComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Debug auto-fire behaves like a player: press when full, release when dry.
	if (bDebugAutoFire)
	{
		if (!bIsFiring && Energy >= 1.f)
		{
			StartFiring();
		}
		else if (bIsFiring && bDepleted)
		{
			StopFiring();
		}
	}

	UpdateEnergy(DeltaTime);
}

void UOpticBlastComponent::SetEnergyFrozen(bool bFrozen)
{
	if (bEnergyFrozen == bFrozen)
	{
		return;
	}
	bEnergyFrozen = bFrozen;

	if (bIsFiring)
	{
		if (bFrozen)
		{
			// Allow the menu beam even if the player paused while holding an empty beam.
			if (bDepleted && LoopAudio)
			{
				LoopAudio->Play();
			}
			bDepleted = false;
		}
		else if (Energy < MinEnergyToFire)
		{
			Deplete();
		}
	}
	TimeSinceRelease = 0.f; // recharge delay restarts after unpausing
}

void UOpticBlastComponent::ResetEnergy()
{
	Energy = 1.f;
	bDepleted = false;
	ReadyFlash = 0.f;
}

void UOpticBlastComponent::UpdateEnergy(float DeltaTime)
{
	float Power = 0.f;

	if (bEnergyFrozen)
	{
		if (bIsFiring)
		{
			Power = 1.f;
			UpdateBeam(DeltaTime, Power, bBurstPending ? BurstEnergy : 0.f);
			bBurstPending = false;
		}
		else if (bBeamVisible)
		{
			SetBeamVisible(false);
			SetImpactLightVisible(false);
		}
		BurstFlash = FMath::Max(0.f, BurstFlash - DeltaTime);
		UpdateFeedback(Power);
		return;
	}

	if (bIsFiring && !bDepleted)
	{
		Power = FMath::Pow(FMath::Max(Energy, 0.f), PowerCurve);
		UpdateBeam(DeltaTime, Power, bBurstPending ? BurstEnergy : 0.f);
		bBurstPending = false;

		Energy -= DeltaTime / DrainTime;
		if (Energy <= 0.f)
		{
			Deplete();
			Power = 0.f;
		}
	}
	else
	{
		if (bBeamVisible)
		{
			SetBeamVisible(false);
			SetImpactLightVisible(false);
		}

		if (!bIsFiring)
		{
			TimeSinceRelease += DeltaTime;
			if (TimeSinceRelease >= RechargeDelay && Energy < 1.f)
			{
				Energy = FMath::Min(1.f, Energy + DeltaTime / RechargeTime);
				if (Energy >= 1.f)
				{
					PlaySound2D(ReadySound, 0.6f);
					ReadyFlash = 0.2f;
				}
			}
		}
	}

	BurstFlash = FMath::Max(0.f, BurstFlash - DeltaTime);
	ReadyFlash = FMath::Max(0.f, ReadyFlash - DeltaTime);
	UpdateFeedback(Power);
}

void UOpticBlastComponent::UpdateFeedback(float Power)
{
	const bool bBeamOn = bIsFiring && !bDepleted;

	if (LoopAudio && bBeamOn)
	{
		LoopAudio->SetVolumeMultiplier(FMath::Lerp(LoopVolumeRange.X, LoopVolumeRange.Y, Power));
		LoopAudio->SetPitchMultiplier(FMath::Lerp(LoopPitchRange.X, LoopPitchRange.Y, Power));
	}

	if (bBeamOn)
	{
		const float Sustain = FMath::Lerp(HapticSustainRange.X, HapticSustainRange.Y, Power);
		SetHaptics(BurstFlash > 0.f ? FMath::Max(Sustain, HapticBurstAmplitude * BurstEnergy) : Sustain);
	}

	UpdateEnergyBar(Power);
}

void UOpticBlastComponent::SetHaptics(float Amplitude)
{
	// Avoid re-sending identical values every frame.
	if (FMath::IsNearlyEqual(Amplitude, LastHaptic, 0.02f) && !(Amplitude == 0.f && LastHaptic != 0.f))
	{
		return;
	}
	LastHaptic = Amplitude;
	if (APlayerController* PC = GetPlayerController())
	{
		PC->SetHapticsByValue(1.f, Amplitude, EControllerHand::Left);
		PC->SetHapticsByValue(1.f, Amplitude, EControllerHand::Right);
	}
}

void UOpticBlastComponent::PlaySound2D(USoundBase* Sound, float Volume) const
{
	if (Sound)
	{
		UGameplayStatics::PlaySound2D(this, Sound, Volume);
	}
}

// ---------------------------------------------------------------------------------------------------------
// Beam

void UOpticBlastComponent::UpdateBeam(float DeltaTime, float Power, float BurstStrength)
{
	APlayerController* PC = GetPlayerController();
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return;
	}

	const FVector EyeLocation = Camera->GetCameraLocation();
	const FRotator EyeRotation = Camera->GetCameraRotation();
	const FVector Forward = EyeRotation.Vector();
	const FVector Up = FRotationMatrix(EyeRotation).GetUnitAxis(EAxis::Z);

	const FVector TraceEnd = EyeLocation + Forward * MaxRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(OpticBlast), /*bTraceComplex*/ false, GetOwner());
	Params.AddIgnoredActor(PC->GetPawn());

	FHitResult Hit;
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, EyeLocation, TraceEnd, TraceChannel, Params);
	const FVector EndPoint = bHit ? Hit.ImpactPoint : TraceEnd;

	// 0..1 while the burst's extra width/brightness settles.
	const float BurstAlpha = BurstVisualTime > 0.f ? BurstFlash / BurstVisualTime : 0.f;

	// Visible beam starts a little below/in front of the eyes and converges on the aim point.
	const FVector BeamStart = EyeLocation + Forward * BeamStartForward - Up * BeamStartDown;
	const FVector BeamVector = EndPoint - BeamStart;
	const double BeamLength = BeamVector.Size();

	if (BeamMeshComponent && BeamLength > UE_KINDA_SMALL_NUMBER)
	{
		// Flicker: smooth Perlin wobble plus a little per-frame jitter on width; Perlin pulse on brightness.
		const float Time = GetWorld()->GetTimeSeconds();
		const float Flicker = FMath::Max(0.05f,
			1.f + WidthFlickerAmount * FMath::PerlinNoise1D(Time * WidthFlickerSpeed)
				+ FMath::FRandRange(-WidthJitter, WidthJitter));
		const float WidthScale = Flicker * FMath::Lerp(MinPowerWidth, 1.f, Power) * (1.f + (BurstWidthScale - 1.f) * BurstAlpha);
		const float Pulse = FMath::Max(0.f, 1.f + BrightnessFlickerAmount * FMath::PerlinNoise1D(Time * BrightnessFlickerSpeed + 37.f))
			* FMath::Lerp(0.45f, 1.f, Power) * (1.f + (BurstBrightness - 1.f) * BurstAlpha);

		const FQuat BeamRotation = FRotationMatrix::MakeFromZ(BeamVector).ToQuat();
		const FVector BeamDir = BeamVector / BeamLength;
		const double RadiusScale = BeamRadius * WidthScale / BeamMeshRadius;

		// Each mesh covers [StartOffset, end] of the beam; the part nearest the eyes is skipped (see CoreStartOffset).
		auto PlaceSegment = [&](UStaticMeshComponent* Mesh, float StartOffset, double Radius)
		{
			const double Start = FMath::Min<double>(StartOffset, BeamLength * 0.5);
			const double Length = BeamLength - Start;
			const FVector Centre = BeamStart + BeamDir * (Start + Length * 0.5);
			Mesh->SetWorldTransform(FTransform(BeamRotation, Centre, FVector(Radius, Radius, Length / BeamMeshLength)));
			Mesh->SetCustomPrimitiveDataFloat(0, Pulse);
		};

		PlaceSegment(BeamMeshComponent, CoreStartOffset, RadiusScale);
		if (BeamGlowComponent)
		{
			PlaceSegment(BeamGlowComponent, GlowStartOffset, RadiusScale * GlowRadiusScale);
		}

		SetBeamVisible(true);
	}

	if (!bHit)
	{
		SetImpactLightVisible(false);
		return; // a burst that hits nothing is wasted
	}

	if (ImpactLightComponent)
	{
		ImpactLightComponent->SetWorldLocation(Hit.ImpactPoint + Hit.ImpactNormal * ImpactLightSurfaceOffset);
		ImpactLightComponent->SetIntensity(ImpactLightIntensity * (FMath::Lerp(0.3f, 1.f, Power) + 2.f * BurstAlpha));
		SetImpactLightVisible(true);
	}

	const float Damage = DamagePerSecond * Power * DeltaTime + BurstDamage * BurstStrength;
	if (AActor* HitActor = Hit.GetActor(); HitActor && Damage > 0.f)
	{
		UGameplayStatics::ApplyPointDamage(HitActor, Damage, Forward, Hit, PC, GetOwner(), UDamageType::StaticClass());
	}

	UPrimitiveComponent* HitComponent = Hit.GetComponent();
	if (HitComponent && HitComponent->IsSimulatingPhysics(Hit.BoneName))
	{
		const float VelocityChange = BeamAcceleration * Power * DeltaTime + BurstKnockback * BurstStrength;
		HitComponent->AddVelocityChangeImpulseAtLocation(Forward * VelocityChange, Hit.ImpactPoint, Hit.BoneName);
	}

	if (BurstStrength > 0.f)
	{
		StampScorch(Hit, 1.f + 0.8f * BurstStrength); // the opening hit leaves a bigger mark
	}
	else if (FVector::DistSquared(Hit.ImpactPoint, LastDecalLocation) > FMath::Square(DecalSpacing))
	{
		StampScorch(Hit, FMath::Lerp(0.5f, 1.f, Power));
	}
}

void UOpticBlastComponent::StampScorch(const FHitResult& Hit, float SizeScale)
{
	if (!ScorchDecalMaterial)
	{
		return;
	}

	const UPrimitiveComponent* HitPrimitive = Hit.GetComponent();
	if (!bScorchSimulatingBodies && HitPrimitive && HitPrimitive->IsSimulatingPhysics(Hit.BoneName))
	{
		return;
	}

	LastDecalLocation = Hit.ImpactPoint;

	// Enforce the budget: drop expired entries, then remove the oldest live decals.
	ActiveDecals.RemoveAll([](const TWeakObjectPtr<UDecalComponent>& Decal) { return !Decal.IsValid(); });
	while (ActiveDecals.Num() >= MaxScorchDecals)
	{
		if (UDecalComponent* Oldest = ActiveDecals[0].Get())
		{
			Oldest->DestroyComponent();
		}
		ActiveDecals.RemoveAt(0);
	}

	// Decals project along their X axis, so point X into the surface; random roll varies the marks.
	FRotator DecalRotation = (-Hit.ImpactNormal).Rotation();
	DecalRotation.Roll = FMath::FRandRange(0.f, 360.f);
	const FVector Size(DecalDepth, DecalSize * SizeScale, DecalSize * SizeScale);

	UDecalComponent* Decal = nullptr;
	if (USceneComponent* HitComponent = Hit.GetComponent())
	{
		// Attached so marks ride along on moving objects (e.g. enemies).
		Decal = UGameplayStatics::SpawnDecalAttached(ScorchDecalMaterial, Size, HitComponent, Hit.BoneName, Hit.ImpactPoint,
			DecalRotation, EAttachLocation::KeepWorldPosition, DecalLifeSpan);
	}
	else
	{
		Decal = UGameplayStatics::SpawnDecalAtLocation(this, ScorchDecalMaterial, Size, Hit.ImpactPoint, DecalRotation, DecalLifeSpan);
	}
	if (Decal)
	{
		ActiveDecals.Add(Decal);
	}
}

void UOpticBlastComponent::SetBeamVisible(bool bVisible)
{
	if (bBeamVisible != bVisible && BeamMeshComponent)
	{
		bBeamVisible = bVisible;
		BeamMeshComponent->SetHiddenInGame(!bVisible);
		if (BeamGlowComponent)
		{
			BeamGlowComponent->SetHiddenInGame(!bVisible);
		}
	}
}

void UOpticBlastComponent::SetImpactLightVisible(bool bVisible)
{
	if (bImpactLightVisible != bVisible && ImpactLightComponent)
	{
		bImpactLightVisible = bVisible;
		ImpactLightComponent->SetHiddenInGame(!bVisible);
	}
}

// ---------------------------------------------------------------------------------------------------------
// Energy bar

bool UOpticBlastComponent::TryCreateEnergyBar()
{
	if (!bShowEnergyBar || !EnergyBarMesh)
	{
		return false;
	}
	const APlayerController* PC = GetPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UCameraComponent* Camera = Pawn ? Pawn->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!Camera)
	{
		return false;
	}

	auto MakeBarPart = [this, Camera](FName Name)
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(GetOwner(), Name);
		Part->SetStaticMesh(EnergyBarMesh);
		Part->SetMaterial(0, EnergyBarMaterial);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCastShadow(false);
		Part->RegisterComponent();
		// Head-locked: rides on the HMD camera.
		Part->AttachToComponent(Camera, FAttachmentTransformRules::KeepRelativeTransform);
		return Part;
	};

	BarBackground = MakeBarPart(TEXT("EnergyBarBackground"));
	BarBackground->SetRelativeLocation(EnergyBarOffset + FVector(0.3, 0.0, 0.0));
	BarBackground->SetRelativeScale3D(FVector(0.002, (EnergyBarSize.X + 0.6) / 100.0, (EnergyBarSize.Y + 0.6) / 100.0));
	BarBackground->SetCustomPrimitiveDataVector4(0, FVector4(BarBackColour.R, BarBackColour.G, BarBackColour.B, 1.0));

	BarFill = MakeBarPart(TEXT("EnergyBarFill"));
	return true;
}

void UOpticBlastComponent::UpdateEnergyBar(float Power)
{
	if (!BarFill && !TryCreateEnergyBar())
	{
		return;
	}

	// Fill shrinks towards the left edge.
	const double Width = EnergyBarSize.X * FMath::Clamp(Energy, 0.f, 1.f);
	BarFill->SetVisibility(Width > 0.05);
	BarFill->SetRelativeLocation(EnergyBarOffset + FVector(0.0, -EnergyBarSize.X * 0.5 + Width * 0.5, 0.0));
	BarFill->SetRelativeScale3D(FVector(0.002, FMath::Max(Width, 0.01) / 100.0, EnergyBarSize.Y / 100.0));

	FLinearColor Colour;
	if (bIsFiring && bDepleted)
	{
		// Held while empty: pulse dark red to say "let go".
		const float Pulse = 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 12.f);
		Colour = BarLowColour * FMath::Lerp(0.3f, 0.8f, Pulse);
	}
	else if (ReadyFlash > 0.f)
	{
		Colour = FLinearColor(1.f, 1.f, 1.f);
	}
	else if (bIsFiring)
	{
		Colour = FMath::Lerp(BarLowColour, BarFullColour, Power);
	}
	else
	{
		// Recharging reads dimmer until full.
		Colour = FMath::Lerp(BarLowColour, BarFullColour, Energy) * (Energy >= 1.f ? 1.f : 0.6f);
	}
	BarFill->SetCustomPrimitiveDataVector4(0, FVector4(Colour.R, Colour.G, Colour.B, 1.0));
}
