// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/GameStateComponent.h"

#include "LyraBotCreationComponent.generated.h"

class ULyraExperienceDefinition;
class ULyraPawnData;
class AAIController;

UCLASS(Blueprintable, Abstract)
class ULyraBotCreationComponent : public UGameStateComponent
{
	GENERATED_BODY()

public:
	ULyraBotCreationComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * Prevent the experience's stock bots from being created when a map owns
	 * its AI roster (for example, TacticalMARL's deterministic red force).
	 * This is evaluated before editor and URL bot-count overrides.
	 */
	void SuppressAutomaticBotCreation() { bSuppressAutomaticBotCreation = true; }

	//~UActorComponent interface
	virtual void BeginPlay() override;
	//~End of UActorComponent interface

private:
	void OnExperienceLoaded(const ULyraExperienceDefinition* Experience);

protected:
	UPROPERTY(EditDefaultsOnly, Category=Teams)
	int32 NumBotsToCreate = 5;

	UPROPERTY(EditDefaultsOnly, Category=Gameplay)
	TSubclassOf<AAIController> BotControllerClass;

	UPROPERTY(EditDefaultsOnly, Category=Gameplay)
	TArray<FString> RandomBotNames;

	TArray<FString> RemainingBotNames;

protected:
	UPROPERTY(Transient)
	TArray<TObjectPtr<AAIController>> SpawnedBotList;

	bool bSuppressAutomaticBotCreation = false;

#if WITH_SERVER_CODE
public:
	void Cheat_AddBot() { SpawnOneBot(); }
	void Cheat_RemoveBot() { RemoveOneBot(); }

protected:
	virtual void ServerCreateBots();

	virtual void SpawnOneBot();
	virtual void RemoveOneBot();

	FString CreateBotName(int32 PlayerIndex);
#endif
};
