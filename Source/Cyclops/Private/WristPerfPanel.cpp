#include "WristPerfPanel.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/TextRenderComponent.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "MotionControllerComponent.h"
#include "RHIStats.h"
#include "RenderTimer.h"

DEFINE_LOG_CATEGORY_STATIC(LogWristPerfPanel, Log, All);

AWristPerfPanel::AWristPerfPanel()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the camera manager has its pose for the frame, so the billboard doesn't lag.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Text = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Text"));
	Text->SetHorizontalAlignment(EHTA_Center);
	Text->SetVerticalAlignment(EVRTA_TextBottom);
	Text->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Text->SetCastShadow(false);
	Text->SetText(FText::FromString(TEXT("perf")));
	RootComponent = Text;
}

void AWristPerfPanel::BeginPlay()
{
	Super::BeginPlay();

	Text->SetWorldSize(TextSize);
	SetPanelVisible(bStartVisible);

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && ToggleAction)
	{
		EnableInput(PC);
		if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent))
		{
			Input->BindAction(ToggleAction, ETriggerEvent::Triggered, this, &AWristPerfPanel::HandleToggle);
		}
	}

	TryAttachToController();
}

bool AWristPerfPanel::TryAttachToController()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}

	TInlineComponentArray<UMotionControllerComponent*> Controllers(Pawn);
	for (UMotionControllerComponent* Controller : Controllers)
	{
		if (Controller->MotionSource == MotionSource)
		{
			AttachToComponent(Controller, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
			Text->SetRelativeLocation(FVector::ZeroVector);
			SetActorRelativeLocation(AttachOffset);
			AttachedController = Controller;
			return true;
		}
	}

	if (!bWarnedNoController)
	{
		bWarnedNoController = true;
		UE_LOG(LogWristPerfPanel, Warning, TEXT("No motion controller with source '%s' on %s; panel stays unattached."),
			*MotionSource.ToString(), *Pawn->GetName());
	}
	return false;
}

void AWristPerfPanel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!AttachedController.IsValid())
	{
		TryAttachToController();
	}

	// Count frames even while hidden so the first reading after showing is meaningful.
	AccumulatedTime += DeltaSeconds;
	++AccumulatedFrames;

	if (!bPanelVisible)
	{
		return;
	}

	FaceCamera();

	if (AccumulatedTime >= UpdateInterval)
	{
		RefreshText();
		AccumulatedTime = 0.0;
		AccumulatedFrames = 0;
	}
}

void AWristPerfPanel::FaceCamera()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return;
	}

	// Text renders facing its +X axis, so point +X at the eyes.
	const FVector ToCamera = Camera->GetCameraLocation() - GetActorLocation();
	if (!ToCamera.IsNearlyZero())
	{
		SetActorRotation(FRotationMatrix::MakeFromXZ(ToCamera, FVector::UpVector).Rotator());
	}
}

void AWristPerfPanel::RefreshText()
{
	const double Fps = AccumulatedTime > 0.0 ? AccumulatedFrames / AccumulatedTime : 0.0;
	const double FrameMs = Fps > 0.0 ? 1000.0 / Fps : 0.0;

	const double GameMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double RenderMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const double RhiMs = FPlatformTime::ToMilliseconds(GRHIThreadTime);
	const uint32 GpuCycles = RHIGetGPUFrameCycles(0);
	const FString GpuText = GpuCycles > 0 ? FString::Printf(TEXT("%.1f"), FPlatformTime::ToMilliseconds(GpuCycles)) : FString(TEXT("n/a"));

	const int32 DrawCalls = GNumDrawCallsRHI[0];
	const int32 Primitives = GNumPrimitivesDrawnRHI[0];
	const uint64 UsedMB = FPlatformMemory::GetStats().UsedPhysical / (1024 * 1024);

	const FString Line = FString::Printf(
		TEXT("%.0f / %.0f fps  (%.1f ms)\n")
		TEXT("Game %.1f  Draw %.1f  RHI %.1f\n")
		TEXT("GPU %s ms\n")
		TEXT("Draws %d  Prims %.1fk\n")
		TEXT("Mem %llu MB"),
		Fps, TargetFrameRate, FrameMs,
		GameMs, RenderMs, RhiMs,
		*GpuText,
		DrawCalls, Primitives / 1000.0,
		UsedMB);
	Text->SetText(FText::FromString(Line));

	// Green at target, yellow within 10%, red below.
	const double Ratio = Fps / TargetFrameRate;
	const FColor Colour = Ratio >= 0.97 ? FColor(80, 255, 120) : (Ratio >= 0.9 ? FColor(255, 220, 60) : FColor(255, 70, 60));
	Text->SetTextRenderColor(Colour);
}

void AWristPerfPanel::TogglePanel()
{
	SetPanelVisible(!bPanelVisible);
}

void AWristPerfPanel::SetPanelVisible(bool bVisible)
{
	bPanelVisible = bVisible;
	SetActorHiddenInGame(!bVisible);
}

void AWristPerfPanel::HandleToggle(const FInputActionValue& Value)
{
	TogglePanel();
}
