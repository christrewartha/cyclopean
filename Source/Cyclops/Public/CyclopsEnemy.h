#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CyclopsEnemy.generated.h"

class AEnemySpawner;

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UCyclopsEnemy : public UInterface
{
	GENERATED_BODY()
};

/**
 * What AEnemySpawner needs from any enemy it spawns (flying bats, walking humanoids, ...).
 * Enemies report kills / hits back through AEnemySpawner::NotifyEnemyKilled / NotifyEnemyReachedPlayer.
 */
class CYCLOPS_API ICyclopsEnemy
{
	GENERATED_BODY()

public:
	/** Called by the spawner right after spawning. Speed is the spawner's current difficulty speed (cm/s). */
	virtual void InitEnemy(AEnemySpawner* InSpawner, float InSpeed) = 0;

	/** Freeze in place (game paused): no movement, no damage, sounds paused. */
	virtual void SetFrozen(bool bInFrozen) = 0;

	/** Dead enemies no longer count towards the spawner's alive limit. */
	virtual bool IsDead() const = 0;
};
