// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SampleGamePlayerController.h"

#include "SampleGamePlayerState.h"
#include "SampleGameCharacter.h"
#include "SampleGameLogging.h"
#include "UI/SampleGameHUD.h"
#include "UI/SampleGameLoginUI.h"
#include "UI/SampleGameUI.h"

#include "SpatialNetDriver.h"


ASampleGamePlayerController::ASampleGamePlayerController()
	: SampleGameUI(nullptr)
	, RespawnCharacterDelay(5.0f)
	, DeleteCharacterDelay(15.0f)
	, PawnToDelete(nullptr)
{
	// Don't automatically switch the camera view when the pawn changes, to avoid weird camera jumps when a character dies.
	bAutoManageActiveCameraTarget = false;
}

void ASampleGamePlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
	GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
}

void ASampleGamePlayerController::UpdateHealthUI(int32 NewHealth, int32 MaxHealth)
{
	if (SampleGameUI != nullptr)
	{
		SampleGameUI->UpdateHealth(NewHealth, MaxHealth);
	}
	else
	{
		UE_LOG(LogSampleGame, Log, TEXT("Couldn't find SampleGameUI for controller: %s"), *this->GetName());
	}
}

void ASampleGamePlayerController::SetPawn(APawn* InPawn)
{
	Super::SetPawn(InPawn);

	if (GetNetMode() == NM_Client && bHasSubmittedLoginOptions)
	{
		SetPlayerUIVisible(InPawn != nullptr);

		ASampleGameCharacter* Character = Cast<ASampleGameCharacter>(InPawn);
		if (Character != nullptr)
		{
			UpdateHealthUI(Character->GetCurrentHealth(), Character->GetMaxHealth());

			// Make the new pawn's camera this controller's camera.
			SetViewTarget(InPawn);
		}
	}
}

void ASampleGamePlayerController::KillCharacter()
{
	check(GetNetMode() == NM_DedicatedServer);

	if (!HasAuthority())
	{
		return;
	}

	PawnToDelete = GetPawn();
	UnPossess();

	// TODO: timers won't persist across worker boundary migrations, and neither will PawnToDelete
	GetWorldTimerManager().SetTimer(DeleteCharacterTimerHandle, this, &ASampleGamePlayerController::DeleteCharacter, DeleteCharacterDelay);
	GetWorldTimerManager().SetTimer(RespawnTimerHandle, this, &ASampleGamePlayerController::RespawnCharacter, RespawnCharacterDelay);
}

void ASampleGamePlayerController::SetPlayerUIVisible(bool bIsVisible)
{
	check(GetNetMode() == NM_Client);

	ASampleGameHUD* HUD = Cast<ASampleGameHUD>(GetHUD());
	if (HUD != nullptr)
	{
		HUD->SetDrawCrosshair(bIsVisible);
	}

	if (bIsVisible)
	{
		if (SampleGameUI == nullptr)
		{
			check(UITemplate != nullptr);
			SampleGameUI = CreateWidget<USampleGameUI>(this, UITemplate);
			if (SampleGameUI == nullptr)
			{
				USpatialNetDriver* SpatialNetDriver = Cast<USpatialNetDriver>(GetNetDriver());
				UE_LOG(LogSampleGame, Error, TEXT("Failed to create UI for controller %s on worker %s"),
					*this->GetName(),
					SpatialNetDriver != nullptr ? *SpatialNetDriver->GetSpatialOS()->GetWorkerId() : TEXT("Invalid SpatialNetDriver"));
				return;
			}
		}

		if (!SampleGameUI->IsVisible())
		{
			SampleGameUI->AddToViewport();
		}
	}
	else
	{
		if (SampleGameUI != nullptr && SampleGameUI->IsVisible())
		{
			SampleGameUI->RemoveFromViewport();
		}
	}
}

void ASampleGamePlayerController::SetLoginUIVisible(bool bIsVisible)
{
	// Lazy instantiate the Login UI
	if (SampleGameLoginUI == nullptr)
	{
		check(LoginUIWidgetTemplate != nullptr);
		SampleGameLoginUI = CreateWidget<USampleGameLoginUI>(this, LoginUIWidgetTemplate);
		
		// Early out - Error case
		if (SampleGameLoginUI == nullptr)
		{
			USpatialNetDriver* SpatialNetDriver = Cast<USpatialNetDriver>(GetNetDriver());
			UE_LOG(LogSampleGame, Error, TEXT("Failed to create Login UI for controller %s on worker %s"),
				*this->GetName(),
				SpatialNetDriver != nullptr ? *SpatialNetDriver->GetSpatialOS()->GetWorkerId() : TEXT("Invalid SpatialNetDriver"));

			return;
		}
	}

	// Early out - If our visibility state is already set to the requested value, do nothing
	if (SampleGameLoginUI->IsVisible() == bIsVisible)
	{
		return;
	}

	if (bIsVisible)
	{
		// Show the Login UI
		SampleGameLoginUI->AddToViewport();
		// The UI Widget needs to know who its owner is, so it knows who to respond to when user submits final selections
		SampleGameLoginUI->SetOwnerPlayerController(this);
		// Set Mouse Cursor to SHOW, and only interact with the UI
		bShowMouseCursor = true;
		SetIgnoreLookInput(true);
		SetIgnoreMoveInput(true);
	}
	else
	{
		// Hide the Login UI
		SampleGameLoginUI->RemoveFromViewport();
		// Hide the Mouse Cursor, restore Look and Move control
		bShowMouseCursor = false;
		SetIgnoreLookInput(false);
		SetIgnoreMoveInput(false);
	}
}

void ASampleGamePlayerController::ServerTryJoinGame_Implementation(const FString& NewPlayerName, const ESampleGameTeam NewPlayerTeam)
{
	const FString CorrectedNewPlayerName = (NewPlayerName.IsEmpty() ? GetName() : NewPlayerName);
	bool bJoinWasSuccessful = true;

	// Validate PlayerState
	if (PlayerState == nullptr
		|| !PlayerState->IsA(ASampleGamePlayerState::StaticClass()))
	{
		bJoinWasSuccessful = false;

		UE_LOG(LogSampleGame, Error, TEXT("%s PlayerController: Invalid PlayerState pointer (%s)"), *this->GetName(), PlayerState == nullptr ? TEXT("nullptr") : *PlayerState->GetName());
	}

	// Validate the join request
	if (bHasSubmittedLoginOptions)
	{
		bJoinWasSuccessful = false;

		UE_LOG(LogSampleGame, Error, TEXT("%s PlayerController: Already submitted Join request.  Client attempting to join session multiple times."), *this->GetName());
	}

	// Inform Client as to whether or not join was accepted
	ClientJoinResults(bJoinWasSuccessful);
	
	if (bJoinWasSuccessful)
	{
		bHasSubmittedLoginOptions = true;

		// Set the player-selected values
		PlayerState->SetPlayerName(NewPlayerName);
		Cast<ASampleGamePlayerState>(PlayerState)->SetSelectedTeam(NewPlayerTeam);

		// Recalculate our PlayerStart using the new SelectedTeam information
		AActor* const NewStartSpot = GetWorld()->GetAuthGameMode()->ChoosePlayerStart(this);
		if (NewStartSpot != nullptr)
		{
			// Set the player controller / camera in this new location
			FRotator InitialControllerRot = NewStartSpot->GetActorRotation();
			InitialControllerRot.Roll = 0.f;
			SetInitialLocationAndRotation(NewStartSpot->GetActorLocation(), InitialControllerRot);
			StartSpot = NewStartSpot;
		}

		// Spawn the Pawn
		RespawnCharacter();
	}

}

bool ASampleGamePlayerController::ServerTryJoinGame_Validate(const FString& NewPlayerName, const ESampleGameTeam NewPlayerTeam)
{
	return true;
}

void ASampleGamePlayerController::ClientJoinResults_Implementation(const bool bJoinSucceeded)
{
	check(SampleGameLoginUI != nullptr);

	if (bJoinSucceeded)
	{
		bHasSubmittedLoginOptions = true;
		SetLoginUIVisible(false);
	}
	else
	{
		SampleGameLoginUI->JoinGameWasRejected();
	}
}

void ASampleGamePlayerController::RespawnCharacter()
{
	check(GetNetMode() == NM_DedicatedServer);
	AGameModeBase* GameMode = GetWorld()->GetAuthGameMode();
	if (GameMode != nullptr)
	{
		APawn* NewPawn = GameMode->SpawnDefaultPawnFor(this, StartSpot.Get());
		Possess(NewPawn);

		ASampleGameCharacter* NewCharacter = Cast<ASampleGameCharacter>(NewPawn);
		if (NewCharacter != nullptr)
		{
			ASampleGamePlayerState* SGPlayerState = Cast<ASampleGamePlayerState>(PlayerState);
			NewCharacter->SetTeam(SGPlayerState != nullptr ? SGPlayerState->GetSelectedTeam() : ESampleGameTeam::Team_None);
		}
	}
}

void ASampleGamePlayerController::DeleteCharacter()
{
	check(GetNetMode() == NM_DedicatedServer);
	if (PawnToDelete != nullptr)
	{
		// TODO: what if the character is on a different worker?
		GetWorld()->DestroyActor(PawnToDelete);
		PawnToDelete = nullptr;
	}
}

void ASampleGamePlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// HACK because sometimes (often?) Tick() runs (WAY) before BeginPlay(), or even before all the assigned-in-Blueprint variables have populated...
	// This appears to be an Unreal issue, not a GDK issue, as I ran into this in Vanilla Shooter as well.
	if (LoginUIWidgetTemplate != nullptr
		&& GetNetMode() == NM_Client
		&& Role == ROLE_AutonomousProxy
		&& !bHasShownLoginHud)
	{
		bHasShownLoginHud = true;
		SetLoginUIVisible(true);
	}
}
