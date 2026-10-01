#include "TacticalAgentHealthComponent.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/DamageType.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalAgentHealth, Log, All);

UTacticalAgentHealthComponent::UTacticalAgentHealthComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetIsReplicatedByDefault(true);
}

void UTacticalAgentHealthComponent::BeginPlay()
{
    Super::BeginPlay();
    MaxHealth = FMath::Max(1.0f, MaxHealth);
    Health = MaxHealth;
    bDisabled = false;
    if (AActor* Owner = GetOwner())
    {
        Owner->OnTakeAnyDamage.AddDynamic(this, &ThisClass::HandleOwnerAnyDamage);
    }
}

void UTacticalAgentHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UTacticalAgentHealthComponent, Health);
    DOREPLIFETIME(UTacticalAgentHealthComponent, bDisabled);
    DOREPLIFETIME(UTacticalAgentHealthComponent, LastDamageSourceAgentId);
    DOREPLIFETIME(UTacticalAgentHealthComponent, LastHitDirection);
    DOREPLIFETIME(UTacticalAgentHealthComponent, DamageEventCount);
}

bool UTacticalAgentHealthComponent::ApplyAgentDamage(
    const float Damage,
    AActor* DamageSource,
    const FVector HitLocation)
{
    AActor* Owner = GetOwner();
    if (!Owner || !Owner->HasAuthority() || Damage <= 0.0f || bDisabled) return false;

    const float OldHealth = Health;
    Health = FMath::Clamp(Health - Damage, 0.0f, MaxHealth);
    LastDamageSourceAgentId = ResolveDamageSourceAgentId(DamageSource);
    const FVector Origin = HitLocation.IsNearlyZero() ? Owner->GetActorLocation() : HitLocation;
    LastHitDirection = DamageSource
        ? (Origin - DamageSource->GetActorLocation()).GetSafeNormal()
        : FVector::ZeroVector;
    ++DamageEventCount;
    OnHealthChanged.Broadcast(this, OldHealth, Health, DamageSource);

    UE_LOG(LogTacticalAgentHealth, Log,
        TEXT("%s damage=%.1f health=%.1f/%.1f source=%s events=%d"),
        *Owner->GetName(), Damage, Health, MaxHealth,
        *LastDamageSourceAgentId.ToString(), DamageEventCount);

    if (Health <= 0.0f)
    {
        bDisabled = true;
        OnDisabled.Broadcast(this, DamageSource);
        UE_LOG(LogTacticalAgentHealth, Warning, TEXT("%s disabled and retained in world"), *Owner->GetName());
    }
    Owner->ForceNetUpdate();
    return Health < OldHealth;
}

void UTacticalAgentHealthComponent::ResetHealth()
{
    AActor* Owner = GetOwner();
    if (!Owner || !Owner->HasAuthority()) return;
    const float OldHealth = Health;
    Health = FMath::Max(1.0f, MaxHealth);
    bDisabled = false;
    LastDamageSourceAgentId = NAME_None;
    LastHitDirection = FVector::ZeroVector;
    DamageEventCount = 0;
    OnHealthChanged.Broadcast(this, OldHealth, Health, nullptr);
    OnHealthReset.Broadcast(this);
    Owner->ForceNetUpdate();
}

FTacticalAgentHealthState UTacticalAgentHealthComponent::GetHealthState() const
{
    FTacticalAgentHealthState State;
    State.Health = Health;
    State.MaxHealth = MaxHealth;
    State.bAlive = IsAlive();
    State.bDisabled = bDisabled;
    State.LastDamageSourceAgentId = LastDamageSourceAgentId;
    State.LastHitDirection = LastHitDirection;
    State.DamageEventCount = DamageEventCount;
    return State;
}

UTacticalAgentHealthComponent* UTacticalAgentHealthComponent::FindHealthComponent(const AActor* Actor)
{
    return Actor ? Actor->FindComponentByClass<UTacticalAgentHealthComponent>() : nullptr;
}

void UTacticalAgentHealthComponent::HandleOwnerAnyDamage(
    AActor*,
    const float Damage,
    const UDamageType*,
    AController*,
    AActor* DamageCauser)
{
    ApplyAgentDamage(Damage, DamageCauser);
}

void UTacticalAgentHealthComponent::OnRep_Health(const float OldHealth)
{
    OnHealthChanged.Broadcast(this, OldHealth, Health, nullptr);
}

void UTacticalAgentHealthComponent::OnRep_Disabled()
{
    if (bDisabled) OnDisabled.Broadcast(this, nullptr);
}

FName UTacticalAgentHealthComponent::ResolveDamageSourceAgentId(const AActor* DamageSource) const
{
    if (!DamageSource) return TEXT("S1_TEST_DAMAGE");
    if (const FNameProperty* AgentIdProperty = FindFProperty<FNameProperty>(DamageSource->GetClass(), TEXT("AgentId")))
    {
        return AgentIdProperty->GetPropertyValue_InContainer(DamageSource);
    }
    return DamageSource->GetFName();
}
