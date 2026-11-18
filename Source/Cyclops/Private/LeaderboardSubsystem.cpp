#include "LeaderboardSubsystem.h"

#include "CyclopsSaveGame.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "LeaderboardConfig.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogLeaderboard, Log, All);

namespace
{
	const TCHAR* ScoresPath = TEXT("/api/v1/scores");
	constexpr float RequestTimeout = 8.f;
	constexpr int32 MaxPendingScores = 20;

	FString GameVersion()
	{
		FString Version;
		GConfig->GetString(TEXT("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings"), TEXT("VersionDisplayName"), Version, GEngineIni);
		return Version;
	}

	TSharedPtr<FJsonObject> ParseJson(const FHttpResponsePtr& Response)
	{
		TSharedPtr<FJsonObject> Json;
		if (Response.IsValid())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Json);
		}
		return Json;
	}
}

bool ULeaderboardSubsystem::IsConfigured() const
{
	return !FString(CYCLOPS_LEADERBOARD_URL).IsEmpty();
}

FString ULeaderboardSubsystem::Sign(const FString& Body)
{
	const FTCHARToUTF8 BodyUtf8(*Body);
	const char* Key = CYCLOPS_LEADERBOARD_KEY;
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HMACBuffer(Key, FCStringAnsi::Strlen(Key), BodyUtf8.Get(), BodyUtf8.Length(), Hash);
	return BytesToHex(Hash, FSHA1::DigestSize).ToLower();
}

bool ULeaderboardSubsystem::ParseTop(const TSharedPtr<FJsonObject>& Json, TArray<FLeaderboardEntry>& OutTop)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("top"), Rows))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Row : *Rows)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Row.IsValid() && Row->TryGetObject(Obj))
		{
			FLeaderboardEntry& Entry = OutTop.AddDefaulted_GetRef();
			Entry.Rank = (*Obj)->GetIntegerField(TEXT("rank"));
			Entry.Name = (*Obj)->GetStringField(TEXT("name"));
			Entry.Score = (*Obj)->GetIntegerField(TEXT("score"));
		}
	}
	return true;
}

void ULeaderboardSubsystem::FetchTop(FOnLeaderboardResult OnDone)
{
	if (!IsConfigured())
	{
		OnDone.ExecuteIfBound(FLeaderboardResult());
		return;
	}

	SendPendingScores();

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString(CYCLOPS_LEADERBOARD_URL) + ScoresPath + TEXT("?limit=10"));
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(RequestTimeout);
	Request->OnProcessRequestComplete().BindWeakLambda(this, [OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FLeaderboardResult Result;
		Result.bOnline = bConnected && Response.IsValid() && Response->GetResponseCode() == 200 && ParseTop(ParseJson(Response), Result.Top);
		if (!Result.bOnline)
		{
			UE_LOG(LogLeaderboard, Warning, TEXT("Fetch failed (connected=%d, code=%d)"), bConnected, Response.IsValid() ? Response->GetResponseCode() : 0);
		}
		OnDone.ExecuteIfBound(Result);
	});
	Request->ProcessRequest();
}

void ULeaderboardSubsystem::SubmitScore(const FString& Name, int32 Score, int32 Kills, float Duration, FOnLeaderboardResult OnDone)
{
	if (!IsConfigured())
	{
		OnDone.ExecuteIfBound(FLeaderboardResult());
		return;
	}

	PostScore(Name, Score, Kills, Duration, [this, Name, Score, Kills, Duration, OnDone](bool bAccepted, const FLeaderboardResult& Result)
	{
		if (!Result.bOnline)
		{
			// Couldn't reach the service: keep the run and try again later.
			UCyclopsSaveGame* SaveGame = UCyclopsSaveGame::LoadOrCreate();
			if (SaveGame->PendingScores.Num() < MaxPendingScores)
			{
				SaveGame->PendingScores.Add({ Name, Score, Kills, Duration });
				SaveGame->Save();
			}
		}
		else
		{
			SendPendingScores();
		}
		OnDone.ExecuteIfBound(Result);
	});
}

void ULeaderboardSubsystem::SendPendingScores()
{
	if (bFlushingPending)
	{
		return;
	}
	UCyclopsSaveGame* SaveGame = UCyclopsSaveGame::LoadOrCreate();
	if (SaveGame->PendingScores.IsEmpty())
	{
		return;
	}

	bFlushingPending = true;
	const FPendingScore Next = SaveGame->PendingScores[0];
	PostScore(Next.Name, Next.Score, Next.Kills, Next.Duration, [this](bool bAccepted, const FLeaderboardResult& Result)
	{
		bFlushingPending = false;
		if (!Result.bOnline)
		{
			return; // still offline; keep everything queued
		}
		// Sent, or rejected by validation (won't ever succeed): drop it and continue with the rest.
		UCyclopsSaveGame* SaveGame = UCyclopsSaveGame::LoadOrCreate();
		if (!SaveGame->PendingScores.IsEmpty())
		{
			SaveGame->PendingScores.RemoveAt(0);
			SaveGame->Save();
		}
		SendPendingScores();
	});
}

void ULeaderboardSubsystem::PostScore(const FString& Name, int32 Score, int32 Kills, float Duration,
	TFunction<void(bool bAccepted, const FLeaderboardResult&)> OnDone)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetNumberField(TEXT("score"), Score);
	Json->SetNumberField(TEXT("kills"), Kills);
	Json->SetNumberField(TEXT("duration"), FMath::Max(Duration, 0.1f));
	Json->SetStringField(TEXT("version"), GameVersion());

	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Json, Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FString(CYCLOPS_LEADERBOARD_URL) + ScoresPath);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-Signature"), Sign(Body));
	Request->SetContentAsString(Body); // sent as UTF-8, matching what was signed
	Request->SetTimeout(RequestTimeout);
	Request->OnProcessRequestComplete().BindWeakLambda(this, [OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FLeaderboardResult Result;
		const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
		// Reachable = we got an HTTP answer that isn't a server/gateway error.
		Result.bOnline = bConnected && Code > 0 && Code < 500;
		const bool bAccepted = Code == 200;
		if (bAccepted)
		{
			const TSharedPtr<FJsonObject> Json = ParseJson(Response);
			ParseTop(Json, Result.Top);
			if (Json.IsValid())
			{
				Result.Rank = Json->GetIntegerField(TEXT("rank"));
			}
		}
		else
		{
			UE_LOG(LogLeaderboard, Warning, TEXT("Submit not accepted (connected=%d, code=%d): %s"), bConnected, Code,
				Response.IsValid() ? *Response->GetContentAsString() : TEXT(""));
		}
		OnDone(bAccepted, Result);
	});
	Request->ProcessRequest();
}
