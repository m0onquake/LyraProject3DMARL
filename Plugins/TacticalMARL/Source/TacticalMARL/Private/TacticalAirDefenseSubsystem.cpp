#include "TacticalAirDefenseSubsystem.h"

#include "TacticalAgentHealthComponent.h"
#include "TacticalMARLCombatantProvider.h"
#include "TacticalMARLEpisodeSubsystem.h"
#include "TacticalThreatSubsystem.h"
#include "TacticalUAVPawn.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalAirDefense, Log, All);

namespace
{
const FName LauncherId(TEXT("RED_ANTIUAV_01"));

bool IsS3CommandLineMode()
{
    return FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3Test")) ||
        FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3AutoDemo"));
}
}

void UTacticalAirDefenseSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ResetForEpisode(0);
}

void UTacticalAirDefenseSubsystem::Deinitialize()
{
    DestroyTransientVisuals();
    Super::Deinitialize();
}

bool UTacticalAirDefenseSubsystem::DoesSupportWorldType(const EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

TStatId UTacticalAirDefenseSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTacticalAirDefenseSubsystem, STATGROUP_Tickables);
}

bool UTacticalAirDefenseSubsystem::IsAirDefenseEnabled() const
{
    const IConsoleVariable* Difficulty = IConsoleManager::Get().FindConsoleVariable(TEXT("tacticalmarl.Difficulty"));
    return (Difficulty && Difficulty->GetInt() >= 2) || IsS3CommandLineMode();
}

void UTacticalAirDefenseSubsystem::ResetForEpisode(const int32 Seed)
{
    DestroyTransientVisuals();
    EpisodeSeed = Seed;
    RandomStream.Initialize(Seed ^ 0x3A17D2);
    Runtime = FTacticalAirDefenseState();
    Runtime.LauncherAgentId = LauncherId;
    Runtime.AmmoRemaining = AmmoCapacity;
    Runtime.State = IsAirDefenseEnabled() ? ETacticalAirDefenseState::Scanning : ETacticalAirDefenseState::Disabled;
    Runtime.LastTransitionReason = TEXT("episode_reset");
    EventSequence.Reset();
    EventSequence.Add(FString::Printf(TEXT("air_defense:%s"), *StateToString(Runtime.State)));
    AcceptanceStage = TEXT("none");
    LauncherActor.Reset();
    DamageSourceActor.Reset();
    CurrentTargetActor.Reset();
    RefreshLauncherActor();
    UE_LOG(LogTacticalAirDefense, Log,
        TEXT("S3 air defense reset seed=%d state=%s ammo=%d heat=%.1f"),
        EpisodeSeed, *StateToString(Runtime.State), Runtime.AmmoRemaining, Runtime.Heat);
}

void UTacticalAirDefenseSubsystem::Tick(const float DeltaTime)
{
    UpdateExplosionVisual(DeltaTime);
    if (!IsAirDefenseEnabled())
    {
        if (Runtime.State != ETacticalAirDefenseState::Disabled) SetState(ETacticalAirDefenseState::Disabled, TEXT("difficulty_below_d2"));
        return;
    }
    const UTacticalMARLEpisodeSubsystem* Episode = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>();
    if (!Episode || !Episode->IsEpisodeRunning()) return;

    if (!LauncherActor.IsValid()) RefreshLauncherActor();
    if (!LauncherActor.IsValid()) return;

    Runtime.StateElapsedSeconds += DeltaTime;
    Runtime.Heat = FMath::Max(0.0f, Runtime.Heat - HeatCoolPerSecond * DeltaTime);

    if (Runtime.State == ETacticalAirDefenseState::Launching)
    {
        UpdateProjectile(DeltaTime);
        return;
    }
    if (Runtime.State == ETacticalAirDefenseState::Cooldown)
    {
        Runtime.CooldownRemainingSeconds = FMath::Max(0.0f, Runtime.CooldownRemainingSeconds - DeltaTime);
        if (Runtime.CooldownRemainingSeconds <= 0.0f)
        {
            if (Runtime.AmmoRemaining <= 0)
            {
                SetState(ETacticalAirDefenseState::OutOfAmmo, TEXT("magazine_empty"));
            }
            else if (Runtime.Heat + HeatPerShot <= MaximumHeat)
            {
                ClearTarget();
                SetState(ETacticalAirDefenseState::Scanning, TEXT("cooldown_complete"));
            }
            else
            {
                Runtime.LastTransitionReason = TEXT("heat_limit_wait");
            }
        }
        return;
    }
    if (Runtime.State == ETacticalAirDefenseState::OutOfAmmo) return;
    if (Runtime.State == ETacticalAirDefenseState::LostLock)
    {
        if (Runtime.StateElapsedSeconds >= LostLockHoldDuration)
        {
            ClearTarget();
            SetState(ETacticalAirDefenseState::Scanning, TEXT("resume_search"));
        }
        return;
    }

    const FTacticalPerceivedContact* Contact = Runtime.TargetAgentId.IsNone()
        ? SelectPriorityContact() : FindCurrentContact();
    if (!Contact)
    {
        Runtime.bDirectLineOfSight = false;
        if (Runtime.State != ETacticalAirDefenseState::Scanning)
        {
            Runtime.bIncoming = false;
            Runtime.WarningRemainingSeconds = 0.0f;
            SetState(ETacticalAirDefenseState::LostLock, TEXT("direct_los_lost"));
        }
        return;
    }

    Runtime.bDirectLineOfSight = true;
    Runtime.TargetAgentId = Contact->TargetAgentId;
    Runtime.TargetRole = Contact->ObservedRole;
    Runtime.LastPerceivedLocation = Contact->ObservedLocation;
    CurrentTargetActor = Contact->TargetActor;

    switch (Runtime.State)
    {
    case ETacticalAirDefenseState::Disabled:
        SetState(ETacticalAirDefenseState::Scanning, TEXT("d2_enabled"));
        break;
    case ETacticalAirDefenseState::Scanning:
        Runtime.ConfirmationProgress = 0.0f;
        SetState(ETacticalAirDefenseState::Confirming, TEXT("priority_uav_visible"));
        break;
    case ETacticalAirDefenseState::Confirming:
        Runtime.ConfirmationProgress = FMath::Clamp(Runtime.StateElapsedSeconds / ConfirmationDuration, 0.0f, 1.0f);
        if (Runtime.ConfirmationProgress >= 1.0f)
        {
            Runtime.LockProgress = 0.0f;
            SetState(ETacticalAirDefenseState::Locking, TEXT("continuous_detection_confirmed"));
        }
        break;
    case ETacticalAirDefenseState::Locking:
        Runtime.LockProgress = FMath::Clamp(Runtime.StateElapsedSeconds / LockDuration, 0.0f, 1.0f);
        if (Runtime.LockProgress >= 1.0f)
        {
            Runtime.WarningRemainingSeconds = WarningDuration;
            SetState(ETacticalAirDefenseState::Warning, TEXT("lock_complete_warning_started"));
        }
        break;
    case ETacticalAirDefenseState::Warning:
        Runtime.WarningRemainingSeconds = FMath::Max(0.0f, WarningDuration - Runtime.StateElapsedSeconds);
        if (Runtime.WarningRemainingSeconds <= 0.0f) BeginLaunch(*Contact);
        break;
    default:
        break;
    }
}

void UTacticalAirDefenseSubsystem::RefreshLauncherActor()
{
    if (const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>())
    {
        if (const FTacticalThreatSensorState* Sensor = Threat->FindSensorState(LauncherId))
        {
            LauncherActor = Sensor->SensorActor;
        }
    }
    DamageSourceActor.Reset();
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UActorComponent*> Components;
        It->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component || !Component->GetClass()->ImplementsInterface(UTacticalMARLCombatantProvider::StaticClass())) continue;
            const FTacticalMARLCombatantState State = ITacticalMARLCombatantProvider::Execute_GetCombatantState(Component);
            if (State.AgentId == LauncherId)
            {
                LauncherActor = State.Actor;
                DamageSourceActor = Component->GetOwner();
                return;
            }
        }
    }
}

const FTacticalPerceivedContact* UTacticalAirDefenseSubsystem::SelectPriorityContact() const
{
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const FTacticalThreatSensorState* Sensor = Threat ? Threat->FindSensorState(LauncherId) : nullptr;
    if (!Sensor) return nullptr;
    const FTacticalPerceivedContact* Best = nullptr;
    int32 BestPriority = MIN_int32;
    for (const FTacticalPerceivedContact& Contact : Sensor->DirectContacts)
    {
        if (Contact.TargetType != TEXT("uav") || Contact.Distance > MaximumEngagementRange) continue;
        const UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Contact.TargetActor.Get());
        if (!Contact.TargetActor.IsValid() || (Health && !Health->IsAlive())) continue;
        const int32 Priority = GetRolePriority(Contact.ObservedRole);
        if (!Best || Priority > BestPriority ||
            (Priority == BestPriority && Contact.Distance < Best->Distance) ||
            (Priority == BestPriority && FMath::IsNearlyEqual(Contact.Distance, Best->Distance) &&
             Contact.TargetAgentId.LexicalLess(Best->TargetAgentId)))
        {
            Best = &Contact;
            BestPriority = Priority;
        }
    }
    return Best;
}

const FTacticalPerceivedContact* UTacticalAirDefenseSubsystem::FindCurrentContact() const
{
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const FTacticalThreatSensorState* Sensor = Threat ? Threat->FindSensorState(LauncherId) : nullptr;
    if (!Sensor) return nullptr;
    return Sensor->DirectContacts.FindByPredicate([this](const FTacticalPerceivedContact& Contact)
    {
        return Contact.TargetAgentId == Runtime.TargetAgentId && Contact.TargetType == TEXT("uav") &&
            Contact.Distance <= MaximumEngagementRange;
    });
}

int32 UTacticalAirDefenseSubsystem::GetRolePriority(const FName Role)
{
    const FString Value = Role.ToString();
    if (Value.Contains(TEXT("Strike"), ESearchCase::IgnoreCase)) return 400;
    if (Value.Contains(TEXT("Track"), ESearchCase::IgnoreCase) || Value.Contains(TEXT("CloseRecon"), ESearchCase::IgnoreCase)) return 300;
    if (Value.Contains(TEXT("WideRecon"), ESearchCase::IgnoreCase)) return 200;
    return 100;
}

void UTacticalAirDefenseSubsystem::SetState(const ETacticalAirDefenseState NewState, const FString& Reason)
{
    if (Runtime.State == NewState && Runtime.LastTransitionReason == Reason) return;
    Runtime.State = NewState;
    Runtime.StateElapsedSeconds = 0.0f;
    Runtime.LastTransitionReason = Reason;
    EventSequence.Add(FString::Printf(TEXT("air_defense:%s"), *StateToString(NewState)));
    UE_LOG(LogTacticalAirDefense, Display,
        TEXT("Air defense -> %s target=%s reason=%s ammo=%d heat=%.1f"),
        *StateToString(NewState), *Runtime.TargetAgentId.ToString(), *Reason,
        Runtime.AmmoRemaining, Runtime.Heat);
}

void UTacticalAirDefenseSubsystem::ClearTarget()
{
    Runtime.TargetAgentId = NAME_None;
    Runtime.TargetRole = NAME_None;
    Runtime.LastPerceivedLocation = FVector::ZeroVector;
    Runtime.IncomingDirection = FVector::ZeroVector;
    Runtime.bDirectLineOfSight = false;
    Runtime.bIncoming = false;
    Runtime.ConfirmationProgress = 0.0f;
    Runtime.LockProgress = 0.0f;
    Runtime.WarningRemainingSeconds = 0.0f;
    Runtime.EstimatedTimeToImpact = 0.0f;
    CurrentTargetActor.Reset();
}

float UTacticalAirDefenseSubsystem::ComputeHitProbability(const FTacticalPerceivedContact& Contact) const
{
    const AActor* Launcher = LauncherActor.Get();
    if (!Launcher) return 0.0f;
    const FVector ToTarget = Contact.ObservedLocation - Launcher->GetActorLocation();
    const FVector SightDirection = ToTarget.GetSafeNormal();
    const float DistanceFactor = FMath::Lerp(0.55f, 1.0f,
        1.0f - FMath::Clamp(Contact.Distance / MaximumEngagementRange, 0.0f, 1.0f));
    const float RelativeAltitude = Contact.ObservedLocation.Z - Launcher->GetActorLocation().Z;
    const float AltitudeFactor = FMath::Clamp(1.0f - FMath::Abs(RelativeAltitude - 900.0f) / 4200.0f, 0.55f, 1.0f);
    const float SpeedFactor = FMath::Lerp(1.0f, 0.55f,
        FMath::Clamp(Contact.ObservedVelocity.Size() / 1800.0f, 0.0f, 1.0f));
    const FVector LateralVelocity = Contact.ObservedVelocity - SightDirection * FVector::DotProduct(Contact.ObservedVelocity, SightDirection);
    float EvasionFactor = FMath::Lerp(1.0f, 0.35f,
        FMath::Clamp(LateralVelocity.Size() / 1200.0f, 0.0f, 1.0f));
    if (AcceptanceStage == TEXT("evade")) EvasionFactor *= 0.35f;
    return FMath::Clamp(BaseHitProbability * DistanceFactor * AltitudeFactor * SpeedFactor * EvasionFactor, 0.05f, 0.98f);
}

void UTacticalAirDefenseSubsystem::BeginLaunch(const FTacticalPerceivedContact& Contact)
{
    if (Runtime.AmmoRemaining <= 0)
    {
        SetState(ETacticalAirDefenseState::OutOfAmmo, TEXT("launch_blocked_no_ammo"));
        return;
    }
    if (Runtime.Heat + HeatPerShot > MaximumHeat)
    {
        Runtime.CooldownRemainingSeconds = 0.0f;
        SetState(ETacticalAirDefenseState::Cooldown, TEXT("launch_blocked_heat_limit"));
        return;
    }

    --Runtime.AmmoRemaining;
    Runtime.Heat = FMath::Min(MaximumHeat, Runtime.Heat + HeatPerShot);
    ++Runtime.ShotCount;
    Runtime.LastHitProbability = ComputeHitProbability(Contact);
    Runtime.LastRandomRoll = RandomStream.FRand();
    bPendingRandomHit = Runtime.LastRandomRoll <= Runtime.LastHitProbability;
    Runtime.LastShotResult = TEXT("in_flight");
    Runtime.bIncoming = true;
    Runtime.WarningRemainingSeconds = 0.0f;
    Runtime.EstimatedTimeToImpact = ProjectileTravelDuration;
    ProjectileElapsed = 0.0f;
    ProjectileStart = LauncherActor->GetActorLocation() + FVector(0.0f, 0.0f, 170.0f);
    ProjectileEnd = Contact.ObservedLocation + Contact.ObservedVelocity * ProjectileTravelDuration;
    Runtime.IncomingDirection = (ProjectileStart - Contact.ObservedLocation).GetSafeNormal();
    SpawnProjectileVisual(ProjectileStart);
    SetState(ETacticalAirDefenseState::Launching, TEXT("warning_complete_missile_launched"));
    UE_LOG(LogTacticalAirDefense, Display,
        TEXT("Shot %d target=%s probability=%.3f roll=%.3f distance=%.0f speed=%.0f altitude=%.0f"),
        Runtime.ShotCount, *Contact.TargetAgentId.ToString(), Runtime.LastHitProbability, Runtime.LastRandomRoll,
        Contact.Distance, Contact.ObservedVelocity.Size(), Contact.ObservedLocation.Z - LauncherActor->GetActorLocation().Z);
}

void UTacticalAirDefenseSubsystem::UpdateProjectile(const float DeltaTime)
{
    ProjectileElapsed += DeltaTime;
    Runtime.EstimatedTimeToImpact = FMath::Max(0.0f, ProjectileTravelDuration - ProjectileElapsed);
    const float Alpha = FMath::Clamp(ProjectileElapsed / ProjectileTravelDuration, 0.0f, 1.0f);
    const FVector Arc = FVector(0.0f, 0.0f, FMath::Sin(Alpha * PI) * 380.0f);
    const FVector Location = FMath::Lerp(ProjectileStart, ProjectileEnd, Alpha) + Arc;
    if (AStaticMeshActor* Projectile = ProjectileVisual.Get()) Projectile->SetActorLocation(Location);
    if (Alpha >= 1.0f) ResolveImpact();
}

bool UTacticalAirDefenseSubsystem::HasPhysicalLineOfSight(AActor* Target, FVector& OutTargetLocation) const
{
    AActor* Launcher = LauncherActor.Get();
    if (!Launcher || !Target) return false;
    OutTargetLocation = Target->GetActorLocation();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(TacticalAirDefenseImpactLOS), true, Launcher);
    if (ProjectileVisual.IsValid()) Params.AddIgnoredActor(ProjectileVisual.Get());
    if (ExplosionVisual.IsValid()) Params.AddIgnoredActor(ExplosionVisual.Get());
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        if (It->ActorHasTag(TEXT("MARL.Red"))) Params.AddIgnoredActor(*It);
    }
    FHitResult Hit;
    const FVector Start = Launcher->GetActorLocation() + FVector(0.0f, 0.0f, 110.0f);
    const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Start, OutTargetLocation, ECC_Visibility, Params);
    return !bHit || Hit.GetActor() == Target;
}

void UTacticalAirDefenseSubsystem::ResolveImpact()
{
    AActor* Target = CurrentTargetActor.Get();
    FVector ImpactLocation = ProjectileEnd;
    const bool bImpactLOS = HasPhysicalLineOfSight(Target, ImpactLocation);
    bool bAppliedDamage = false;
    if (bImpactLOS && bPendingRandomHit && Target)
    {
        if (UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Target))
        {
            bAppliedDamage = Health->ApplyAgentDamage(DamagePerHit,
                DamageSourceActor.IsValid() ? DamageSourceActor.Get() : LauncherActor.Get(), ImpactLocation);
        }
    }

    if (bAppliedDamage)
    {
        ++Runtime.HitCount;
        Runtime.LastShotResult = TEXT("hit");
    }
    else
    {
        ++Runtime.MissCount;
        Runtime.LastShotResult = !bImpactLOS ? TEXT("occluded_at_impact") :
            (Target ? TEXT("evaded_or_probability_miss") : TEXT("target_invalid"));
    }
    Runtime.bIncoming = false;
    Runtime.EstimatedTimeToImpact = 0.0f;
    SpawnExplosionVisual(ImpactLocation, bAppliedDamage);
    if (AStaticMeshActor* Projectile = ProjectileVisual.Get()) Projectile->Destroy();
    ProjectileVisual.Reset();
    Runtime.CooldownRemainingSeconds = CooldownDuration;
    UE_LOG(LogTacticalAirDefense, Display,
        TEXT("Shot %d resolved result=%s target=%s hits=%d misses=%d"),
        Runtime.ShotCount, *Runtime.LastShotResult, *Runtime.TargetAgentId.ToString(), Runtime.HitCount, Runtime.MissCount);
    SetState(ETacticalAirDefenseState::Cooldown, Runtime.LastShotResult);
}

void UTacticalAirDefenseSubsystem::SpawnProjectileVisual(const FVector& Start)
{
    if (AStaticMeshActor* Existing = ProjectileVisual.Get()) Existing->Destroy();
    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), AStaticMeshActor::StaticClass(), TEXT("TacticalMARL_S3_Missile"));
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.ObjectFlags |= RF_Transient;
    AStaticMeshActor* Projectile = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Start, FRotator::ZeroRotator, Params);
    if (!Projectile) return;
    UStaticMeshComponent* Mesh = Projectile->GetStaticMeshComponent();
    Mesh->SetMobility(EComponentMobility::Movable);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Projectile->SetActorScale3D(FVector(0.16f, 0.16f, 0.16f));
    Projectile->Tags.Add(TEXT("MARL.S3Projectile"));
    if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
    {
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Projectile);
        Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.08f, 0.01f));
        Mesh->SetMaterial(0, Material);
    }
    ProjectileVisual = Projectile;
}

void UTacticalAirDefenseSubsystem::SpawnExplosionVisual(const FVector& Location, const bool bHit)
{
    if (AStaticMeshActor* Existing = ExplosionVisual.Get()) Existing->Destroy();
    FActorSpawnParameters Params;
    Params.Name = MakeUniqueObjectName(GetWorld(), AStaticMeshActor::StaticClass(), TEXT("TacticalMARL_S3_Impact"));
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.ObjectFlags |= RF_Transient;
    AStaticMeshActor* Explosion = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
    if (!Explosion) return;
    UStaticMeshComponent* Mesh = Explosion->GetStaticMeshComponent();
    Mesh->SetMobility(EComponentMobility::Movable);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Explosion->SetActorScale3D(FVector(0.08f));
    Explosion->Tags.Add(TEXT("MARL.S3Impact"));
    if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
    {
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Explosion);
        Material->SetVectorParameterValue(TEXT("Color"), bHit ? FLinearColor(1.0f, 0.18f, 0.01f) : FLinearColor(1.0f, 0.8f, 0.05f));
        Mesh->SetMaterial(0, Material);
    }
    ExplosionVisual = Explosion;
    ExplosionElapsed = 0.0f;
}

void UTacticalAirDefenseSubsystem::UpdateExplosionVisual(const float DeltaTime)
{
    AStaticMeshActor* Explosion = ExplosionVisual.Get();
    if (!Explosion) return;
    ExplosionElapsed += DeltaTime;
    const float Alpha = FMath::Clamp(ExplosionElapsed / 0.75f, 0.0f, 1.0f);
    Explosion->SetActorScale3D(FVector(FMath::Lerp(0.08f, 0.95f, Alpha)));
    if (Alpha >= 1.0f)
    {
        Explosion->Destroy();
        ExplosionVisual.Reset();
    }
}

void UTacticalAirDefenseSubsystem::DestroyTransientVisuals()
{
    if (AStaticMeshActor* Projectile = ProjectileVisual.Get()) Projectile->Destroy();
    if (AStaticMeshActor* Explosion = ExplosionVisual.Get()) Explosion->Destroy();
    ProjectileVisual.Reset();
    ExplosionVisual.Reset();
}

FVector UTacticalAirDefenseSubsystem::GetProjectileLocation() const
{
    return ProjectileVisual.IsValid() ? ProjectileVisual->GetActorLocation() : FVector::ZeroVector;
}

bool UTacticalAirDefenseSubsystem::SetAcceptanceStage(const FString& Stage, FString& OutError)
{
    if (!IsS3CommandLineMode())
    {
        OutError = TEXT("s3_test_stage_disabled");
        return false;
    }
    UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    if (!Threat)
    {
        OutError = TEXT("threat_subsystem_unavailable");
        return false;
    }
    const FString Normalized = Stage.ToLower();
    const FString ThreatStage = (Normalized == TEXT("visible_hit") || Normalized == TEXT("evade"))
        ? TEXT("visible") : (Normalized == TEXT("lost_lock") || Normalized == TEXT("occluded"))
            ? TEXT("occluded") : FString();
    if (ThreatStage.IsEmpty())
    {
        OutError = TEXT("unsupported_s3_test_stage");
        return false;
    }
    if (!Threat->SetAcceptanceStage(ThreatStage, OutError)) return false;

    ATacticalUAVPawn* Target = nullptr;
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        if (It->AgentId == TEXT("BLUE_UAV_01")) { Target = *It; break; }
    }
    if (!Target)
    {
        OutError = TEXT("s3_acceptance_target_not_ready");
        return false;
    }
    FString CommandError;
    Target->ReceivePolicyJson(TEXT("{\"sequence_id\":3100,\"task\":\"idle\",\"role\":\"StrikeRole\"}"), CommandError);
    if (Normalized == TEXT("evade"))
    {
        RefreshLauncherActor();
        const FVector EvasionDirection = LauncherActor.IsValid()
            ? LauncherActor->GetActorRightVector().GetSafeNormal() : FVector::RightVector;
        Target->SubmitContinuousAction(EvasionDirection * 0.35f, 0.0f, 3101);
    }
    AcceptanceStage = Normalized;
    OutError.Reset();
    UE_LOG(LogTacticalAirDefense, Display, TEXT("S3 acceptance stage -> %s"), *AcceptanceStage);
    return true;
}

void UTacticalAirDefenseSubsystem::AppendAgentThreatFields(
    const TSharedRef<FJsonObject>& Agent, const FName AgentId) const
{
    TSharedRef<FJsonObject> Threat = MakeShared<FJsonObject>();
    const bool bTarget = !Runtime.TargetAgentId.IsNone() && Runtime.TargetAgentId == AgentId;
    const bool bDetected = bTarget && Runtime.State != ETacticalAirDefenseState::Scanning &&
        Runtime.State != ETacticalAirDefenseState::Disabled && Runtime.State != ETacticalAirDefenseState::OutOfAmmo;
    const bool bLocked = bTarget && (Runtime.State == ETacticalAirDefenseState::Locking ||
        Runtime.State == ETacticalAirDefenseState::Warning || Runtime.State == ETacticalAirDefenseState::Launching);
    Threat->SetBoolField(TEXT("detected"), bDetected);
    Threat->SetBoolField(TEXT("locked"), bLocked);
    Threat->SetBoolField(TEXT("incoming"), bTarget && Runtime.bIncoming);
    Threat->SetStringField(TEXT("source_agent_id"), bTarget ? Runtime.LauncherAgentId.ToString() : TEXT("None"));
    Threat->SetNumberField(TEXT("estimated_time_to_impact"), bTarget ? Runtime.EstimatedTimeToImpact : 0.0f);
    Threat->SetArrayField(TEXT("incoming_direction"), {
        MakeShared<FJsonValueNumber>(bTarget ? Runtime.IncomingDirection.X : 0.0),
        MakeShared<FJsonValueNumber>(bTarget ? Runtime.IncomingDirection.Y : 0.0),
        MakeShared<FJsonValueNumber>(bTarget ? Runtime.IncomingDirection.Z : 0.0)});
    Agent->SetObjectField(TEXT("threat"), Threat);
}

void UTacticalAirDefenseSubsystem::AppendAirDefenseFields(const TSharedRef<FJsonObject>& Root) const
{
    TSharedRef<FJsonObject> AirDefense = MakeShared<FJsonObject>();
    AirDefense->SetBoolField(TEXT("enabled"), IsAirDefenseEnabled());
    AirDefense->SetStringField(TEXT("difficulty"), IsAirDefenseEnabled() ? TEXT("D2") : TEXT("D0_D1"));
    AirDefense->SetNumberField(TEXT("seed"), EpisodeSeed);
    AirDefense->SetStringField(TEXT("state"), StateToString(Runtime.State));
    AirDefense->SetStringField(TEXT("launcher_agent_id"), Runtime.LauncherAgentId.ToString());
    AirDefense->SetStringField(TEXT("target_agent_id"), Runtime.TargetAgentId.ToString());
    AirDefense->SetStringField(TEXT("target_role"), Runtime.TargetRole.ToString());
    AirDefense->SetBoolField(TEXT("direct_los"), Runtime.bDirectLineOfSight);
    AirDefense->SetBoolField(TEXT("incoming"), Runtime.bIncoming);
    AirDefense->SetNumberField(TEXT("confirmation_progress"), Runtime.ConfirmationProgress);
    AirDefense->SetNumberField(TEXT("lock_progress"), Runtime.LockProgress);
    AirDefense->SetNumberField(TEXT("warning_remaining_seconds"), Runtime.WarningRemainingSeconds);
    AirDefense->SetNumberField(TEXT("estimated_time_to_impact"), Runtime.EstimatedTimeToImpact);
    AirDefense->SetNumberField(TEXT("cooldown_remaining_seconds"), Runtime.CooldownRemainingSeconds);
    AirDefense->SetNumberField(TEXT("ammo_remaining"), Runtime.AmmoRemaining);
    AirDefense->SetNumberField(TEXT("ammo_capacity"), AmmoCapacity);
    AirDefense->SetNumberField(TEXT("heat"), Runtime.Heat);
    AirDefense->SetNumberField(TEXT("maximum_heat"), MaximumHeat);
    AirDefense->SetNumberField(TEXT("shot_count"), Runtime.ShotCount);
    AirDefense->SetNumberField(TEXT("hit_count"), Runtime.HitCount);
    AirDefense->SetNumberField(TEXT("miss_count"), Runtime.MissCount);
    AirDefense->SetNumberField(TEXT("last_hit_probability"), Runtime.LastHitProbability);
    AirDefense->SetNumberField(TEXT("last_random_roll"), Runtime.LastRandomRoll);
    AirDefense->SetStringField(TEXT("last_shot_result"), Runtime.LastShotResult);
    AirDefense->SetStringField(TEXT("transition_reason"), Runtime.LastTransitionReason);
    AirDefense->SetStringField(TEXT("acceptance_stage"), AcceptanceStage);
    if (!Runtime.TargetAgentId.IsNone())
    {
        AirDefense->SetArrayField(TEXT("last_perceived_location"), {
            MakeShared<FJsonValueNumber>(Runtime.LastPerceivedLocation.X),
            MakeShared<FJsonValueNumber>(Runtime.LastPerceivedLocation.Y),
            MakeShared<FJsonValueNumber>(Runtime.LastPerceivedLocation.Z)});
    }
    else
    {
        AirDefense->SetField(TEXT("last_perceived_location"), MakeShared<FJsonValueNull>());
    }
    TArray<TSharedPtr<FJsonValue>> Events;
    for (const FString& Event : EventSequence) Events.Add(MakeShared<FJsonValueString>(Event));
    AirDefense->SetArrayField(TEXT("event_sequence"), Events);
    Root->SetObjectField(TEXT("red_air_defense"), AirDefense);
}

FString UTacticalAirDefenseSubsystem::StateToString(const ETacticalAirDefenseState State)
{
    switch (State)
    {
    case ETacticalAirDefenseState::Scanning: return TEXT("scanning");
    case ETacticalAirDefenseState::Confirming: return TEXT("confirming");
    case ETacticalAirDefenseState::Locking: return TEXT("locking");
    case ETacticalAirDefenseState::Warning: return TEXT("warning");
    case ETacticalAirDefenseState::Launching: return TEXT("launching");
    case ETacticalAirDefenseState::Cooldown: return TEXT("cooldown");
    case ETacticalAirDefenseState::LostLock: return TEXT("lost_lock");
    case ETacticalAirDefenseState::OutOfAmmo: return TEXT("out_of_ammo");
    default: return TEXT("disabled");
    }
}
