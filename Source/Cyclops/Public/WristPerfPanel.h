#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WristPerfPanel.generated.h"

class UInputAction;
class UMotionControllerComponent;
class UTextRenderComponent;
struct FInputActionValue;

/**
 * Small world-space performance readout that attaches itself to the player's left controller.
 *
 * Shows FPS vs target, game / render / RHI / GPU frame times, draw calls, primitives and memory
 * (the same sources as `stat unit` / `stat rhi`). The text always turns to face the HMD.
 * Place one in a level; it finds the local pawn's motion controller at runtime, so the pawn
 * Blueprint doesn't need to change.
 */
UCLASS()
class CYCLOPS_API AWristPerfPanel : public AActor
{
	GENERATED_BODY()

public:
	AWristPerfPanel();

	UFUNCTION(BlueprintCallable, Category = "Perf Panel")
	void TogglePanel();

	UFUNCTION(BlueprintCallable, Category = "Perf Panel")
	void SetPanelVisible(bool bVisible);

	/** Pressing this toggles the panel. Should use a Pressed trigger. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Perf Panel|Input")
	TObjectPtr<UInputAction> ToggleAction;

	/** Motion source of the controller to attach to (template pawn: LeftGrip). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Perf Panel|Attach")
	FName MotionSource = TEXT("LeftGrip");

	/** Offset from the controller, in the controller's space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perf Panel|Attach", meta = (Units = "cm"))
	FVector AttachOffset = FVector(-4.0, 0.0, 9.0);

	/** Frame rate the headset is running at; drives the colour of the FPS line. Quest 2 default is 72. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perf Panel", meta = (ClampMin = "1"))
	float TargetFrameRate = 72.f;

	/** How often the text refreshes; FPS is averaged over this window. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perf Panel", meta = (ClampMin = "0.05", Units = "s"))
	float UpdateInterval = 0.25f;

	/** World height of one line of text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perf Panel", meta = (ClampMin = "0.1", Units = "cm"))
	float TextSize = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Perf Panel")
	bool bStartVisible = true;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool TryAttachToController();
	void FaceCamera();
	void RefreshText();
	void HandleToggle(const FInputActionValue& Value);

	UPROPERTY(VisibleAnywhere, Category = "Perf Panel")
	TObjectPtr<UTextRenderComponent> Text;

	TWeakObjectPtr<UMotionControllerComponent> AttachedController;

	double AccumulatedTime = 0.0;
	int32 AccumulatedFrames = 0;
	bool bPanelVisible = true;
	bool bWarnedNoController = false;
};
