#include "GameFlowManager.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnemySpawner.h"
#include "EngineUtils.h"
#include "OpticBlastComponent.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Menu faces the player along +X (text renders towards +X). +Y is the player's left, Z up.
	const float ButtonZ = -12.f;
	const FVector ButtonOffsets[] = { FVector(0.f, 0.f, ButtonZ), FVector(0.f, 72.f, ButtonZ), FVector(0.f, -72.f, ButtonZ) };
	const FVector ButtonScale(0.04f, 0.6f, 0.2f); // invisible 60 x 20 cm hit volume (100 cm cube)
	const FVector2D ButtonSize(60.f, 19.f);

	// Background panel.
	const FVector PanelCentre(-3.f, 0.f, 20.f);
	const FVector2D PanelSize(215.f, 122.f);

	// Title graphic: bat floats above the panel, zapped by a beam from the upper left.
	const FVector GraphicBatLocation(6.f, -28.f, 112.f);
	const FVector GraphicBeamStart(6.f, 100.f, 84.f);
	const float GraphicBatScale = 0.42f;
	const float GraphicBeamRadius = 1.3f;

	// Engine plane is 100x100 in XY with +Z normal; pitch -90 stands it up facing +X with local X vertical.
	const FRotator PlaneFacingPlayer(-90.f, 0.f, 0.f);

	/** Rounded-rect shape for M_MenuPanel via custom primitive data. */
	void SetPanelShape(UStaticMeshComponent* Plane, const FVector2D& Size, float Radius, const FLinearColor& Fill, const FLinearColor& Rim, float Glow)
	{
		Plane->SetRelativeScale3D(FVector(Size.Y / 100.f, Size.X / 100.f, 1.f));
		Plane->SetCustomPrimitiveDataFloat(0, Size.Y); // SizeU: along local X (vertical)
		Plane->SetCustomPrimitiveDataFloat(1, Size.X); // SizeV: along local Y (horizontal)
		Plane->SetCustomPrimitiveDataFloat(2, Radius);
		Plane->SetCustomPrimitiveDataFloat(3, Glow);
		Plane->SetCustomPrimitiveDataVector4(4, FVector4(Fill.R, Fill.G, Fill.B, Fill.A));
		Plane->SetCustomPrimitiveDataVector4(8, FVector4(Rim.R, Rim.G, Rim.B, Rim.A)); // A = rim width (cm)
	}

	UStaticMeshComponent* MakeVisual(AActor* Owner, FName Name, USceneComponent* Parent, const FVector& Location)
	{
		UStaticMeshComponent* Mesh = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Mesh->SetupAttachment(Parent);
		Mesh->SetRelativeLocation(Location);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCastShadow(false);
		return Mesh;
	}

	const FLinearColor PanelFill(0.01f, 0.014f, 0.022f, 0.94f); // high alpha: blending is in linear light, so 0.84 still reads hazy
	const FLinearColor PanelRim(0.25f, 0.38f, 0.5f, 0.7f);
	const FLinearColor ButtonFill(0.015f, 0.16f, 0.22f, 0.6f);
	const FLinearColor ButtonRim(0.3f, 0.85f, 1.f, 0.55f);

	template <typename T>
	T* FindAsset(const TCHAR* Path)
	{
		ConstructorHelpers::FObjectFinder<T> Finder(Path);
		return Finder.Object;
	}

	UTextRenderComponent* MakeText(AActor* Owner, FName Name, USceneComponent* Parent, const FVector& Location, float Size, FColor Colour)
	{
		UTextRenderComponent* Text = Owner->CreateDefaultSubobject<UTextRenderComponent>(Name);
		Text->SetupAttachment(Parent);
		Text->SetRelativeLocation(Location);
		Text->SetHorizontalAlignment(EHTA_Center);
		Text->SetVerticalAlignment(EVRTA_TextCenter);
		Text->SetWorldSize(Size);
		Text->SetTextRenderColor(Colour);
		Text->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Text->SetCastShadow(false);
		return Text;
	}

	void SetButtonCollision(UStaticMeshComponent* Panel, bool bEnabled)
	{
		Panel->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	}
}

AGameFlowManager::AGameFlowManager()
{
	PrimaryActorTick.bCanEverTick = true;

	MenuRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MenuRoot"));
	RootComponent = MenuRoot;

	// Defaults so a freshly placed manager is complete; all overridable per instance.
	ButtonMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Cube.Cube"));
	PanelMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Plane.Plane"));
	PanelMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/M_MenuPanel.M_MenuPanel"));
	GraphicSphereMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	GraphicCubeMesh = ButtonMesh;
	GraphicCylinderMesh = FindAsset<UStaticMesh>(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	BatBodyMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_EnemyBody.MI_EnemyBody"));
	BatWingMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_EnemyWing.MI_EnemyWing"));
	BatEyeMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_EnemyEyes.MI_EnemyEyes"));
	GraphicBeamMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/M_OpticBeam.M_OpticBeam"));
	GraphicBeamGlowMaterial = FindAsset<UMaterialInterface>(TEXT("/Game/Cyclops/Materials/MI_OpticBeamGlow.MI_OpticBeamGlow"));

	BackgroundPanel = MakeVisual(this, TEXT("BackgroundPanel"), MenuRoot, PanelCentre);
	BackgroundPanel->SetRelativeRotation(PlaneFacingPlayer);

	TitleText = MakeText(this, TEXT("TitleText"), MenuRoot, FVector(0.f, 0.f, 56.f), 26.f, FColor(255, 90, 60));
	SubtitleText = MakeText(this, TEXT("SubtitleText"), MenuRoot, FVector(0.f, 0.f, 30.f), 7.f, FColor(200, 210, 220));

	for (int32 i = 0; i < UE_ARRAY_COUNT(ButtonOffsets); ++i)
	{
		// Invisible hit volume (a real box, so the beam's trace reliably hits it).
		UStaticMeshComponent* Panel = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("ButtonPanel%d"), i));
		Panel->SetupAttachment(MenuRoot);
		Panel->SetRelativeLocation(ButtonOffsets[i]);
		Panel->SetRelativeScale3D(ButtonScale);
		Panel->SetCollisionObjectType(ECC_WorldDynamic);
		Panel->SetCollisionResponseToAllChannels(ECR_Ignore);
		Panel->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block); // the beam's trace channel
		Panel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Panel->SetGenerateOverlapEvents(false);
		Panel->SetCastShadow(false);
		Panel->SetVisibility(false);
		ButtonPanels.Add(Panel);

		UStaticMeshComponent* Visual = MakeVisual(this, *FString::Printf(TEXT("ButtonVisual%d"), i), MenuRoot, ButtonOffsets[i] + FVector(1.f, 0.f, 0.f));
		Visual->SetRelativeRotation(PlaneFacingPlayer);
		// Always draw over the background panel (otherwise distance sorting puts the side pills behind it).
		Visual->SetTranslucentSortPriority(1);
		ButtonVisuals.Add(Visual);

		ButtonLabels.Add(MakeText(this, *FString::Printf(TEXT("ButtonLabel%d"), i), MenuRoot,
			ButtonOffsets[i] + FVector(3.f, 0.f, 0.f), 7.5f, FColor::White));
	}

	// Title graphic: same proportions as AFlyingEnemy, scaled down.
	GraphicBat = CreateDefaultSubobject<USceneComponent>(TEXT("GraphicBat"));
	GraphicBat->SetupAttachment(MenuRoot);
	GraphicBat->SetRelativeLocation(GraphicBatLocation);
	GraphicBat->SetRelativeRotation(FRotator(10.f, -25.f, 15.f)); // knocked askew by the hit
	GraphicBat->SetRelativeScale3D(FVector(GraphicBatScale));

	GraphicBatBody = MakeVisual(this, TEXT("GraphicBatBody"), GraphicBat, FVector::ZeroVector);
	GraphicBatBody->SetRelativeScale3D(FVector(0.45f, 0.35f, 0.3f));
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Sign = Side == 0 ? -1.f : 1.f;
		UStaticMeshComponent* Eye = MakeVisual(this, *FString::Printf(TEXT("GraphicBatEye%d"), Side), GraphicBat, FVector(19.f, 7.f * Sign, 5.f));
		Eye->SetRelativeScale3D(FVector(0.07f));
		GraphicBatEyes.Add(Eye);

		USceneComponent* Pivot = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("GraphicWingPivot%d"), Side));
		Pivot->SetupAttachment(GraphicBat);
		Pivot->SetRelativeLocation(FVector(0.f, 12.f * Sign, 4.f));
		GraphicWingPivots.Add(Pivot);

		UStaticMeshComponent* Wing = MakeVisual(this, *FString::Printf(TEXT("GraphicWing%d"), Side), Pivot, FVector(-4.f, 24.f * Sign, 0.f));
		Wing->SetRelativeScale3D(FVector(0.3f, 0.45f, 0.02f));
		GraphicWings.Add(Wing);
	}

	GraphicBeam = MakeVisual(this, TEXT("GraphicBeam"), MenuRoot, FVector::ZeroVector);
	GraphicBeamGlow = MakeVisual(this, TEXT("GraphicBeamGlow"), MenuRoot, FVector::ZeroVector);
	GraphicImpact = MakeVisual(this, TEXT("GraphicImpact"), MenuRoot, FVector::ZeroVector);
}

void AGameFlowManager::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyMeshesAndMaterials();
}

void AGameFlowManager::ApplyMeshesAndMaterials()
{
	for (UStaticMeshComponent* Panel : ButtonPanels)
	{
		Panel->SetStaticMesh(ButtonMesh);
		Panel->SetMaterial(0, ButtonMaterial);
	}

	BackgroundPanel->SetStaticMesh(PanelMesh);
	BackgroundPanel->SetMaterial(0, PanelMaterial);
	SetPanelShape(BackgroundPanel, PanelSize, 12.f, PanelFill, PanelRim, 0.f);

	for (UStaticMeshComponent* Visual : ButtonVisuals)
	{
		Visual->SetStaticMesh(PanelMesh);
		Visual->SetMaterial(0, PanelMaterial);
		SetPanelShape(Visual, ButtonSize, ButtonSize.Y * 0.5f, ButtonFill, ButtonRim, 0.f);
	}

	GraphicBatBody->SetStaticMesh(GraphicSphereMesh);
	GraphicBatBody->SetMaterial(0, BatBodyMaterial);
	for (UStaticMeshComponent* Eye : GraphicBatEyes)
	{
		Eye->SetStaticMesh(GraphicSphereMesh);
		Eye->SetMaterial(0, BatEyeMaterial);
	}
	for (UStaticMeshComponent* Wing : GraphicWings)
	{
		Wing->SetStaticMesh(GraphicCubeMesh);
		Wing->SetMaterial(0, BatWingMaterial);
	}

	// Beam from the upper-left corner to just short of the bat (cylinder: Z-up, 100 cm long, 50 cm radius).
	const FVector HitPoint = GraphicBatLocation + (GraphicBeamStart - GraphicBatLocation).GetSafeNormal() * 6.f;
	const FVector BeamVector = HitPoint - GraphicBeamStart;
	const FRotator BeamRotation = FRotationMatrix::MakeFromZ(BeamVector).Rotator();
	const FVector BeamCentre = GraphicBeamStart + BeamVector * 0.5f;
	const float Length = BeamVector.Size() / 100.f;
	const float Radius = GraphicBeamRadius / 50.f;

	GraphicBeam->SetStaticMesh(GraphicCylinderMesh);
	GraphicBeam->SetMaterial(0, GraphicBeamMaterial);
	GraphicBeam->SetRelativeLocationAndRotation(BeamCentre, BeamRotation);
	GraphicBeam->SetRelativeScale3D(FVector(Radius, Radius, Length));

	GraphicBeamGlow->SetStaticMesh(GraphicCylinderMesh);
	GraphicBeamGlow->SetMaterial(0, GraphicBeamGlowMaterial);
	GraphicBeamGlow->SetRelativeLocationAndRotation(BeamCentre, BeamRotation);
	GraphicBeamGlow->SetRelativeScale3D(FVector(Radius * 3.f, Radius * 3.f, Length));

	GraphicImpact->SetStaticMesh(GraphicSphereMesh);
	GraphicImpact->SetMaterial(0, BatEyeMaterial);
	GraphicImpact->SetRelativeLocation(HitPoint);
	GraphicImpact->SetRelativeScale3D(FVector(0.05f));
}

void AGameFlowManager::AnimateGraphic(float Time)
{
	// Wings flap, the bat shudders and flashes, the beam and impact flicker.
	const float Flap = 35.f * FMath::Sin(Time * 2.f * PI * 6.f);
	if (GraphicWingPivots.Num() == 2)
	{
		GraphicWingPivots[0]->SetRelativeRotation(FRotator(0.f, 0.f, -Flap));
		GraphicWingPivots[1]->SetRelativeRotation(FRotator(0.f, 0.f, Flap));
	}

	const FVector Shudder(0.f, 1.2f * FMath::Sin(Time * 37.f), 1.f * FMath::Sin(Time * 43.f) + 2.f * FMath::Sin(Time * 1.7f));
	GraphicBat->SetRelativeLocation(GraphicBatLocation + Shudder);
	GraphicBatBody->SetCustomPrimitiveDataFloat(0, 0.45f + 0.45f * FMath::Sin(Time * 26.f));

	const float Pulse = 1.1f + 0.3f * FMath::Sin(Time * 31.f) + 0.15f * FMath::Sin(Time * 13.f);
	GraphicBeam->SetCustomPrimitiveDataFloat(0, Pulse);
	GraphicBeamGlow->SetCustomPrimitiveDataFloat(0, Pulse);
	GraphicImpact->SetRelativeScale3D(FVector(0.05f * (1.f + 0.35f * FMath::Sin(Time * 29.f))));
}

void AGameFlowManager::BeginPlay()
{
	Super::BeginPlay();

	// Custom primitive data set during construction is runtime-only and gets dropped, so re-apply here.
	ApplyMeshesAndMaterials();

	Slots.SetNum(ButtonPanels.Num());
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		Slots[i].Panel = ButtonPanels[i];
		Slots[i].Label = ButtonLabels[i];
		Slots[i].Visual = ButtonVisuals[i];
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PauseAction)
	{
		EnableInput(PC);
		if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent))
		{
			Input->BindAction(PauseAction, ETriggerEvent::Triggered, this, &AGameFlowManager::HandlePauseInput);
		}
	}

	HideMenu();
	// Give the HMD a moment to report a real pose so the title appears in front of the player.
	FTimerHandle Handle;
	GetWorldTimerManager().SetTimer(Handle, this, &AGameFlowManager::ShowTitle, 0.5f, false);
}

void AGameFlowManager::HandlePauseInput(const FInputActionValue& Value)
{
	switch (State)
	{
	case EGameFlowState::Title:   StartGame(); break;
	case EGameFlowState::Playing: PauseGame(); break;
	case EGameFlowState::Paused:  ResumeGame(); break;
	}
}

void AGameFlowManager::ShowTitle()
{
	State = EGameFlowState::Title;
	if (Spawner)
	{
		Spawner->ResetSpawner();
	}
	SetBeamsPaused(false, /*bRefill*/ true);
	ConfigureButton(0, EMenuButton::Start, TEXT("START"));
	ConfigureButton(1, EMenuButton::None, FString());
	ConfigureButton(2, EMenuButton::None, FString());
	ShowMenu(TEXT("CYCLOPS"), TEXT("Blast START to begin      Menu button pauses"));
}

void AGameFlowManager::StartGame()
{
	HideMenu();
	// Restarting from the pause menu: unfreeze and start with a full charge.
	SetBeamsPaused(false, /*bRefill*/ State == EGameFlowState::Paused);
	State = EGameFlowState::Playing;
	if (Spawner)
	{
		Spawner->ResetSpawner();
		Spawner->StartSpawning();
	}
}

void AGameFlowManager::PauseGame()
{
	if (State != EGameFlowState::Playing)
	{
		return;
	}
	State = EGameFlowState::Paused;
	if (Spawner)
	{
		Spawner->SetGamePaused(true);
	}
	SetBeamsPaused(true, /*bRefill*/ false);
	PlayUISound(PauseSound);

	ConfigureButton(0, EMenuButton::Resume, TEXT("RESUME"));
	ConfigureButton(1, EMenuButton::Restart, TEXT("RESTART"));
	ConfigureButton(2, EMenuButton::MainMenu, TEXT("MAIN MENU"));
	const FString Score = Spawner
		? FString::Printf(TEXT("Kills %d    Hits %d"), Spawner->GetKills(), Spawner->GetHitsTaken())
		: FString();
	ShowMenu(TEXT("PAUSED"), Score);
}

void AGameFlowManager::ResumeGame()
{
	if (State != EGameFlowState::Paused)
	{
		return;
	}
	HideMenu();
	State = EGameFlowState::Playing;
	if (Spawner)
	{
		Spawner->SetGamePaused(false);
	}
	SetBeamsPaused(false, /*bRefill*/ false);
	PlayUISound(PauseSound);
}

void AGameFlowManager::SetBeamsPaused(bool bPaused, bool bRefill) const
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (UOpticBlastComponent* Beam = It->FindComponentByClass<UOpticBlastComponent>())
		{
			Beam->SetEnergyFrozen(bPaused);
			if (bRefill)
			{
				Beam->ResetEnergy();
			}
		}
	}
}

void AGameFlowManager::ConfigureButton(int32 Index, EMenuButton Action, const FString& Text)
{
	if (!Slots.IsValidIndex(Index))
	{
		return;
	}
	FButtonSlot& Slot = Slots[Index];
	Slot.Action = Action;
	Slot.Charge = 0.f;
	const bool bUsed = Action != EMenuButton::None;
	Slot.Visual->SetVisibility(bUsed);
	Slot.Label->SetVisibility(bUsed);
	Slot.Label->SetText(FText::FromString(Text));
	SetButtonCollision(Slot.Panel, false); // enabled in ShowMenu
}

void AGameFlowManager::ShowMenu(const FString& Title, const FString& Subtitle)
{
	PlaceInFrontOfPlayer();
	TitleText->SetText(FText::FromString(Title));
	SubtitleText->SetText(FText::FromString(Subtitle));
	SetActorHiddenInGame(false);
	for (const FButtonSlot& Slot : Slots)
	{
		SetButtonCollision(Slot.Panel, Slot.Action != EMenuButton::None);
	}
	bMenuVisible = true;
	MenuOpenTime = GetWorld()->GetTimeSeconds();
	if (Spawner)
	{
		Spawner->SetScoreboardVisible(false);
	}
}

void AGameFlowManager::HideMenu()
{
	SetActorHiddenInGame(true);
	for (const FButtonSlot& Slot : Slots)
	{
		SetButtonCollision(Slot.Panel, false);
	}
	bMenuVisible = false;
	if (Spawner)
	{
		Spawner->SetScoreboardVisible(true);
	}
}

void AGameFlowManager::PlaceInFrontOfPlayer()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return;
	}
	const FVector Eye = Camera->GetCameraLocation();
	const float Yaw = Camera->GetCameraRotation().Yaw;
	FVector Location = Eye + FRotator(0.f, Yaw, 0.f).Vector() * MenuDistance;
	Location.Z = Eye.Z + MenuHeightOffset;
	SetActorLocationAndRotation(Location, FRotator(0.f, Yaw + 180.f, 0.f));
}

AGameFlowManager::FButtonSlot* AGameFlowManager::FindSlot(const UPrimitiveComponent* Component)
{
	return Slots.FindByPredicate([Component](const FButtonSlot& Slot) { return Slot.Panel == Component; });
}

float AGameFlowManager::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// Ignore the first moment after opening, so a beam that was already firing doesn't press anything.
	const float Now = GetWorld()->GetTimeSeconds();
	if (!bMenuVisible || Now - MenuOpenTime < 0.4f || !DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		return 0.f;
	}

	const FHitResult& Hit = static_cast<const FPointDamageEvent&>(DamageEvent).HitInfo;
	FButtonSlot* Slot = FindSlot(Hit.GetComponent());
	if (!Slot || Slot->Action == EMenuButton::None)
	{
		return 0.f;
	}

	Slot->Charge += DamageAmount;
	Slot->LastHitTime = Now;
	if (Slot->Charge >= ChargeRequired)
	{
		PressButton(Slot->Action);
	}
	return DamageAmount;
}

void AGameFlowManager::PressButton(EMenuButton Action)
{
	for (FButtonSlot& Slot : Slots)
	{
		Slot.Charge = 0.f;
	}
	PlayUISound(ConfirmSound);

	switch (Action)
	{
	case EMenuButton::Start:
	case EMenuButton::Restart:
		if (Spawner && State == EGameFlowState::Paused)
		{
			Spawner->SetGamePaused(false);
		}
		StartGame();
		break;
	case EMenuButton::Resume:   ResumeGame(); break;
	case EMenuButton::MainMenu: ShowTitle(); break;
	default: break;
	}
}

void AGameFlowManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bMenuVisible)
	{
		return;
	}

	const float Now = GetWorld()->GetTimeSeconds();
	AnimateGraphic(Now);

	// Charge feedback: buttons glow and swell while the beam is on them, and drain when it leaves.
	for (FButtonSlot& Slot : Slots)
	{
		if (Now - Slot.LastHitTime > 0.1f)
		{
			Slot.Charge = FMath::Max(0.f, Slot.Charge - ChargeDecayPerSecond * DeltaSeconds);
		}
		const float Fraction = FMath::Clamp(Slot.Charge / ChargeRequired, 0.f, 1.f);
		const float Swell = 1.f + 0.12f * Fraction;
		Slot.Visual->SetCustomPrimitiveDataFloat(3, Fraction); // Glow
		Slot.Visual->SetRelativeScale3D(FVector(ButtonSize.Y * Swell / 100.f, ButtonSize.X * Swell / 100.f, 1.f));
		Slot.Panel->SetRelativeScale3D(ButtonScale * Swell);
	}
}

void AGameFlowManager::PlayUISound(USoundBase* Sound) const
{
	if (Sound)
	{
		UGameplayStatics::PlaySound2D(this, Sound);
	}
}
