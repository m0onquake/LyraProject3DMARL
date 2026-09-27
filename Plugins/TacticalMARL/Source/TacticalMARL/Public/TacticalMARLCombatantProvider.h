#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "TacticalMARLCombatantProvider.generated.h"

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalMARLCombatantState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Combatant") FName AgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") FName Role = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") TObjectPtr<AActor> Actor = nullptr;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") float Health = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") float MaxHealth = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") bool bAlive = false;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") bool bMissionObjective = false;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") int32 Deaths = 0;
    UPROPERTY(BlueprintReadOnly, Category="Combatant") FName LastKillerAgentId = NAME_None;
};

UINTERFACE(BlueprintType)
class TACTICALMARL_API UTacticalMARLCombatantProvider : public UInterface
{
    GENERATED_BODY()
};

/** Implemented by components that expose engine-specific health to TacticalMARL. */
class TACTICALMARL_API ITacticalMARLCombatantProvider
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Tactical MARL|Combatant")
    FTacticalMARLCombatantState GetCombatantState() const;

    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Tactical MARL|Combatant")
    bool ResetCombatant();

    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Tactical MARL|Combatant")
    bool ApplyMARLDamage(float Damage, AActor* DamageInstigator);
};
