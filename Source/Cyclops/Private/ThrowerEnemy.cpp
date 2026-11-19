#include "ThrowerEnemy.h"

#include "EnemyRock.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

AThrowerEnemy::AThrowerEnemy()
{
	bCanThrow = true;
	MaxHealth = 45.f;
	RockClass = AEnemyRock::StaticClass();
	ThrowSpeed = 1600.f; // ~0.6 s to cover 10 m: the wind-up is the warning, the rock itself should be hard to dodge
	// Throwers mostly come straight on (they stop at range anyway); none try to ambush.
	ChaserWeight = 0.7f;
	FlankerWeight = 0.3f;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Body(TEXT("/Game/Cyclops/Materials/MI_ThrowerBody.MI_ThrowerBody"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Rock(TEXT("/Game/Cyclops/Materials/MI_ThrowerRock.MI_ThrowerRock"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Charge(TEXT("/Game/Cyclops/Audio/SW_ThrowerCharge.SW_ThrowerCharge"));
	static ConstructorHelpers::FObjectFinder<USoundBase> Release(TEXT("/Game/Cyclops/Audio/SW_ThrowRelease.SW_ThrowRelease"));
	BodyMaterial = Body.Object;
	RockMaterial = Rock.Object;
	ChargeSound = Charge.Object;
	ReleaseSound = Release.Object;
}
