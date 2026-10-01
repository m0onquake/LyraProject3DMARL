#pragma once

#include "Components/ActorComponent.h"
#include "TacticalAgentHealthComponent.generated.h"

class AController;
class UDamageType;
class UTacticalAgentHealthComponent;

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalAgentHealthState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") float Health = 100.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") float MaxHealth = 100.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") bool bAlive = true;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") bool bDisabled = false;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") FName LastDamageSourceAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") FVector LastHitDirection = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Health") int32 DamageEventCount = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
    FTacticalAgentHealthChanged,
    UTacticalAgentHealthComponent*, HealthComponent,
    float, OldHealth,
    float, NewHealth,
    AActor*, DamageSource);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FTacticalAgentDisabled,
    UTacticalAgentHealthComponent*, HealthComponent,
    AActor*, DamageSource);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FTacticalAgentHealthReset,
    UTacticalAgentHealthComponent*, HealthComponent);

/** Shared, resettable health and disabled-state implementation for blue agents. */
UCLASS(ClassGroup=(TacticalMARL), meta=(BlueprintSpawnableComponent))
class TACTICALMARL_API UTacticalAgentHealthComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UTacticalAgentHealthComponent();

    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Health")
    bool ApplyAgentDamage(float Damage, AActor* DamageSource = nullptr, FVector HitLocation = FVector::ZeroVector);

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Health")
    void ResetHealth();

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Health")
    FTacticalAgentHealthState GetHealthState() const;

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Health")
    bool IsAlive() const { return Health > 0.0f && !bDisabled; }

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Health")
    bool IsDisabled() const { return bDisabled; }

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Health")
    float GetHealth() const { return Health; }

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Health")
    float GetMaxHealth() const { return MaxHealth; }

    static UTacticalAgentHealthComponent* FindHealthComponent(const AActor* Actor);

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tactical MARL|Health", meta=(ClampMin="1.0"))
    float MaxHealth = 100.0f;

    UPROPERTY(BlueprintAssignable, Category="Tactical MARL|Health")
    FTacticalAgentHealthChanged OnHealthChanged;

    UPROPERTY(BlueprintAssignable, Category="Tactical MARL|Health")
    FTacticalAgentDisabled OnDisabled;

    UPROPERTY(BlueprintAssignable, Category="Tactical MARL|Health")
    FTacticalAgentHealthReset OnHealthReset;

private:
    UFUNCTION()
    void HandleOwnerAnyDamage(
        AActor* DamagedActor,
        float Damage,
        const UDamageType* DamageType,
        AController* InstigatedBy,
        AActor* DamageCauser);

    UFUNCTION()
    void OnRep_Health(float OldHealth);

    UFUNCTION()
    void OnRep_Disabled();

    FName ResolveDamageSourceAgentId(const AActor* DamageSource) const;

    UPROPERTY(ReplicatedUsing=OnRep_Health, VisibleAnywhere, Category="Tactical MARL|Health")
    float Health = 100.0f;

    UPROPERTY(ReplicatedUsing=OnRep_Disabled, VisibleAnywhere, Category="Tactical MARL|Health")
    bool bDisabled = false;

    UPROPERTY(Replicated, VisibleAnywhere, Category="Tactical MARL|Health")
    FName LastDamageSourceAgentId = NAME_None;

    UPROPERTY(Replicated, VisibleAnywhere, Category="Tactical MARL|Health")
    FVector LastHitDirection = FVector::ZeroVector;

    UPROPERTY(Replicated, VisibleAnywhere, Category="Tactical MARL|Health")
    int32 DamageEventCount = 0;
};
