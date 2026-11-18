#include "CyclopsSaveGame.h"

#include "Kismet/GameplayStatics.h"

UCyclopsSaveGame* UCyclopsSaveGame::LoadOrCreate()
{
	if (UCyclopsSaveGame* Existing = Cast<UCyclopsSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0)))
	{
		return Existing;
	}
	return Cast<UCyclopsSaveGame>(UGameplayStatics::CreateSaveGameObject(UCyclopsSaveGame::StaticClass()));
}

void UCyclopsSaveGame::Save()
{
	UGameplayStatics::SaveGameToSlot(this, SlotName, 0);
}
