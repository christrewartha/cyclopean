#pragma once

#include "CoreMinimal.h"
#include "WalkingEnemy.h"
#include "ThrowerEnemy.generated.h"

/**
 * Walker variant that stops at range and throws glowing rocks (AEnemyRock). Olive body that pulses red-orange while
 * winding up, with a rising howl - the warning - then the throw. Still attacks up close like any walker.
 * A little more fragile than a plain walker, so shooting it during the wind-up is the counter.
 */
UCLASS()
class CYCLOPS_API AThrowerEnemy : public AWalkingEnemy
{
	GENERATED_BODY()

public:
	AThrowerEnemy();
};
