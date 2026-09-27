#pragma once

#include "Components/ActorComponent.h"
#include "GameplayEffect.h"
#include "TacticalMARLCombatantProvider.h"
#include "TacticalRedBotStateComponent.generated.h"

class APawn;
class ULyraHealthComponent;

/** Signed set-by-caller modifier used when a non-Lyra MARL agent changes health. */
UCLASS()
class LYRAGAME_API UTacticalMARLDirectHealthEffect : public UGameplayEffect
{
    GENERATED_BODY()

public:
    UTacticalMARLDirectHealthEffect();
};

/** Adapts Lyra Health/GAS state to the engine-independent TacticalMARL API. */
UCLASS(ClassGroup=(TacticalMARL), meta=(BlueprintSpawnableComponent))
class LYRAGAME_API UTacticalRedBotStateComponent : public UActorComponent, public ITacticalMARLCombatantProvider
{
    GENERATED_BODY()

public:
    UTacticalRedBotStateComponent();

    void Configure(FName InAgentId, FName InRole, bool bInMissionObjective);
    void BindToPawn(APawn* InPawn);
    void RestoreFullHealth();

    virtual FTacticalMARLCombatantState GetCombatantState_Implementation() const override;
    virtual bool ResetCombatant_Implementation() override;
    virtual bool ApplyMARLDamage_Implementation(float Damage, AActor* DamageInstigator) override;

private:
    UFUNCTION()
    void HandleHealthChanged(ULyraHealthComponent* Component, float OldValue, float NewValue, AActor* Instigator);

    UFUNCTION()
    void HandleDeathStarted(AActor* OwningActor);

    FName ResolveAgentId(const AActor* Actor) const;

    UPROPERTY(Transient) TObjectPtr<APawn> TrackedPawn;
    UPROPERTY(Transient) TObjectPtr<ULyraHealthComponent> HealthComponent;
    UPROPERTY(Transient) FName AgentId = NAME_None;
    UPROPERTY(Transient) FName Role = NAME_None;
    UPROPERTY(Transient) FName LastKillerAgentId = NAME_None;
    UPROPERTY(Transient) bool bMissionObjective = false;
    UPROPERTY(Transient) int32 Deaths = 0;
};
