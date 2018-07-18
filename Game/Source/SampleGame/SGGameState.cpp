// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SGGameState.h"

#include "Teams/SampleGameTeams.h"
#include "UnrealNetwork.h"


// Sets default values
ASGGameState::ASGGameState()
{
	PrimaryActorTick.bCanEverTick = false;

	SetReplicates(true);
}

void ASGGameState::AddKill(ESampleGameTeam Team, const FString& Killer)
{
	if (Team != ESampleGameTeam::Team_None && Team != ESampleGameTeam::Team_MAX)
	{
		if (FTeamScore* TeamScore = GetScoreForTeam(Team))
		{
			++TeamScore->TeamKills;

			bool bFoundPlayer = false;
			for (FPlayerScore& PlayerScore : TeamScore->TopPlayers)
			{
				if (PlayerScore.PlayerName.Compare(Killer) == 0)
				{
					++PlayerScore.Kills;
					bFoundPlayer = true;
					break;
				}
			}

			if (!bFoundPlayer)
			{
				FPlayerScore NewPlayerScore;
				NewPlayerScore.PlayerName = Killer;
				NewPlayerScore.Kills = 1;
				TeamScore->TopPlayers.Emplace(NewPlayerScore);
			}
		}
	}
}

void ASGGameState::RegisterScoreChangeListener(FSGTeamScoresUpdatedDelegate Callback)
{
	TeamScoresUpdatedCallback = Callback;
	Callback.ExecuteIfBound(TeamScores);
}

void ASGGameState::BeginPlay()
{
	if (HasAuthority() && TeamScores.Num() == 0)
	{
		// Initialize team scores for all possible teams.
		const uint32 TeamMax = static_cast<uint32>(ESampleGameTeam::Team_MAX);
		for (uint32 i = 0; i < TeamMax; ++i)
		{
			FTeamScore TeamScore;
			TeamScore.Team = static_cast<ESampleGameTeam>(i);
			TeamScore.TeamKills = 0;
			TeamScores.Add(TeamScore);
		}
	}
}

void ASGGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ASGGameState, TeamScores);
}

void ASGGameState::OnRep_TeamScores()
{
	const uint32 TeamMax = static_cast<uint32>(ESampleGameTeam::Team_MAX);
	for (uint32 i = 0; i < TeamMax; ++i)
	{
		UEnum* pEnum = FindObject<UEnum>(ANY_PACKAGE, TEXT("ESampleGameTeam"), true);
		FString TeamName = pEnum ? pEnum->GetNameStringByIndex(static_cast<uint8>(i)) : "null";
		int32 Kills = -1;
		if (FTeamScore* TeamScore = GetScoreForTeam(static_cast<ESampleGameTeam>(i)))
		{
			Kills = TeamScore->TeamKills;
		}
		UE_LOG(LogTemp, Log, TEXT("%s team score: %d"), *TeamName, Kills);
	}

	TeamScoresUpdatedCallback.ExecuteIfBound(TeamScores);
}

FTeamScore* ASGGameState::GetScoreForTeam(ESampleGameTeam Team)
{
	for (int i = 0; i < TeamScores.Num(); ++i)
	{
		if (TeamScores[i].Team == Team)
		{
			return &TeamScores[i];
		}
	}
	
	return nullptr;
}

