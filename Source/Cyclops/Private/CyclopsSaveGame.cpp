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

int32 UCyclopsSaveGame::GetBest(const FString& Mode) const
{
	if (const int32* Best = BestScores.Find(Mode))
	{
		return *Best;
	}
	return Mode == TEXT("arena") ? BestScore : 0;
}

void UCyclopsSaveGame::SetBest(const FString& Mode, int32 Score)
{
	BestScores.Add(Mode, Score);
}

void UCyclopsSaveGame::Save()
{
	UGameplayStatics::SaveGameToSlot(this, SlotName, 0);
}
