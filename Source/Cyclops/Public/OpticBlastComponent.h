#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "OpticBlastComponent.generated.h"

class APlayerController;
class UAudioComponent;
class UDecalComponent;
class USoundBase;
class UInputAction;
class UInputMappingContext;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FInputActionValue;

/**
 * Cyclops-style optic blast fired from the player's view (HMD in VR).
 *
 * While the fire action is held, every tick it traces from the camera, stretches a beam mesh
 * from just below the eyes to the impact, damages and pushes what it hits, and stamps scorch decals
 * attached to it. Power comes from an energy charge (see the Energy category): a strong opening
 * burst, then a draining sustain, then recharge on release. A head-locked bar shows the charge.
 *
 * Add to any actor in the level (it binds input on the owning actor). The mapping context must
 * also be listed in Project Settings > Enhanced Input > Default Mapping Contexts so that OpenXR
 * creates controller bindings for it at session start.
 */
UCLASS(ClassGroup = (Cyclops), meta = (BlueprintSpawnableComponent))
class CYCLOPS_API UOpticBlastComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOpticBlastComponent();

	/** Starts the beam as if the fire button had been pressed. */
	UFUNCTION(BlueprintCallable, Category = "Optic Blast")
	void StartFiring();

	/** Stops the beam as if the fire button had been released. */
	UFUNCTION(BlueprintCallable, Category = "Optic Blast")
	void StopFiring();

	UFUNCTION(BlueprintPure, Category = "Optic Blast")
	bool IsFiring() const { return bIsFiring; }

	// ---- Input ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Input")
	TObjectPtr<UInputAction> FireAction;

	/** Added at BeginPlay if not already active. Must also be a Default Mapping Context for XR controllers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Input")
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Input")
	int32 MappingContextPriority = 1;

	// ---- Beam ----

	/** Max distance of the beam, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (ClampMin = "100", Units = "cm"))
	float MaxRange = 5000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;

	/** How far in front of the eyes the visible beam starts. Larger is more comfortable in VR. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (Units = "cm"))
	float BeamStartForward = 20.f;

	/** How far below the eyes the visible beam starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (Units = "cm"))
	float BeamStartDown = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (ClampMin = "0.1", Units = "cm"))
	float BeamRadius = 2.5f;

	/** Beam mesh: expected to be a Z-up cylinder centred on its pivot (e.g. /Engine/BasicShapes/Cylinder). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Beam")
	TObjectPtr<UStaticMesh> BeamMesh;

	/** Native radius of BeamMesh at scale 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Beam", AdvancedDisplay, meta = (ClampMin = "0.01"))
	float BeamMeshRadius = 50.f;

	/** Native length of BeamMesh along Z at scale 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Beam", AdvancedDisplay, meta = (ClampMin = "0.01"))
	float BeamMeshLength = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Beam")
	TObjectPtr<UMaterialInterface> BeamMaterial;

	/** Optional wider, dimmer shell drawn around the core beam so it reads as light rather than a tube. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Beam")
	TObjectPtr<UMaterialInterface> BeamGlowMaterial;

	/** Glow shell radius as a multiple of the core beam radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (ClampMin = "1"))
	float GlowRadiusScale = 3.f;

	/**
	 * How far along the beam (from its start point) the core mesh begins. Near the eyes the beam is faded out
	 * by the material anyway, but the mesh would still cover a large patch of each eye and cost GPU shading;
	 * starting it further out removes that overdraw.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (ClampMin = "0", Units = "cm"))
	float CoreStartOffset = 10.f;

	/** Same for the (wider, so costlier up close) glow shell. Pair with the glow material's NearFadeStart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Beam", meta = (ClampMin = "0", Units = "cm"))
	float GlowStartOffset = 45.f;

	// ---- Flicker ----
	// Brightness flicker is written to custom primitive data index 0 (the "Pulse" material parameter).

	/** Smooth width wobble as a fraction of BeamRadius (0.25 = +/-25%). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Flicker", meta = (ClampMin = "0", ClampMax = "1"))
	float WidthFlickerAmount = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Flicker", meta = (ClampMin = "0"))
	float WidthFlickerSpeed = 14.f;

	/** Per-frame random width jitter as a fraction of BeamRadius, on top of the smooth wobble. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Flicker", meta = (ClampMin = "0", ClampMax = "1"))
	float WidthJitter = 0.06f;

	/** Brightness wobble as a fraction of full brightness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Flicker", meta = (ClampMin = "0", ClampMax = "1"))
	float BrightnessFlickerAmount = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Flicker", meta = (ClampMin = "0"))
	float BrightnessFlickerSpeed = 9.f;

	// ---- Energy ----
	// Energy (0..1) is the beam's single resource. Pressing fire spends a burst scaled by stored energy on
	// the first frame only (miss it and it's gone); holding then drains energy, and sustained damage, push,
	// width, brightness, audio and haptics all scale with what's left. Energy only recharges while released.

	/** Seconds of holding to drain a full charge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0.1", Units = "s"))
	float DrainTime = 1.5f;

	/** Pause after release before recharging starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0", Units = "s"))
	float RechargeDelay = 0.25f;

	/** Seconds to recharge from empty to full. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0.1", Units = "s"))
	float RechargeTime = 1.2f;

	/** Below this the beam won't fire at all (dry sputter instead). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0", ClampMax = "1"))
	float MinEnergyToFire = 0.08f;

	/** Sustained power = Energy ^ PowerCurve. <1 keeps the beam strong for longer before it tails off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0.1"))
	float PowerCurve = 0.7f;

	/** Damage of the opening hit at full charge (scaled by stored energy). Enemies have 50 HP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0"))
	float BurstDamage = 60.f;

	/** Instant velocity change given to a simulating body by the opening hit at full charge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0", Units = "cm/s"))
	float BurstKnockback = 900.f;

	/** How long the burst's extra width/brightness takes to settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0.01", Units = "s"))
	float BurstVisualTime = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "1"))
	float BurstWidthScale = 2.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "1"))
	float BurstBrightness = 2.5f;

	/** Beam width (fraction of BeamRadius) when nearly out of energy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy", meta = (ClampMin = "0.05", ClampMax = "1"))
	float MinPowerWidth = 0.35f;

	UFUNCTION(BlueprintPure, Category = "Optic Blast")
	float GetEnergy() const { return Energy; }

	/**
	 * While frozen (game paused) energy neither drains nor recharges, but the beam still fires at full
	 * power so menu buttons can be pressed without spending charge.
	 */
	UFUNCTION(BlueprintCallable, Category = "Optic Blast")
	void SetEnergyFrozen(bool bFrozen);

	/** Refill to full (new game). */
	UFUNCTION(BlueprintCallable, Category = "Optic Blast")
	void ResetEnergy();

	// ---- Physics ----

	/** Acceleration applied to simulating bodies at full power (mass independent), in cm/s^2. 980 = 1g. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Physics", meta = (ClampMin = "0"))
	float BeamAcceleration = 6000.f;

	// ---- Damage ----

	/** Sustained point damage per second at full power (UE damage events, shot direction = beam). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Damage", meta = (ClampMin = "0"))
	float DamagePerSecond = 100.f;

	// ---- Scorch decals ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Scorch")
	TObjectPtr<UMaterialInterface> ScorchDecalMaterial;

	/** Minimum distance the impact point must move before another decal is stamped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch", meta = (ClampMin = "0.1", Units = "cm"))
	float DecalSpacing = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch", meta = (ClampMin = "0.1", Units = "cm"))
	float DecalSize = 10.f;

	/** Projection depth of the decal box. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch", meta = (ClampMin = "0.1", Units = "cm"))
	float DecalDepth = 4.f;

	/** Seconds before a decal is removed. 0 = never. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch", meta = (ClampMin = "0", Units = "s"))
	float DecalLifeSpan = 30.f;

	/** Most scorch decals alive at once; the oldest is removed when exceeded (each one is an extra draw on mobile). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch", meta = (ClampMin = "1"))
	int32 MaxScorchDecals = 60;

	/** Whether moving physics bodies get scorched (they collect many decals quickly as they tumble). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Scorch")
	bool bScorchSimulatingBodies = false;

	// ---- Impact light ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Impact Light")
	bool bUseImpactLight = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Impact Light")
	FLinearColor ImpactLightColor = FLinearColor(1.f, 0.12f, 0.04f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Impact Light", meta = (ClampMin = "0"))
	float ImpactLightIntensity = 300.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Impact Light", meta = (ClampMin = "0", Units = "cm"))
	float ImpactLightRadius = 400.f;

	/** How far off the surface the light sits so it doesn't clip into it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Impact Light", meta = (Units = "cm"))
	float ImpactLightSurfaceOffset = 15.f;

	// ---- Audio (2D: it comes from your own eyes) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Audio")
	TObjectPtr<USoundBase> BurstSound;

	/** Looping; volume and pitch follow beam power. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Audio")
	TObjectPtr<USoundBase> BeamLoopSound;

	/** Played when the beam runs dry (or when firing with too little energy). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Audio")
	TObjectPtr<USoundBase> FizzleSound;

	/** Played when energy is back to full. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Audio")
	TObjectPtr<USoundBase> ReadySound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Audio")
	FVector2D LoopVolumeRange = FVector2D(0.15f, 0.8f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Audio")
	FVector2D LoopPitchRange = FVector2D(0.7f, 1.15f);

	// ---- Haptics (both controllers, follow power) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Haptics", meta = (ClampMin = "0", ClampMax = "1"))
	float HapticBurstAmplitude = 1.f;

	/** Amplitude range of the sustained rumble from empty to full power. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Haptics")
	FVector2D HapticSustainRange = FVector2D(0.1f, 0.5f);

	// ---- Energy bar (head-locked, low in view) ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Energy Bar")
	bool bShowEnergyBar = true;

	/** A 100 cm cube (engine BasicShapes/Cube). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Energy Bar")
	TObjectPtr<UStaticMesh> EnergyBarMesh;

	/** Unlit material reading its colour from custom primitive data 0-3. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Optic Blast|Energy Bar")
	TObjectPtr<UMaterialInterface> EnergyBarMaterial;

	/** Bar centre in camera space (X forward, Y right, Z up). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy Bar", meta = (Units = "cm"))
	FVector EnergyBarOffset = FVector(55.0, 0.0, -21.0);

	/** Width x height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Energy Bar", meta = (Units = "cm"))
	FVector2D EnergyBarSize = FVector2D(22.f, 1.1f);

	// ---- Debug ----

	/** Fire continuously without input (testing / demos). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Optic Blast|Debug")
	bool bDebugAutoFire = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void SetupInput(APlayerController& PC);
	void CreateBeamVisuals();
	void UpdateEnergy(float DeltaTime);
	void UpdateBeam(float DeltaTime, float Power, float BurstStrength);
	void UpdateFeedback(float Power);
	void SetBeamVisible(bool bVisible);
	void SetImpactLightVisible(bool bVisible);
	void StampScorch(const FHitResult& Hit, float SizeScale);
	void Deplete();
	void SetHaptics(float Amplitude);
	void PlaySound2D(USoundBase* Sound, float Volume = 1.f) const;
	bool TryCreateEnergyBar();
	void UpdateEnergyBar(float Power);

	void HandleFireStarted(const FInputActionValue& Value);
	void HandleFireStopped(const FInputActionValue& Value);

	APlayerController* GetPlayerController() const;

	UStaticMeshComponent* CreateBeamMesh(FName Name, UMaterialInterface* Material);

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BeamMeshComponent;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BeamGlowComponent;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> ImpactLightComponent;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> LoopAudio;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BarBackground;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BarFill;

	TWeakObjectPtr<APlayerController> CachedPC;

	/** Live scorch decals, oldest first, for MaxScorchDecals. */
	TArray<TWeakObjectPtr<UDecalComponent>> ActiveDecals;

	FVector LastDecalLocation = FVector(0.0, 0.0, -1.0e9);
	float Energy = 1.f;
	float BurstEnergy = 0.f;      // stored energy at the moment of firing; consumed by the first beam frame
	float BurstFlash = 0.f;       // counts down from BurstVisualTime after a burst
	float ReadyFlash = 0.f;       // counts down after recharge completes
	float TimeSinceRelease = 0.f;
	float LastHaptic = 0.f;
	bool bIsFiring = false;
	bool bDepleted = false;       // ran dry while held: no beam until released
	bool bBurstPending = false;
	bool bEnergyFrozen = false;
	bool bBeamVisible = false;
	bool bImpactLightVisible = false;
};
