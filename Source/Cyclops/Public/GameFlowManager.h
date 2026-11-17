#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFlowManager.generated.h"

class AEnemySpawner;
class UInputAction;
class UMaterialInterface;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;
class UTextRenderComponent;
struct FInputActionValue;

UENUM(BlueprintType)
enum class EGameFlowState : uint8
{
	Title,
	Playing,
	Paused,
};

UENUM()
enum class EMenuButton : uint8
{
	None,
	Start,
	Resume,
	Restart,
	MainMenu,
};

/**
 * Start screen + pause menu for VR, operated with the optic blast: keep the beam on a button for
 * ChargeRequired damage to press it. The menu is this actor; it repositions itself in front of the
 * player's view whenever it opens.
 *
 * Pause is a soft pause: the spawner stops and enemies freeze, but the engine keeps running so the
 * HMD, hands and beam still work (a real game pause would freeze the player's hands).
 */
UCLASS()
class CYCLOPS_API AGameFlowManager : public AActor
{
	GENERATED_BODY()

public:
	AGameFlowManager();

	UFUNCTION(BlueprintCallable, Category = "Game Flow")
	void ShowTitle();

	UFUNCTION(BlueprintCallable, Category = "Game Flow")
	void StartGame();

	UFUNCTION(BlueprintCallable, Category = "Game Flow")
	void PauseGame();

	UFUNCTION(BlueprintCallable, Category = "Game Flow")
	void ResumeGame();

	UFUNCTION(BlueprintPure, Category = "Game Flow")
	EGameFlowState GetFlowState() const { return State; }

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** The spawner in this level that this manager drives. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow")
	TObjectPtr<AEnemySpawner> Spawner;

	/** Pause / resume (and start, from the title). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Input")
	TObjectPtr<UInputAction> PauseAction;

	/** Beam damage needed to press a button (beam does 100/s by default, so 30 = ~0.3 s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Flow|Menu", meta = (ClampMin = "1"))
	float ChargeRequired = 30.f;

	/** Charge lost per second when the beam isn't on a button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Flow|Menu", meta = (ClampMin = "0"))
	float ChargeDecayPerSecond = 60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Flow|Menu", meta = (Units = "cm"))
	float MenuDistance = 260.f;

	/** Menu centre relative to eye height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Flow|Menu", meta = (Units = "cm"))
	float MenuHeightOffset = -15.f;

	/** Invisible hit volume for each button (a 100 cm cube). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<UStaticMesh> ButtonMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<UMaterialInterface> ButtonMaterial;

	/** Flat 100 cm plane used for the panel and button visuals. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<UStaticMesh> PanelMesh;

	/** Rounded-rect material driven by custom primitive data (size, radius, glow, fill, rim). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<UMaterialInterface> PanelMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UStaticMesh> GraphicSphereMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UStaticMesh> GraphicCubeMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UStaticMesh> GraphicCylinderMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UMaterialInterface> BatBodyMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UMaterialInterface> BatWingMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UMaterialInterface> BatEyeMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UMaterialInterface> GraphicBeamMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets|Graphic")
	TObjectPtr<UMaterialInterface> GraphicBeamGlowMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<USoundBase> ConfirmSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Flow|Assets")
	TObjectPtr<USoundBase> PauseSound;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FButtonSlot
	{
		UStaticMeshComponent* Panel = nullptr;   // invisible hit volume
		UStaticMeshComponent* Visual = nullptr;  // rounded pill
		UTextRenderComponent* Label = nullptr;
		EMenuButton Action = EMenuButton::None;
		float Charge = 0.f;
		float LastHitTime = -1.f;
	};

	void HandlePauseInput(const FInputActionValue& Value);
	void ConfigureButton(int32 Index, EMenuButton Action, const FString& Text);
	void ShowMenu(const FString& Title, const FString& Subtitle);
	void HideMenu();
	void PlaceInFrontOfPlayer();
	void PressButton(EMenuButton Action);
	void PlayUISound(USoundBase* Sound) const;
	/** Applies pause state to every optic blast in the world (energy frozen while paused). */
	void SetBeamsPaused(bool bPaused, bool bRefill) const;
	void ApplyMeshesAndMaterials();
	void AnimateGraphic(float Time);
	FButtonSlot* FindSlot(const UPrimitiveComponent* Component);

	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TObjectPtr<USceneComponent> MenuRoot;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TObjectPtr<UTextRenderComponent> TitleText;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TObjectPtr<UTextRenderComponent> SubtitleText;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TObjectPtr<UStaticMeshComponent> BackgroundPanel;

	/** Centre, left, right: invisible hit volumes the beam presses. */
	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TArray<TObjectPtr<UStaticMeshComponent>> ButtonPanels;

	/** Rounded pill visuals, one per button. */
	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TArray<TObjectPtr<UStaticMeshComponent>> ButtonVisuals;

	// Title graphic: a bat getting zapped.
	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TObjectPtr<USceneComponent> GraphicBat;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TObjectPtr<UStaticMeshComponent> GraphicBatBody;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TArray<TObjectPtr<UStaticMeshComponent>> GraphicBatEyes;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TArray<TObjectPtr<USceneComponent>> GraphicWingPivots;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TArray<TObjectPtr<UStaticMeshComponent>> GraphicWings;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TObjectPtr<UStaticMeshComponent> GraphicBeam;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TObjectPtr<UStaticMeshComponent> GraphicBeamGlow;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow|Graphic")
	TObjectPtr<UStaticMeshComponent> GraphicImpact;

	UPROPERTY(VisibleAnywhere, Category = "Game Flow")
	TArray<TObjectPtr<UTextRenderComponent>> ButtonLabels;

	TArray<FButtonSlot> Slots;
	EGameFlowState State = EGameFlowState::Title;
	bool bMenuVisible = false;
	float MenuOpenTime = 0.f;
};
