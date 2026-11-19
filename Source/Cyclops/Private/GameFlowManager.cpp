#include "GameFlowManager.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "CyclopsSaveGame.h"
#include "Engine/DamageEvents.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnemySpawner.h"
#include "EngineUtils.h"
#include "OpticBlastComponent.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "LeaderboardSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Menu faces the player along +X (text renders towards +X). +Y is the player's left, Z up.
	constexpr int32 MaxButtons = 8;
	const float ButtonZ = -12.f;
	const FVector2D ButtonSize(60.f, 19.f);
	const float ButtonDepth = 0.04f; // hit volume thickness (x 100 cm cube)

	// Standard three-button row: centre, left, right.
	const FVector2D RowCentre(0.f, ButtonZ);
	const FVector2D RowLeft(72.f, ButtonZ);
	const FVector2D RowRight(-72.f, ButtonZ);

	// Background panel.
	const FVector PanelCentre(-3.f, 0.f, 20.f);
	const FVector2D PanelSize(215.f, 122.f);

	// Initials editor: three letters with + above and - below, OK underneath.
	const float LetterSpacing = 34.f;
	const float LetterZ = 12.f;
	const FVector2D LetterButtonSize(24.f, 13.f);
	const float LetterUpZ = 34.f;
	const float LetterDownZ = -10.f;
	const FVector2D OkPosition(0.f, -29.f);
	const FVector2D OkSize(50.f, 15.f);
	const TCHAR* InitialsCharset = TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

	// Leaderboard panel: hinged off the main panel's right edge and turned in towards the player.
	const FVector LeaderboardLocation(21.f, -153.f, 20.f);
	const float LeaderboardYaw = 30.f;
	const FVector2D LeaderboardSize(95.f, 122.f);
	const float LeaderboardRowSize = 7.f;

	// Menu placement around walls (PlaceInFrontOfPlayer).
	const float MenuCandidateYaws[] = { 0.f, -30.f, 30.f, -60.f, 60.f, -90.f, 90.f, -135.f, 135.f, 180.f };
	const FVector MenuFootprintHalfExtent(15.f, 180.f, 70.f); // main panel + the angled leaderboard panel, with margin
	constexpr float MenuClearance = 40.f;  // free space wanted beyond the menu
	constexpr float MenuSightRadius = 15.f;
	constexpr float MinMenuDistance = 120.f;

	/** URL option set when arriving via the title's level switch: skip the title and start. */
	const TCHAR* StartOption = TEXT("start");

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

	FString JoinLines(const TArray<FString>& Lines)
	{
		return FString::Join(Lines, TEXT("\n"));
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

	// Button slots; each screen positions and sizes the ones it uses (ConfigureButton).
	for (int32 i = 0; i < MaxButtons; ++i)
	{
		// Invisible hit volume (a real box, so the beam's trace reliably hits it).
		UStaticMeshComponent* Panel = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("ButtonPanel%d"), i));
		Panel->SetupAttachment(MenuRoot);
		Panel->SetCollisionObjectType(ECC_WorldDynamic);
		Panel->SetCollisionResponseToAllChannels(ECR_Ignore);
		Panel->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block); // the beam's trace channel
		Panel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Panel->SetGenerateOverlapEvents(false);
		Panel->SetCastShadow(false);
		Panel->SetVisibility(false);
		ButtonPanels.Add(Panel);

		UStaticMeshComponent* Visual = MakeVisual(this, *FString::Printf(TEXT("ButtonVisual%d"), i), MenuRoot, FVector::ZeroVector);
		Visual->SetRelativeRotation(PlaneFacingPlayer);
		// Always draw over the background panel (otherwise distance sorting puts the side pills behind it).
		Visual->SetTranslucentSortPriority(1);
		ButtonVisuals.Add(Visual);

		ButtonLabels.Add(MakeText(this, *FString::Printf(TEXT("ButtonLabel%d"), i), MenuRoot, FVector::ZeroVector, 7.5f, FColor::White));
	}

	for (int32 i = 0; i < 3; ++i)
	{
		InitialLetters.Add(MakeText(this, *FString::Printf(TEXT("InitialLetter%d"), i), MenuRoot,
			FVector(3.f, LetterSpacing * (1 - i), LetterZ), 22.f, FColor(255, 200, 120)));
	}

	// Leaderboard side panel.
	LeaderboardRoot = CreateDefaultSubobject<USceneComponent>(TEXT("LeaderboardRoot"));
	LeaderboardRoot->SetupAttachment(MenuRoot);
	LeaderboardRoot->SetRelativeLocationAndRotation(LeaderboardLocation, FRotator(0.f, LeaderboardYaw, 0.f));

	LeaderboardPanel = MakeVisual(this, TEXT("LeaderboardPanel"), LeaderboardRoot, FVector::ZeroVector);
	LeaderboardPanel->SetRelativeRotation(PlaneFacingPlayer);

	LeaderboardTitle = MakeText(this, TEXT("LeaderboardTitle"), LeaderboardRoot, FVector(3.f, 0.f, 49.f), 8.f, FColor(255, 90, 60));
	LeaderboardTitle->SetText(FText::FromString(TEXT("TOP 10")));

	// Columns: rank and name left-aligned (text runs towards -Y), score right-aligned at the far edge.
	const float ColumnY[] = { 40.f, 24.f, -40.f };
	const EHorizTextAligment ColumnAlign[] = { EHTA_Left, EHTA_Left, EHTA_Right };
	for (int32 i = 0; i < 3; ++i)
	{
		UTextRenderComponent* Column = MakeText(this, *FString::Printf(TEXT("LeaderboardColumn%d"), i), LeaderboardRoot,
			FVector(3.f, ColumnY[i], 38.f), LeaderboardRowSize, i == 1 ? FColor::White : FColor(200, 210, 220));
		Column->SetHorizontalAlignment(ColumnAlign[i]);
		Column->SetVerticalAlignment(EVRTA_TextTop);
		LeaderboardColumns.Add(Column);
	}
	LeaderboardColumns.Add(MakeText(this, TEXT("LeaderboardMessage"), LeaderboardRoot, FVector(3.f, 0.f, 5.f), LeaderboardRowSize, FColor(200, 210, 220)));

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

	LeaderboardPanel->SetStaticMesh(PanelMesh);
	LeaderboardPanel->SetMaterial(0, PanelMaterial);
	SetPanelShape(LeaderboardPanel, LeaderboardSize, 12.f, PanelFill, PanelRim, 0.f);

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

	if (Spawner)
	{
		Spawner->OnRunEnded.AddUObject(this, &AGameFlowManager::HandleRunEnded);
	}

	LeaderboardTitle->SetText(FText::FromString(GetModeName() + TEXT(" TOP 10")));
	SetInitialsVisible(false);
	SetLeaderboardVisible(false);
	HideMenu();
	// Give the HMD a moment to report a real pose so the title appears in front of the player.
	// Arriving from the other level's title (level switch): go straight into a run.
	FTimerHandle Handle;
	const bool bStartImmediately = GetWorld()->URL.HasOption(StartOption);
	GetWorldTimerManager().SetTimer(Handle, this, bStartImmediately ? &AGameFlowManager::StartGame : &AGameFlowManager::ShowTitle, 0.5f, false);
}

void AGameFlowManager::HandlePauseInput(const FInputActionValue& Value)
{
	switch (State)
	{
	case EGameFlowState::Title:
		if (!bEditingInitials)
		{
			StartGame();
		}
		break;
	case EGameFlowState::Playing: PauseGame(); break;
	case EGameFlowState::Paused:  ResumeGame(); break;
	default: break; // game over: choose from the menu
	}
}

void AGameFlowManager::ShowTitle()
{
	State = EGameFlowState::Title;
	bEditingInitials = false;
	if (Spawner)
	{
		Spawner->ResetSpawner();
	}
	SetBeamsPaused(false, /*bRefill*/ true);

	const UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
	ClearButtons();
	ConfigureButton(0, EMenuButton::Start, TEXT("START"), RowCentre, ButtonSize);
	ConfigureButton(1, EMenuButton::EditName, Save->Initials.IsEmpty() ? FString(TEXT("SET NAME")) : TEXT("NAME ") + Save->Initials, RowRight, ButtonSize);

	if (!OtherLevel.IsNull() && !OtherLevelLabel.IsEmpty())
	{
		ConfigureButton(2, EMenuButton::SwitchLevel, OtherLevelLabel, RowLeft, ButtonSize);
	}

	FString Subtitle = GetModeName() + TEXT("      Blast START to begin      Menu button pauses");
	if (const int32 Best = Save->GetBest(GetMode()); Best > 0)
	{
		Subtitle += FString::Printf(TEXT("\nYour best: %d"), Best);
	}
	SetInitialsVisible(false);
	SetLeaderboardVisible(true);
	ShowMenu(TEXT("CYCLOPS"), Subtitle);
	FetchLeaderboard();
}

void AGameFlowManager::StartGame()
{
	HideMenu();
	SetInitialsVisible(false);
	SetLeaderboardVisible(false);
	bEditingInitials = false;
	// Restarting from the pause or game-over menu: unfreeze and start with a full charge.
	SetBeamsPaused(false, /*bRefill*/ State == EGameFlowState::Paused || State == EGameFlowState::GameOver);
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

	ClearButtons();
	ConfigureButton(0, EMenuButton::Resume, TEXT("RESUME"), RowCentre, ButtonSize);
	ConfigureButton(1, EMenuButton::Restart, TEXT("RESTART"), RowLeft, ButtonSize);
	ConfigureButton(2, EMenuButton::MainMenu, TEXT("MAIN MENU"), RowRight, ButtonSize);
	const FString Score = Spawner
		? FString::Printf(TEXT("Score %d    Lives %d"), Spawner->GetScore(), Spawner->GetLives())
		: FString();
	SetInitialsVisible(false);
	SetLeaderboardVisible(false);
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

// ---- Game over ----

void AGameFlowManager::HandleRunEnded(const FRunResult& Result)
{
	State = EGameFlowState::GameOver;
	LastScore = Result.Score;
	LastKills = Result.Kills;
	LastDuration = Result.Duration;
	LastRank = 0;
	bLastSubmitted = false;
	bSubmitting = false;
	bLastCompleted = Result.bCompleted;
	LastFinishBonus = Result.FinishBonus;

	UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
	bLastWasBest = LastScore > Save->GetBest(GetMode());
	if (bLastWasBest)
	{
		Save->SetBest(GetMode(), LastScore);
		Save->Save();
	}

	// Frozen at full so menu taps and holds always work.
	SetBeamsPaused(true, /*bRefill*/ true);
	PlayUISound(PauseSound);

	if (Save->Initials.IsEmpty())
	{
		// First game on this headset: ask for initials, then submit.
		ShowInitialsEditor(/*bReposition*/ true);
	}
	else
	{
		SubmitLastRun();
		ShowGameOver(/*bReposition*/ true);
	}
}

void AGameFlowManager::ShowGameOver(bool bReposition)
{
	bEditingInitials = false;
	const UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();

	ClearButtons();
	ConfigureButton(0, EMenuButton::PlayAgain, TEXT("PLAY AGAIN"), RowCentre, ButtonSize);
	ConfigureButton(1, EMenuButton::MainMenu, TEXT("MAIN MENU"), RowLeft, ButtonSize);
	ConfigureButton(2, EMenuButton::EditName, TEXT("NAME ") + Save->Initials, RowRight, ButtonSize);

	SetInitialsVisible(false);
	SetLeaderboardVisible(true);
	ShowMenu(bLastCompleted ? TEXT("ESCAPED!") : TEXT("GAME OVER"), FString(), bReposition);
	RefreshGameOverText();
}

void AGameFlowManager::RefreshGameOverText()
{
	if (State != EGameFlowState::GameOver || bEditingInitials)
	{
		return;
	}
	const UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
	FString Text = bLastWasBest
		? FString::Printf(TEXT("NEW BEST!    Score %d"), LastScore)
		: FString::Printf(TEXT("Score %d      Best %d"), LastScore, Save->GetBest(GetMode()));
	if (bLastCompleted)
	{
		Text += FString::Printf(TEXT("    (exit bonus %d)"), LastFinishBonus);
	}

	const ULeaderboardSubsystem* Leaderboard = GetLeaderboard();
	if (Leaderboard && Leaderboard->IsConfigured())
	{
		if (bSubmitting)
		{
			Text += TEXT("\nSending score...");
		}
		else if (LastRank > 0)
		{
			Text += FString::Printf(TEXT("\nLeaderboard rank #%d"), LastRank);
		}
		else if (bLastSubmitted)
		{
			Text += bLastSubmitOnline ? TEXT("\nScore not accepted by server") : TEXT("\nOffline - score will be sent later");
		}
	}
	SubtitleText->SetText(FText::FromString(Text));
}

void AGameFlowManager::ShowInitialsEditor(bool bReposition)
{
	bEditingInitials = true;
	const UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
	EditInitials = Save->Initials.Len() == 3 ? Save->Initials : FString(TEXT("AAA"));

	// Energy frozen at full while picking letters (also when opened from the title screen).
	SetBeamsPaused(true, /*bRefill*/ true);

	ClearButtons();
	for (int32 i = 0; i < 3; ++i)
	{
		const float Y = LetterSpacing * (1 - i);
		ConfigureButton(i, EMenuButton::LetterUp, TEXT("+"), FVector2D(Y, LetterUpZ), LetterButtonSize, 10.f, i);
		ConfigureButton(3 + i, EMenuButton::LetterDown, TEXT("-"), FVector2D(Y, LetterDownZ), LetterButtonSize, 10.f, i);
	}
	ConfigureButton(6, EMenuButton::NameDone, TEXT("OK"), OkPosition, OkSize);

	RefreshInitialsLetters();
	SetInitialsVisible(true);
	ShowMenu(TEXT("INITIALS"), FString(), bReposition);
}

void AGameFlowManager::RefreshInitialsLetters()
{
	for (int32 i = 0; i < InitialLetters.Num(); ++i)
	{
		InitialLetters[i]->SetText(FText::FromString(EditInitials.Mid(i, 1)));
	}
}

void AGameFlowManager::SubmitLastRun()
{
	ULeaderboardSubsystem* Leaderboard = GetLeaderboard();
	const UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
	if (!Leaderboard || Save->Initials.IsEmpty())
	{
		return;
	}
	if (LastScore <= 0)
	{
		FetchLeaderboard(); // nothing worth posting; just show the table
		return;
	}
	if (!Leaderboard->IsConfigured())
	{
		SetLeaderboardMessage(TEXT("Leaderboard\nnot set up"));
		return;
	}
	bSubmitting = true;
	SetLeaderboardMessage(TEXT("Loading..."));

	const int32 RequestId = ++LeaderboardRequestId;
	Leaderboard->SubmitScore(GetMode(), Save->Initials, LastScore, LastKills, LastDuration,
		FOnLeaderboardResult::CreateWeakLambda(this, [this, RequestId](const FLeaderboardResult& Result)
		{
			bSubmitting = false;
			bLastSubmitted = true;
			bLastSubmitOnline = Result.bOnline;
			LastRank = Result.Rank;
			if (RequestId == LeaderboardRequestId)
			{
				ApplyLeaderboard(Result, /*bFromSubmit*/ true);
			}
			RefreshGameOverText();
		}));
	RefreshGameOverText();
}

void AGameFlowManager::FetchLeaderboard()
{
	ULeaderboardSubsystem* Leaderboard = GetLeaderboard();
	if (!Leaderboard || !Leaderboard->IsConfigured())
	{
		SetLeaderboardMessage(TEXT("Leaderboard\nnot set up"));
		return;
	}
	SetLeaderboardMessage(TEXT("Loading..."));
	const int32 RequestId = ++LeaderboardRequestId;
	Leaderboard->FetchTop(GetMode(), FOnLeaderboardResult::CreateWeakLambda(this, [this, RequestId](const FLeaderboardResult& Result)
	{
		if (RequestId == LeaderboardRequestId)
		{
			ApplyLeaderboard(Result, /*bFromSubmit*/ false);
		}
	}));
}

void AGameFlowManager::ApplyLeaderboard(const FLeaderboardResult& Result, bool bFromSubmit)
{
	if (!Result.bOnline)
	{
		SetLeaderboardMessage(TEXT("Leaderboard\noffline"));
		return;
	}
	if (Result.Top.IsEmpty())
	{
		if (bFromSubmit)
		{
			FetchLeaderboard(); // submit was rejected, so it carried no table: ask for it
		}
		else
		{
			SetLeaderboardMessage(TEXT("No scores yet"));
		}
		return;
	}

	const FString& Me = UCyclopsSaveGame::LoadOrCreate()->Initials;
	TArray<FString> Ranks, Names, Scores;
	for (const FLeaderboardEntry& Entry : Result.Top)
	{
		Ranks.Add((Entry.Name == Me ? TEXT(">") : TEXT("")) + FString::FromInt(Entry.Rank));
		Names.Add(Entry.Name);
		Scores.Add(FString::FromInt(Entry.Score));
	}
	LeaderboardColumns[0]->SetText(FText::FromString(JoinLines(Ranks)));
	LeaderboardColumns[1]->SetText(FText::FromString(JoinLines(Names)));
	LeaderboardColumns[2]->SetText(FText::FromString(JoinLines(Scores)));
	LeaderboardColumns[3]->SetText(FText::GetEmpty());
}

void AGameFlowManager::SetLeaderboardMessage(const FString& Message)
{
	for (int32 i = 0; i < 3; ++i)
	{
		LeaderboardColumns[i]->SetText(FText::GetEmpty());
	}
	LeaderboardColumns[3]->SetText(FText::FromString(Message));
}

void AGameFlowManager::SetLeaderboardVisible(bool bVisible)
{
	LeaderboardRoot->SetVisibility(bVisible, /*bPropagateToChildren*/ true);
}

void AGameFlowManager::SetInitialsVisible(bool bVisible)
{
	for (UTextRenderComponent* Letter : InitialLetters)
	{
		Letter->SetVisibility(bVisible);
	}
}

FString AGameFlowManager::GetMode() const
{
	return Spawner && !Spawner->LeaderboardMode.IsEmpty() ? Spawner->LeaderboardMode : FString(TEXT("arena"));
}

FString AGameFlowManager::GetModeName() const
{
	return Spawner && !Spawner->ModeName.IsEmpty() ? Spawner->ModeName : FString(TEXT("ARENA"));
}

ULeaderboardSubsystem* AGameFlowManager::GetLeaderboard() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<ULeaderboardSubsystem>() : nullptr;
}

// ---- Menu plumbing ----

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

void AGameFlowManager::ClearButtons()
{
	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		ConfigureButton(i, EMenuButton::None, FString(), FVector2D::ZeroVector, ButtonSize);
	}
}

void AGameFlowManager::ConfigureButton(int32 Index, EMenuButton Action, const FString& Text, const FVector2D& Position, const FVector2D& Size, float TextSize, int32 Param)
{
	if (!Slots.IsValidIndex(Index))
	{
		return;
	}
	FButtonSlot& Slot = Slots[Index];
	Slot.Action = Action;
	Slot.Param = Param;
	Slot.Size = Size;
	Slot.Charge = 0.f;
	Slot.LastHitTime = -1.f;

	const FVector Location(0.f, Position.X, Position.Y);
	Slot.Panel->SetRelativeLocation(Location);
	Slot.Panel->SetRelativeScale3D(FVector(ButtonDepth, Size.X / 100.f, Size.Y / 100.f));
	Slot.Visual->SetRelativeLocation(Location + FVector(1.f, 0.f, 0.f));
	SetPanelShape(Slot.Visual, Size, Size.Y * 0.5f, ButtonFill, ButtonRim, 0.f);
	Slot.Label->SetRelativeLocation(Location + FVector(3.f, 0.f, 0.f));
	Slot.Label->SetWorldSize(TextSize);
	Slot.Label->SetText(FText::FromString(Text));

	const bool bUsed = Action != EMenuButton::None;
	Slot.Visual->SetVisibility(bUsed);
	Slot.Label->SetVisibility(bUsed);
	SetButtonCollision(Slot.Panel, false); // enabled in ShowMenu
}

void AGameFlowManager::ShowMenu(const FString& Title, const FString& Subtitle, bool bReposition)
{
	if (bReposition || !bMenuVisible)
	{
		PlaceInFrontOfPlayer();
	}
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
	const FVector Centre = Camera->GetCameraLocation() + FVector(0.0, 0.0, MenuHeightOffset);
	const float ViewYaw = Camera->GetCameraRotation().Yaw;

	// In the open arena straight ahead is always free, but in the streets the player may be facing a wall: a menu
	// placed inside a building can't be pressed. Try straight ahead, then fan out, and take the first direction with
	// a clear line to the menu (the beam has to reach it) and room for the whole menu; failing that the most open one.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MenuPlacement), false, this);
	Params.AddIgnoredActor(PC->GetPawn());
	const FCollisionObjectQueryParams Statics(ECC_WorldStatic);
	const float Reach = MenuDistance + MenuClearance;

	float BestYaw = ViewYaw;
	float BestClear = -1.f;
	bool bFoundRoom = false;
	for (const float Offset : MenuCandidateYaws)
	{
		const float Yaw = ViewYaw + Offset;
		const FVector Direction = FRotator(0.f, Yaw, 0.f).Vector();
		float Clear = Reach;
		FHitResult Hit;
		if (GetWorld()->SweepSingleByObjectType(Hit, Centre, Centre + Direction * Reach, FQuat::Identity, Statics,
			FCollisionShape::MakeSphere(MenuSightRadius), Params))
		{
			Clear = Hit.Distance;
		}
		if (Clear >= Reach && !GetWorld()->OverlapAnyTestByObjectType(Centre + Direction * MenuDistance,
			FRotator(0.f, Yaw, 0.f).Quaternion(), Statics, FCollisionShape::MakeBox(MenuFootprintHalfExtent), Params))
		{
			BestYaw = Yaw;
			bFoundRoom = true;
			break;
		}
		if (Clear > BestClear)
		{
			BestClear = Clear;
			BestYaw = Yaw;
		}
	}

	const float Distance = bFoundRoom ? MenuDistance : FMath::Clamp(BestClear - MenuClearance, MinMenuDistance, MenuDistance);
	SetActorLocationAndRotation(Centre + FRotator(0.f, BestYaw, 0.f).Vector() * Distance, FRotator(0.f, BestYaw + 180.f, 0.f));
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
		// Copy: pressing reconfigures the slots.
		const EMenuButton Action = Slot->Action;
		const int32 Param = Slot->Param;
		PressButton(Action, Param);
	}
	return DamageAmount;
}

void AGameFlowManager::PressButton(EMenuButton Action, int32 Param)
{
	for (FButtonSlot& Slot : Slots)
	{
		Slot.Charge = 0.f;
	}
	PlayUISound(ConfirmSound);

	const int32 CharsetLen = FCString::Strlen(InitialsCharset);
	auto StepLetter = [this, Param, CharsetLen](int32 Step)
	{
		if (Param < 0 || Param >= EditInitials.Len())
		{
			return;
		}
		int32 Index = 0;
		FString(InitialsCharset).FindChar(EditInitials[Param], Index);
		Index = (Index + Step + CharsetLen) % CharsetLen;
		EditInitials[Param] = InitialsCharset[Index];
		RefreshInitialsLetters();
	};

	switch (Action)
	{
	case EMenuButton::Start:
	case EMenuButton::Restart:
	case EMenuButton::PlayAgain:
		if (Spawner && State == EGameFlowState::Paused)
		{
			Spawner->SetGamePaused(false);
		}
		StartGame();
		break;
	case EMenuButton::Resume:     ResumeGame(); break;
	case EMenuButton::MainMenu:   ShowTitle(); break;
	case EMenuButton::EditName:   ShowInitialsEditor(/*bReposition*/ false); break;
	case EMenuButton::SwitchLevel:
		HideMenu();
		UGameplayStatics::OpenLevelBySoftObjectPtr(this, OtherLevel, /*bAbsolute*/ true, StartOption);
		break;
	case EMenuButton::LetterUp:   StepLetter(1); break;   // hold the beam on it to keep stepping
	case EMenuButton::LetterDown: StepLetter(-1); break;
	case EMenuButton::NameDone:
	{
		UCyclopsSaveGame* Save = UCyclopsSaveGame::LoadOrCreate();
		Save->Initials = EditInitials;
		Save->Save();
		if (State == EGameFlowState::GameOver)
		{
			if (!bLastSubmitted && !bSubmitting)
			{
				SubmitLastRun();
			}
			ShowGameOver(/*bReposition*/ false);
		}
		else
		{
			ShowTitle();
		}
		break;
	}
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
		if (Slot.Action == EMenuButton::None)
		{
			continue;
		}
		if (Now - Slot.LastHitTime > 0.1f)
		{
			Slot.Charge = FMath::Max(0.f, Slot.Charge - ChargeDecayPerSecond * DeltaSeconds);
		}
		const float Fraction = FMath::Clamp(Slot.Charge / ChargeRequired, 0.f, 1.f);
		const float Swell = 1.f + 0.12f * Fraction;
		Slot.Visual->SetCustomPrimitiveDataFloat(3, Fraction); // Glow
		Slot.Visual->SetRelativeScale3D(FVector(Slot.Size.Y * Swell / 100.f, Slot.Size.X * Swell / 100.f, 1.f));
		Slot.Panel->SetRelativeScale3D(FVector(ButtonDepth, Slot.Size.X * Swell / 100.f, Slot.Size.Y * Swell / 100.f));
	}
}

void AGameFlowManager::PlayUISound(USoundBase* Sound) const
{
	if (Sound)
	{
		UGameplayStatics::PlaySound2D(this, Sound);
	}
}
