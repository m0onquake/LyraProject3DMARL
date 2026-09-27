#include "AI/TacticalRedBotStateComponent.h"

#include "AI/TacticalRedBotSpawnPoint.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "Character/LyraHealthComponent.h"
#include "GameplayEffect.h"
#include "LyraGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(TacticalRedBotStateComponent)

UTacticalMARLDirectHealthEffect::UTacticalMARLDirectHealthEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Instant;
    FSetByCallerFloat SetByCaller;
    SetByCaller.DataTag = LyraGameplayTags::SetByCaller_Damage;
    FGameplayModifierInfo Modifier;
    Modifier.Attribute = ULyraHealthSet::GetHealthAttribute();
    Modifier.ModifierOp = EGameplayModOp::Additive;
    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(SetByCaller);
    Modifiers.Add(Modifier);
}

UTacticalRedBotStateComponent::UTacticalRedBotStateComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UTacticalRedBotStateComponent::Configure(FName InAgentId, FName InRole, bool bInMissionObjective)
{
    AgentId = InAgentId;
    Role = InRole;
    bMissionObjective = bInMissionObjective;
    Deaths = 0;
}

void UTacticalRedBotStateComponent::BindToPawn(APawn* InPawn)
{
    if (HealthComponent)
    {
        HealthComponent->OnHealthChanged.RemoveAll(this);
        HealthComponent->OnDeathStarted.RemoveAll(this);
    }
    TrackedPawn = InPawn;
    HealthComponent = ULyraHealthComponent::FindHealthComponent(InPawn);
    LastKillerAgentId = NAME_None;
    if (HealthComponent)
    {
        HealthComponent->OnHealthChanged.AddDynamic(this, &ThisClass::HandleHealthChanged);
        HealthComponent->OnDeathStarted.AddDynamic(this, &ThisClass::HandleDeathStarted);
    }
}

void UTacticalRedBotStateComponent::RestoreFullHealth()
{
    if (!TrackedPawn || !HealthComponent || HealthComponent->IsDeadOrDying()) return;
    UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(TrackedPawn);
    if (!ASC) return;
    FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UTacticalMARLDirectHealthEffect::StaticClass(), 1.0f, ASC->MakeEffectContext());
    if (Spec.IsValid())
    {
        Spec.Data->SetSetByCallerMagnitude(LyraGameplayTags::SetByCaller_Damage, HealthComponent->GetMaxHealth());
        ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
    }
}

FTacticalMARLCombatantState UTacticalRedBotStateComponent::GetCombatantState_Implementation() const
{
    FTacticalMARLCombatantState State;
    State.AgentId = AgentId;
    State.Role = Role;
    State.Actor = TrackedPawn;
    State.Health = HealthComponent ? HealthComponent->GetHealth() : 0.0f;
    State.MaxHealth = HealthComponent ? HealthComponent->GetMaxHealth() : 0.0f;
    State.bAlive = HealthComponent && !HealthComponent->IsDeadOrDying() && State.Health > 0.0f;
    State.bMissionObjective = bMissionObjective;
    State.Deaths = Deaths;
    State.LastKillerAgentId = LastKillerAgentId;
    return State;
}

bool UTacticalRedBotStateComponent::ResetCombatant_Implementation()
{
    if (ATacticalRedBotSpawnPoint* SpawnPoint = Cast<ATacticalRedBotSpawnPoint>(GetOwner()))
    {
        return SpawnPoint->ResetBotForEpisode();
    }
    return false;
}

bool UTacticalRedBotStateComponent::ApplyMARLDamage_Implementation(float Damage, AActor* DamageInstigator)
{
    if (Damage <= 0.0f || !TrackedPawn || !HealthComponent || HealthComponent->IsDeadOrDying()) return false;
    UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(TrackedPawn);
    if (!ASC) return false;
    FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
    Context.AddInstigator(DamageInstigator, DamageInstigator);
    FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UTacticalMARLDirectHealthEffect::StaticClass(), 1.0f, Context);
    if (!Spec.IsValid()) return false;
    LastKillerAgentId = ResolveAgentId(DamageInstigator);
    Spec.Data->SetSetByCallerMagnitude(LyraGameplayTags::SetByCaller_Damage, -Damage);
    ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
    return true;
}

void UTacticalRedBotStateComponent::HandleHealthChanged(ULyraHealthComponent*, float OldValue, float NewValue, AActor* Instigator)
{
    if (Instigator) LastKillerAgentId = ResolveAgentId(Instigator);
    if (NewValue < OldValue)
    {
        if (ATacticalRedBotSpawnPoint* SpawnPoint = Cast<ATacticalRedBotSpawnPoint>(GetOwner()))
        {
            SpawnPoint->ShowLauncherHitFeedback();
        }
    }
}

void UTacticalRedBotStateComponent::HandleDeathStarted(AActor*)
{
    ++Deaths;
}

FName UTacticalRedBotStateComponent::ResolveAgentId(const AActor* Actor) const
{
    if (!Actor) return NAME_None;
    if (const FNameProperty* AgentIdProperty = FindFProperty<FNameProperty>(Actor->GetClass(), TEXT("AgentId")))
    {
        return AgentIdProperty->GetPropertyValue_InContainer(Actor);
    }
    return Actor->GetFName();
}
