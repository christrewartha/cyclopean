#include "MatchSubsystem.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "IWebSocket.h"
#include "LeaderboardConfig.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogMatch, Log, All);

namespace
{
	constexpr int32 ProtocolVersion = 1; // must match relay.py PROTOCOL
	const TCHAR* MatchPath = TEXT("/ws/v1/match");
	constexpr double PingInterval = 3.0; // keeps the connection alive through Cloudflare and measures lag

	TAutoConsoleVariable<FString> CVarMatchUrl(
		TEXT("cyclops.MatchUrl"), TEXT(""),
		TEXT("Versus relay URL override, e.g. ws://127.0.0.1:8797/ws/v1/match. Empty = derived from the leaderboard URL."));
}

void UMatchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FModuleManager::Get().LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UMatchSubsystem::Tick), 0.5f);
}

void UMatchSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Leave();
	Super::Deinitialize();
}

FString UMatchSubsystem::GetServerUrl()
{
	const FString Override = CVarMatchUrl.GetValueOnGameThread();
	if (!Override.IsEmpty())
	{
		return Override;
	}
	FString Url = FString(CYCLOPS_LEADERBOARD_URL);
	Url.ReplaceInline(TEXT("https://"), TEXT("wss://"));
	Url.ReplaceInline(TEXT("http://"), TEXT("ws://"));
	return Url.IsEmpty() ? Url : Url + MatchPath;
}

void UMatchSubsystem::Join(const FString& InRoom, const FString& InName)
{
	Leave();
	const FString Url = GetServerUrl();
	if (Url.IsEmpty())
	{
		SetStatus(EMatchStatus::Error, TEXT("No server configured"));
		return;
	}
	Room = InRoom.ToUpper();
	Name = InName.Len() == 3 ? InName.ToUpper() : FString::Printf(TEXT("P%02d"), static_cast<int32>(FDateTime::Now().GetTicks() / ETimespan::TicksPerMillisecond % 100)) /* clock, not FMath::Rand: unseeded, so every fresh install got the same name */;
	Socket = FWebSocketsModule::Get().CreateWebSocket(Url);
	Socket->OnConnected().AddUObject(this, &UMatchSubsystem::HandleConnected);
	Socket->OnConnectionError().AddUObject(this, &UMatchSubsystem::HandleConnectionError);
	Socket->OnClosed().AddUObject(this, &UMatchSubsystem::HandleClosed);
	Socket->OnMessage().AddUObject(this, &UMatchSubsystem::HandleMessage);
	UE_LOG(LogMatch, Log, TEXT("Connecting to %s, room %s as %s"), *Url, *Room, *Name);
	SetStatus(EMatchStatus::Connecting);
	Socket->Connect();
}

void UMatchSubsystem::Leave()
{
	if (Socket.IsValid())
	{
		// Unbind first: closing must not report "connection lost" for a deliberate leave.
		Socket->OnConnected().RemoveAll(this);
		Socket->OnConnectionError().RemoveAll(this);
		Socket->OnClosed().RemoveAll(this);
		Socket->OnMessage().RemoveAll(this);
		Socket->Close();
		Socket.Reset();
	}
	Slot = -1;
	PeerName.Reset();
	RoundTripMs = -1.f;
	if (Status != EMatchStatus::Offline)
	{
		SetStatus(EMatchStatus::Offline);
	}
}

void UMatchSubsystem::SetStatus(EMatchStatus NewStatus, const FString& Error)
{
	Status = NewStatus;
	LastError = Error;
	if (!Error.IsEmpty())
	{
		UE_LOG(LogMatch, Warning, TEXT("Match error: %s"), *Error);
	}
	OnStatusChanged.Broadcast();
}

void UMatchSubsystem::HandleConnected()
{
	const TSharedRef<FJsonObject> JoinMessage = MakeShared<FJsonObject>();
	JoinMessage->SetStringField(TEXT("t"), TEXT("join"));
	JoinMessage->SetStringField(TEXT("room"), Room);
	JoinMessage->SetStringField(TEXT("name"), Name);
	JoinMessage->SetNumberField(TEXT("v"), ProtocolVersion);
	SendRaw(JoinMessage);
}

void UMatchSubsystem::HandleConnectionError(const FString& Error)
{
	SetStatus(EMatchStatus::Error, FString::Printf(TEXT("Can't reach the server (%s)"), *Error));
}

void UMatchSubsystem::HandleClosed(int32 Code, const FString& Reason, bool bWasClean)
{
	if (Status != EMatchStatus::Error && Status != EMatchStatus::Offline)
	{
		SetStatus(EMatchStatus::Error, TEXT("Connection lost"));
	}
}

void UMatchSubsystem::HandleMessage(const FString& Text)
{
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
	{
		return;
	}
	const FString Type = Json->GetStringField(TEXT("t"));
	if (Type == TEXT("joined"))
	{
		Slot = static_cast<int32>(Json->GetNumberField(TEXT("slot")));
		PeerName.Reset();
		Json->TryGetStringField(TEXT("peer"), PeerName); // null while waiting
		UE_LOG(LogMatch, Log, TEXT("Joined room %s as slot %d%s"), *Room, Slot, PeerName.IsEmpty() ? TEXT(", waiting") : *(TEXT(" with ") + PeerName));
		SetStatus(PeerName.IsEmpty() ? EMatchStatus::Waiting : EMatchStatus::Paired);
	}
	else if (Type == TEXT("peer"))
	{
		PeerName = Json->GetStringField(TEXT("name"));
		UE_LOG(LogMatch, Log, TEXT("%s joined"), *PeerName);
		SetStatus(EMatchStatus::Paired);
	}
	else if (Type == TEXT("left"))
	{
		UE_LOG(LogMatch, Log, TEXT("%s left"), *PeerName);
		PeerName.Reset();
		SetStatus(EMatchStatus::Waiting);
	}
	else if (Type == TEXT("error"))
	{
		SetStatus(EMatchStatus::Error, Json->GetStringField(TEXT("msg")));
	}
	else if (Type == TEXT("pong"))
	{
		double Sent = 0.0;
		if (Json->TryGetNumberField(TEXT("c"), Sent))
		{
			RoundTripMs = static_cast<float>((FPlatformTime::Seconds() - Sent) * 1000.0);
		}
	}
	else if (Status == EMatchStatus::Paired)
	{
		OnMessage.Broadcast(Type, Json.ToSharedRef());
	}
}

void UMatchSubsystem::Send(const FString& Type, const TSharedRef<FJsonObject>& Message)
{
	if (Status != EMatchStatus::Paired)
	{
		return;
	}
	Message->SetStringField(TEXT("t"), Type);
	SendRaw(Message);
}

void UMatchSubsystem::SendRaw(const TSharedRef<FJsonObject>& Message)
{
	if (!Socket.IsValid() || !Socket->IsConnected())
	{
		return;
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Message, Writer);
	Socket->Send(Text);
}

bool UMatchSubsystem::Tick(float DeltaSeconds)
{
	const double Now = FPlatformTime::Seconds();
	if (Socket.IsValid() && Socket->IsConnected() && Now - LastPingTime >= PingInterval)
	{
		LastPingTime = Now;
		const TSharedRef<FJsonObject> Ping = MakeShared<FJsonObject>();
		Ping->SetStringField(TEXT("t"), TEXT("ping"));
		Ping->SetNumberField(TEXT("c"), Now);
		SendRaw(Ping);
	}
	return true;
}
