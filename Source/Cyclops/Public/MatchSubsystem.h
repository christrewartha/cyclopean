#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MatchSubsystem.generated.h"

class FJsonObject;
class IWebSocket;

UENUM(BlueprintType)
enum class EMatchStatus : uint8
{
	Offline,
	Connecting,
	Waiting,   // in the room, no opponent yet
	Paired,    // both players in the room
	Error,     // see GetLastError(); call Leave() before joining again
};

DECLARE_MULTICAST_DELEGATE(FOnMatchStatusChanged);
/** A message from the other player: its "t" type and the whole JSON object. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMatchMessage, const FString& /*Type*/, const TSharedRef<FJsonObject>& /*Message*/);

/**
 * Connection to the versus relay (Server/leaderboard/relay.py): joins a 3-character room and passes JSON messages to
 * and from the other player in it. Knows nothing about gameplay - AVersusMatch builds on it.
 *
 * Lives on the game instance so a match could survive level travel. Server URL = the leaderboard URL with
 * https -> wss + /ws/v1/match; override with the console variable cyclops.MatchUrl (e.g. a local relay).
 */
UCLASS()
class CYCLOPS_API UMatchSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Connects and joins Room (3 x A-Z / 0-9) as Name. Leaves any current room first. */
	void Join(const FString& Room, const FString& Name);
	void Leave();

	/** Sends a message to the other player (dropped unless paired). Type becomes the "t" field. */
	void Send(const FString& Type, const TSharedRef<FJsonObject>& Message);

	EMatchStatus GetStatus() const { return Status; }
	bool IsPaired() const { return Status == EMatchStatus::Paired; }
	const FString& GetRoom() const { return Room; }
	const FString& GetPeerName() const { return PeerName; }
	const FString& GetLastError() const { return LastError; }
	/** 0 or 1: who joined first. Decides the start position. -1 before joining. */
	int32 GetSlot() const { return Slot; }
	/** Latest round trip to the relay, in ms (-1 until measured). */
	float GetRoundTripMs() const { return RoundTripMs; }

	FOnMatchStatusChanged OnStatusChanged;
	FOnMatchMessage OnMessage;

	static FString GetServerUrl();

private:
	void SetStatus(EMatchStatus NewStatus, const FString& Error = FString());
	void HandleConnected();
	void HandleConnectionError(const FString& Error);
	void HandleClosed(int32 Code, const FString& Reason, bool bWasClean);
	void HandleMessage(const FString& Text);
	void SendRaw(const TSharedRef<FJsonObject>& Message);
	bool Tick(float DeltaSeconds);

	TSharedPtr<IWebSocket> Socket;
	FTSTicker::FDelegateHandle TickHandle;
	EMatchStatus Status = EMatchStatus::Offline;
	FString Room;
	FString Name;
	FString PeerName;
	FString LastError;
	int32 Slot = -1;
	float RoundTripMs = -1.f;
	double LastPingTime = 0.0;
};
