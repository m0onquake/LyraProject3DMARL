#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Templates/SubclassOf.h"
#include "TacticalRedBotSpawnPoint.generated.h"

class AAIController;
class APawn;
class ULyraExperienceDefinition;
class ULyraInventoryItemDefinition;
class UTacticalRedBotStateComponent;

UENUM(BlueprintType)
enum class ETacticalRedBotRole : uint8
{
    Rifleman,
    Sniper,
    AntiUAV
};

/**
 * Deterministic Lyra bot spawn point for tactical scenarios.
 * It suppresses the sample experience's generic bot count, spawns one bot at
 * this transform, forces it onto the red team, and grants its configured loadout.
 */
UCLASS(BlueprintType)
class LYRAGAME_API ATacticalRedBotSpawnPoint : public AActor
{
    GENERATED_BODY()

public:
    ATacticalRedBotSpawnPoint();

    virtual void BeginPlay() override;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    FName AgentId = TEXT("RED_RIFLE_01");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    ETacticalRedBotRole BotRole = ETacticalRedBotRole::Rifleman;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    int32 RedTeamId = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    TSoftClassPtr<ULyraInventoryItemDefinition> WeaponItemDefinition;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    TSoftClassPtr<AAIController> BotControllerClass;

    // Visual character part used when the stock Blueprint bot spawner has not
    // assigned cosmetics to this manually-created controller.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    TSoftClassPtr<AActor> CharacterPartClass;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot", meta = (ClampMin = "0.1"))
    float LoadoutDelay = 1.0f;

    UFUNCTION(BlueprintPure, Category = "Tactical Bot")
    AAIController* GetSpawnedController() const { return SpawnedController; }

    UFUNCTION(BlueprintCallable, Category = "Tactical Bot")
    bool ResetBotForEpisode();

    void ShowLauncherHitFeedback();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    bool bMissionObjective = false;

protected:
    UFUNCTION()
    void HandleExperienceLoaded(const ULyraExperienceDefinition* Experience);

    void SpawnConfiguredBot();
    void SetBotAwaitingEpisode(bool bAwaitingEpisode);
    void ApplyConfiguredLoadout();
    void EnsureVisibleCharacterPart(APawn* Pawn);
    void EnsureLauncherVehicleVisual(APawn* Pawn);
    void RestoreLauncherVehicleVisual();
    void ScheduleLoadoutRetry();
    void ConfigureSpawnedPawn(APawn* Pawn);
    FName GetRoleTag() const;

    UPROPERTY(Transient)
    TObjectPtr<AAIController> SpawnedController;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tactical Bot")
    TObjectPtr<UTacticalRedBotStateComponent> CombatState;

    FTimerHandle SpawnTimer;
    FTimerHandle LoadoutTimer;
    FTimerHandle DamageFeedbackTimer;
    int32 LoadoutRetryCount = 0;
};
