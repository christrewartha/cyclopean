#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "HandGun.generated.h"

class UDecalComponent;
class UHapticFeedbackEffect_Base;
class UInputAction;
class UMaterialInterface;
class UPointLightComponent;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogHandGun, Log, All);

/** A shot left the muzzle and ended at End (hit point or max range). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnGunShot, const FVector& /*Muzzle*/, const FVector& /*End*/);

/**
 * A pistol held FPS-style (the alternative to the optic blast): pull either trigger to fire one shot.
 *
 * The controllers are only buttons - the gun is fixed in view (head-locked, low and to the right, like a first-person
 * viewmodel) and its barrel continuously turns towards whatever the player is looking at. It pulls back towards the
 * eyes when a wall is close, so it never pokes through geometry.
 *
 * Shots go where the player is LOOKING but leave from the muzzle. Eyes and gun are in different places, so each shot
 * is solved in two steps:
 *  1. Trace from the eyes along the view to find the aim point (whatever is under the centre of vision).
 *  2. Fire from the muzzle at that aim point and use what that line hits. It passes through the aim point, so it
 *     lands exactly where the player looked unless something is in the way of the gun (cover the eyes can see over).
 * If the eye can't see the muzzle (the gun is poked through a wall), the shot hits that wall instead of coming out the
 * other side.
 *
 * Hits are instant (hitscan, point damage like the beam, so headshots and menu buttons work); a tracer streak flies
 * from the muzzle to the hit for show. Spawned by AGameFlowManager when the gun is selected; attaches itself to the
 * pawn's camera, so it also survives the pawn arriving late.
 */
UCLASS()
class CYCLOPS_API AHandGun : public AActor
{
	GENERATED_BODY()

public:
	AHandGun();

	/** Fires one shot (the trigger calls this). Ignored inside MinShotInterval. */
	UFUNCTION(BlueprintCallable, Category = "Hand Gun")
	void Fire();

	/** Where a shot fired now would hit: returns whether it hit something; OutEnd is the hit or max-range point. */
	bool TraceShot(const FVector& Eye, const FVector& LookDirection, const FVector& Muzzle, FHitResult& OutHit, FVector& OutEnd) const;

	UFUNCTION(BlueprintPure, Category = "Hand Gun")
	FVector GetMuzzleLocation() const;

	/** Every shot fired (e.g. so a versus match can show it to the other player). */
	FOnGunShot OnShot;

	/** Off (and hidden) while the player is dead in versus. */
	void SetCanFire(bool bInCanFire) { bCanFire = bInCanFire; SetActorHiddenInGame(!bInCanFire); }

	/** Starts a reload (no-op if already reloading or full). Also bound to A / X, and automatic when empty. */
	UFUNCTION(BlueprintCallable, Category = "Hand Gun")
	void Reload();

	UFUNCTION(BlueprintPure, Category = "Hand Gun")
	int32 GetAmmo() const { return Ammo; }

	UFUNCTION(BlueprintPure, Category = "Hand Gun")
	bool IsReloading() const { return ReloadRemaining >= 0.f; }

	/** While on (a menu is open), shots don't use ammo, so menu buttons don't cost a magazine. */
	void SetFreeFire(bool bInFreeFire) { bFreeFire = bInFreeFire; }

	/** Full magazine, no reload in progress (respawn, new run). */
	void Refill();

	// ---- Magazine ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Magazine", meta = (ClampMin = "1"))
	int32 MagazineSize = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Magazine", meta = (ClampMin = "0.1", Units = "s"))
	float ReloadTime = 1.5f;

	/** How far the gun dips (pitch) at the middle of a reload. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Magazine", meta = (Units = "deg"))
	float ReloadDipAngle = 28.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	FLinearColor RoundColour = FLinearColor(1.f, 0.75f, 0.25f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	FLinearColor ReloadingColour = FLinearColor(0.2f, 0.75f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	FLinearColor EmptyColour = FLinearColor(0.06f, 0.05f, 0.04f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	TObjectPtr<USoundBase> ReloadStartSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	TObjectPtr<USoundBase> ReloadDoneSound;

	/** Engine cube for the ammo pips. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Magazine")
	TObjectPtr<UStaticMesh> PipMesh;

	// ---- Placement (head-locked) ----

	/** Where the gun sits relative to the eyes: X forward, Y right, Z up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Placement", meta = (Units = "cm"))
	FVector ViewOffset = FVector(30.0, 13.0, -17.0);

	/** How quickly the barrel turns onto a new aim point (higher = snappier; it never lags behind a shot). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Placement", meta = (ClampMin = "1"))
	float AimTurnSpeed = 25.f;

	/** Gap kept between the front of the gun and a wall in front of the player; it slides back towards the eyes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Placement", meta = (ClampMin = "0", Units = "cm"))
	float WallGap = 4.f;

	/** Closest the gun comes to the eyes when pulled back from a wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Placement", meta = (ClampMin = "5", Units = "cm"))
	float MinViewDistance = 6.f;

	/**
	 * The barrel never converges on anything closer than this (the bullet still goes to the true aim point). Without it a
	 * target right in front of the face would swing the gun across the view.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Placement", meta = (ClampMin = "0", Units = "cm"))
	float MinConvergeDistance = 150.f;

	// ---- Shooting ----

	/** Enemies have 50-55 HP and walkers take 2.5x on the head: two body shots or one headshot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Shooting", meta = (ClampMin = "0"))
	float Damage = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Shooting", meta = (ClampMin = "100", Units = "cm"))
	float MaxRange = 8000.f;

	/** Fastest the trigger can fire (semi-automatic). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Shooting", meta = (ClampMin = "0", Units = "s"))
	float MinShotInterval = 0.12f;

	/** Velocity change given to a simulating physics body that's hit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Shooting", meta = (ClampMin = "0", Units = "cm/s"))
	float Knockback = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Shooting")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;

	// ---- Model ----

	/** Template pistol: barrel along +Y. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Model")
	TObjectPtr<UStaticMesh> GunMesh;

	/**
	 * Gun mesh relative to the aiming pivot (whose X axis is turned onto the aim point): turns the template pistol's
	 * +Y barrel onto X and centres the pivot on the gun, so it swivels about its middle.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Model")
	FTransform MeshOnPivot = FTransform(FRotator(0.f, -90.f, 0.f), FVector(-7.8, 0.0, -3.2));

	/** Muzzle relative to the gun mesh (template BP_Pistol's MuzzleLocation); its X axis points down the barrel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Model")
	FTransform MuzzleOnGun = FTransform(FRotator(0.f, 90.f, 0.f), FVector(0.0, 19.2, 8.3));

	// ---- Effects ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<USoundBase> FireSound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0"))
	float FireVolume = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<UHapticFeedbackEffect_Base> FireHaptic;

	/** Z-up cylinder for tracers (engine BasicShapes/Cylinder: radius 50, length 100). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<UStaticMesh> TracerMesh;

	/** Sphere for the muzzle flash (engine BasicShapes/Sphere: radius 50). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<UStaticMesh> FlashMesh;

	/** Unlit, colour from custom primitive data 0-3 (M_HudBar). Used for the tracer and the flash. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<UMaterialInterface> GlowMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	FLinearColor TracerColour = FLinearColor(1.f, 0.78f, 0.35f);

	/** Slower than a real bullet so the streak can be seen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "100", Units = "cm/s"))
	float TracerSpeed = 15000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "1", Units = "cm"))
	float TracerLength = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0.05", Units = "cm"))
	float TracerRadius = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0", Units = "s"))
	float FlashTime = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0"))
	float FlashLightIntensity = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Effects")
	TObjectPtr<UMaterialInterface> ImpactDecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0.1", Units = "cm"))
	float ImpactDecalSize = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "0", Units = "s"))
	float ImpactDecalLifeSpan = 20.f;

	/** Most bullet holes alive at once (each is a draw on mobile); the oldest goes first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand Gun|Effects", meta = (ClampMin = "1"))
	int32 MaxImpactDecals = 30;

	// ---- Input ----

	/** Template's trigger actions; already in default mapping contexts, so OpenXR binds them. Either trigger fires. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Input")
	TObjectPtr<UInputAction> FireActionRight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Input")
	TObjectPtr<UInputAction> FireActionLeft;

	/** Early reload (A / X: the optic blast's button, unused while the gun is out). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand Gun|Input")
	TObjectPtr<UInputAction> ReloadAction;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FTracer
	{
		UStaticMeshComponent* Mesh = nullptr;
		FVector Start = FVector::ZeroVector;
		FVector Direction = FVector::ForwardVector;
		double Length = 0.0;
		double Travelled = 0.0;
		bool bActive = false;
	};

	void HandleFireRight(const FInputActionValue& Value);
	void HandleFireLeft(const FInputActionValue& Value);
	void FireFrom(EControllerHand TriggerHand);
	bool TryAttachToView();
	bool TryBindInput();
	/** Head-locked placement: pull back from walls, turn the barrel onto the aim point. */
	void UpdatePose(float DeltaSeconds);
	void HandleReload(const FInputActionValue& Value);
	void UpdateReload(float DeltaSeconds);
	/** Lights the pips: rounds left, or the refill progress while reloading. */
	void UpdateAmmoPips();
	bool GetEye(FVector& OutLocation, FVector& OutDirection) const;
	FCollisionQueryParams MakeQueryParams() const;
	void SpawnTracer(const FVector& Start, const FVector& End);
	void UpdateTracers(float DeltaSeconds);
	void StampImpact(const FHitResult& Hit);
	UStaticMeshComponent* MakeGlowMesh(FName Name, UStaticMesh* Mesh, const FLinearColor& Colour);

	UPROPERTY(VisibleAnywhere, Category = "Hand Gun")
	TObjectPtr<USceneComponent> AimPivot;

	UPROPERTY(VisibleAnywhere, Category = "Hand Gun")
	TObjectPtr<UStaticMeshComponent> GunMeshComponent;

	UPROPERTY(VisibleAnywhere, Category = "Hand Gun")
	TObjectPtr<USceneComponent> MuzzlePoint;

	UPROPERTY(VisibleAnywhere, Category = "Hand Gun")
	TObjectPtr<UPointLightComponent> FlashLight;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> FlashComponent;

	/** Owns the pooled tracer meshes (FTracer holds raw pointers into this). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> TracerMeshes;

	/** Ammo indicator along the top of the slide, rear to front. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> AmmoPips;

	TArray<FTracer> Tracers;
	TArray<TWeakObjectPtr<UDecalComponent>> ActiveDecals;
	TWeakObjectPtr<APlayerController> BoundController;
	TWeakObjectPtr<USceneComponent> ViewCamera;
	float LastShotTime = -1000.f;
	float FlashRemaining = 0.f;
	int32 Ammo = 0;
	float ReloadRemaining = -1.f; // < 0 = not reloading
	int32 LastPipState = -1;      // what the pips currently show (only recolour on change)
	bool bFreeFire = false;
	float Retract = 0.f;          // how far the gun is currently pulled back towards the eyes (walls)
	bool bAttached = false;
	bool bCanFire = true;
	bool bHasAimRotation = false;
};
