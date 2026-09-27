#include "TacticalMARLMissionSubsystem.h"

#include "TacticalUAVPawn.h"
#include "TacticalUGVPawn.h"

#include "Components/ActorComponent.h"
#include "EngineUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool UTacticalMARLMissionSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

bool UTacticalMARLMissionSubsystem::ResetMission()
{
    TArray<UObject*> Providers;
    TArray<FTacticalMARLCombatantState> Combatants;
    CollectCombatants(Providers, Combatants);
    if (Providers.Num() == 0) return false;
    // A Lyra phase transition may have already removed a provider's Pawn.
    // Give each provider a chance to rebuild it before validating GAS state.
    bool bAllProvidersReset = true;
    for (UObject* Provider : Providers)
    {
        bAllProvidersReset &= ITacticalMARLCombatantProvider::Execute_ResetCombatant(Provider);
    }
    Providers.Reset();
    Combatants.Reset();
    CollectCombatants(Providers, Combatants);
    if (!bAllProvidersReset || Combatants.ContainsByPredicate(
        [](const FTacticalMARLCombatantState& C) { return !C.Actor || C.MaxHealth <= 0.0f; }))
    {
        return false;
    }

    State = FTacticalMARLMissionState();
    PreviousHealth.Reset();
    PreviousAlive.Reset();
    KillsByAgent.Reset();
    PendingAgentRewards.Reset();
    VisitedCoverageCells.Reset();
    PendingTeamReward = 0.0f;
    LastTeamReward = 0.0f;
    PendingRewardComponents.Reset();
    LastRewardComponents.Reset();
    CandidateObservedSeconds = 0.0f;
    TimeSinceTargetObserved = 0.0f;
    bHadValidTrack = false;
    for (const FTacticalMARLCombatantState& Combatant : Combatants)
    {
        PreviousHealth.Add(Combatant.AgentId.ToString(), Combatant.Health);
        PreviousAlive.Add(Combatant.AgentId.ToString(), Combatant.bAlive);
        if (Combatant.bAlive) ++State.AliveHostiles;
    }
    // Initial deployment is the episode baseline, not exploration progress.
    UpdateCoverage();
    PendingTeamReward = 0.0f;
    PendingRewardComponents.Reset();
    return true;
}

void UTacticalMARLMissionSubsystem::AdvanceMissionStep(float DeltaSeconds)
{
    if (State.bMissionSuccess || State.bMissionFailure) return;
    AccumulateTeamReward(TEXT("step_cost"), RewardConfig.StepCost);
    UpdateCoverage();

    TArray<UObject*> Providers;
    TArray<FTacticalMARLCombatantState> Combatants;
    CollectCombatants(Providers, Combatants);
    UpdateCombatantRewards(Combatants);

    const bool bHasExplicitObjectives = Combatants.ContainsByPredicate(
        [](const FTacticalMARLCombatantState& C) { return C.bMissionObjective; });
    TSet<TWeakObjectPtr<AActor>> DetectedTargets;
    int32 ObserverCount = 0;
    CollectDetectedTargets(DetectedTargets, ObserverCount);
    bool bObjectiveObserved = false;
    int32 AliveObjectives = 0;
    int32 EffectiveObjectives = 0;
    State.AliveHostiles = 0;
    for (const FTacticalMARLCombatantState& Combatant : Combatants)
    {
        if (Combatant.bAlive) ++State.AliveHostiles;
        if (!IsObjective(Combatant, bHasExplicitObjectives)) continue;
        if (Combatant.bAlive) ++AliveObjectives;
        const float NeutralizedHealth = FMath::Max(1.0f, Combatant.MaxHealth * ObjectiveNeutralizedHealthFraction);
        if (Combatant.bAlive && Combatant.Health > NeutralizedHealth + KINDA_SMALL_NUMBER) ++EffectiveObjectives;
        if (Combatant.Actor && DetectedTargets.Contains(Combatant.Actor)) bObjectiveObserved = true;
    }

    if (bObjectiveObserved)
    {
        TimeSinceTargetObserved = 0.0f;
        CandidateObservedSeconds += DeltaSeconds;
        State.TrackQuality = FMath::Clamp(State.TrackQuality + DeltaSeconds * 0.5f + ObserverCount * 0.05f, 0.0f, 1.0f);
        if (!State.bCandidateFound)
        {
            State.bCandidateFound = true;
            State.Phase = ETacticalMARLMissionPhase::CandidateFound;
            AccumulateTeamReward(TEXT("first_detection"), RewardConfig.FirstDetection);
        }
        if (!State.bTargetConfirmed && CandidateObservedSeconds >= ConfirmationSeconds)
        {
            State.bTargetConfirmed = true;
            State.Phase = ETacticalMARLMissionPhase::TargetConfirmed;
            AccumulateTeamReward(TEXT("target_confirmed"), RewardConfig.TargetConfirmed);
        }
        if (State.bTargetConfirmed && !State.bTrackValid)
        {
            State.bTrackValid = true;
            State.Phase = ETacticalMARLMissionPhase::TrackEstablished;
            AccumulateTeamReward(
                bHadValidTrack ? TEXT("track_reacquired") : TEXT("track_established"),
                bHadValidTrack ? RewardConfig.TrackReacquired : RewardConfig.TrackEstablished);
            bHadValidTrack = true;
        }
        else if (State.bTrackValid)
        {
            AccumulateTeamReward(TEXT("track_maintained"), RewardConfig.TrackMaintained);
        }
        if (State.bTrackValid)
        {
            State.bStrikeReady = true;
            if (!State.bEngagementStarted) State.Phase = ETacticalMARLMissionPhase::StrikeReady;
        }
    }
    else
    {
        TimeSinceTargetObserved += DeltaSeconds;
        State.TrackQuality = FMath::Clamp(State.TrackQuality - DeltaSeconds * 0.5f, 0.0f, 1.0f);
        if (State.bTrackValid && TimeSinceTargetObserved >= TrackLossGraceSeconds)
        {
            State.bTrackValid = false;
            State.bStrikeReady = false;
            AccumulateTeamReward(TEXT("track_lost"), RewardConfig.TrackLost);
        }
    }

    if (Combatants.Num() > 0 && (AliveObjectives == 0 || EffectiveObjectives == 0))
    {
        if (State.ObjectiveKills == 0)
        {
            State.ObjectiveKills = 1;
            AccumulateTeamReward(TEXT("objective_neutralized"), RewardConfig.ObjectiveKill);
        }
        State.bEffectAchieved = true;
        State.bMissionSuccess = true;
        State.Phase = ETacticalMARLMissionPhase::Success;
        AccumulateTeamReward(TEXT("mission_success"), RewardConfig.MissionSuccess);
    }
    if (State.InvalidEngagements >= MaxInvalidEngagements)
    {
        State.bMissionFailure = true;
        State.Phase = ETacticalMARLMissionPhase::Failed;
    }
}

bool UTacticalMARLMissionSubsystem::ReportEngagement(
    FName AgentId,
    AActor* InstigatorActor,
    AActor* RequestedTarget,
    FVector EngagementLocation,
    float Damage,
    bool bApplyDamage)
{
    TArray<UObject*> Providers;
    TArray<FTacticalMARLCombatantState> Combatants;
    CollectCombatants(Providers, Combatants);
    const bool bHasExplicitObjectives = Combatants.ContainsByPredicate(
        [](const FTacticalMARLCombatantState& C) { return C.bMissionObjective; });
    int32 SelectedIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Combatants.Num(); ++Index)
    {
        if (Combatants[Index].Actor == RequestedTarget) { SelectedIndex = Index; break; }
    }
    if (SelectedIndex == INDEX_NONE)
    {
        float BestDistance = EngagementTargetTolerance;
        for (int32 Index = 0; Index < Combatants.Num(); ++Index)
        {
            if (!Combatants[Index].bAlive || !Combatants[Index].Actor || !IsObjective(Combatants[Index], bHasExplicitObjectives)) continue;
            const float Distance = FVector::Dist(EngagementLocation, Combatants[Index].Actor->GetActorLocation());
            if (Distance <= BestDistance) { BestDistance = Distance; SelectedIndex = Index; }
        }
    }

    const bool bAuthorized = State.bTargetConfirmed && State.bTrackValid && State.bStrikeReady &&
        SelectedIndex != INDEX_NONE && IsObjective(Combatants[SelectedIndex], bHasExplicitObjectives);
    State.bEngagementStarted = true;
    State.Phase = ETacticalMARLMissionPhase::Engagement;
    if (!bAuthorized)
    {
        ++State.InvalidEngagements;
        AccumulateTeamReward(TEXT("invalid_engagement"), RewardConfig.InvalidEngagement);
        PendingAgentRewards.FindOrAdd(AgentId.ToString()) += RewardConfig.InvalidEngagement;
        return false;
    }

    AccumulateTeamReward(TEXT("valid_engagement"), RewardConfig.ValidEngagement);
    PendingAgentRewards.FindOrAdd(AgentId.ToString()) += RewardConfig.ValidEngagement;
    if (bApplyDamage && Damage > 0.0f)
    {
        const FTacticalMARLCombatantState& Target = Combatants[SelectedIndex];
        const float HealthFloor = FMath::Max(1.0f, Target.MaxHealth * ObjectiveNeutralizedHealthFraction);
        const float SafeDamage = FMath::Min(Damage, FMath::Max(0.0f, Target.Health - HealthFloor));
        if (SafeDamage > 0.0f)
        {
            ITacticalMARLCombatantProvider::Execute_ApplyMARLDamage(Providers[SelectedIndex], SafeDamage, InstigatorActor);
        }
    }
    State.Phase = ETacticalMARLMissionPhase::EffectAssessment;
    return true;
}

float UTacticalMARLMissionSubsystem::ConsumeTeamReward()
{
    const float Result = PendingTeamReward;
    LastTeamReward = Result;
    LastRewardComponents = PendingRewardComponents;
    PendingTeamReward = 0.0f;
    PendingRewardComponents.Reset();
    return Result;
}

void UTacticalMARLMissionSubsystem::ConsumeAgentRewards(TMap<FString, float>& OutRewards)
{
    OutRewards = MoveTemp(PendingAgentRewards);
    PendingAgentRewards.Reset();
}

FString UTacticalMARLMissionSubsystem::GetMissionJson() const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    AppendMissionFields(Root);
    FString Result;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

void UTacticalMARLMissionSubsystem::AppendMissionFields(const TSharedRef<FJsonObject>& Root) const
{
    TSharedRef<FJsonObject> Mission = MakeShared<FJsonObject>();
    Mission->SetStringField(TEXT("phase"), PhaseToString(State.Phase));
    Mission->SetBoolField(TEXT("candidate_found"), State.bCandidateFound);
    Mission->SetBoolField(TEXT("target_confirmed"), State.bTargetConfirmed);
    Mission->SetBoolField(TEXT("track_valid"), State.bTrackValid);
    Mission->SetNumberField(TEXT("track_quality"), State.TrackQuality);
    Mission->SetBoolField(TEXT("strike_ready"), State.bStrikeReady);
    Mission->SetBoolField(TEXT("engagement_started"), State.bEngagementStarted);
    Mission->SetBoolField(TEXT("effect_achieved"), State.bEffectAchieved);
    Mission->SetBoolField(TEXT("success"), State.bMissionSuccess);
    Mission->SetBoolField(TEXT("failure"), State.bMissionFailure);
    Mission->SetNumberField(TEXT("alive_hostiles"), State.AliveHostiles);
    Mission->SetNumberField(TEXT("objective_kills"), State.ObjectiveKills);
    Mission->SetNumberField(TEXT("invalid_engagements"), State.InvalidEngagements);
    Mission->SetStringField(TEXT("target_zone"), State.TargetZone.ToString());
    Mission->SetNumberField(TEXT("last_team_reward"), LastTeamReward);
    TSharedRef<FJsonObject> RewardComponents = MakeShared<FJsonObject>();
    for (const TPair<FString, float>& Pair : LastRewardComponents)
    {
        RewardComponents->SetNumberField(Pair.Key, Pair.Value);
    }
    Mission->SetObjectField(TEXT("reward_components"), RewardComponents);

    TArray<UObject*> Providers;
    TArray<FTacticalMARLCombatantState> Combatants;
    CollectCombatants(Providers, Combatants);
    TArray<TSharedPtr<FJsonValue>> TargetArray;
    for (const FTacticalMARLCombatantState& Combatant : Combatants)
    {
        TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
        Target->SetStringField(TEXT("agent_id"), Combatant.AgentId.ToString());
        Target->SetStringField(TEXT("role"), Combatant.Role.ToString());
        Target->SetStringField(
            TEXT("visual_type"),
            Combatant.Role == TEXT("MARL.Role.AntiUAV") ? TEXT("launcher_vehicle") : TEXT("infantry"));
        Target->SetNumberField(TEXT("health"), Combatant.Health);
        Target->SetNumberField(TEXT("max_health"), Combatant.MaxHealth);
        Target->SetBoolField(TEXT("alive"), Combatant.bAlive);
        Target->SetBoolField(TEXT("mission_objective"), Combatant.bMissionObjective);
        Target->SetNumberField(TEXT("deaths"), Combatant.Deaths);
        Target->SetStringField(TEXT("last_killer_agent_id"), Combatant.LastKillerAgentId.ToString());
        if (Combatant.Actor)
        {
            const FVector Location = Combatant.Actor->GetActorLocation();
            Target->SetArrayField(TEXT("location"), {
                MakeShared<FJsonValueNumber>(Location.X),
                MakeShared<FJsonValueNumber>(Location.Y),
                MakeShared<FJsonValueNumber>(Location.Z)});
        }
        TargetArray.Add(MakeShared<FJsonValueObject>(Target));
    }
    Mission->SetArrayField(TEXT("targets"), TargetArray);

    TSharedRef<FJsonObject> Kills = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : KillsByAgent) Kills->SetNumberField(Pair.Key, Pair.Value);
    Mission->SetObjectField(TEXT("kills_by_agent"), Kills);
    Root->SetObjectField(TEXT("mission"), Mission);
}

void UTacticalMARLMissionSubsystem::CollectCombatants(TArray<UObject*>& OutProviders, TArray<FTacticalMARLCombatantState>& OutStates) const
{
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UActorComponent*> Components;
        It->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (Component && Component->GetClass()->ImplementsInterface(UTacticalMARLCombatantProvider::StaticClass()))
            {
                OutProviders.Add(Component);
                OutStates.Add(ITacticalMARLCombatantProvider::Execute_GetCombatantState(Component));
            }
        }
    }
}

void UTacticalMARLMissionSubsystem::CollectDetectedTargets(TSet<TWeakObjectPtr<AActor>>& OutTargets, int32& OutObservers) const
{
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        bool bObserved = false;
        for (AActor* Actor : It->GetDetectedActors())
        {
            if (Actor && Actor->ActorHasTag(TEXT("MARL.Red"))) { OutTargets.Add(Actor); bObserved = true; }
        }
        if (bObserved) ++OutObservers;
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        bool bObserved = false;
        for (AActor* Actor : It->GetDetectedActors())
        {
            if (Actor && Actor->ActorHasTag(TEXT("MARL.Red"))) { OutTargets.Add(Actor); bObserved = true; }
        }
        if (bObserved) ++OutObservers;
    }
}

void UTacticalMARLMissionSubsystem::UpdateCoverage()
{
    int32 NewCells = 0;
    auto Visit = [this, &NewCells](const FVector& Location)
    {
        const FIntPoint Cell(FMath::FloorToInt(Location.X / CoverageCellSize), FMath::FloorToInt(Location.Y / CoverageCellSize));
        if (!VisitedCoverageCells.Contains(Cell)) { VisitedCoverageCells.Add(Cell); ++NewCells; }
    };
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It) Visit(It->GetActorLocation());
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It) Visit(It->GetActorLocation());
    AccumulateTeamReward(
        NewCells > 0 ? TEXT("new_coverage") : TEXT("repeated_search"),
        NewCells > 0 ? NewCells * RewardConfig.NewCoverageCell : RewardConfig.RepeatedSearchCost);
}

void UTacticalMARLMissionSubsystem::UpdateCombatantRewards(const TArray<FTacticalMARLCombatantState>& Combatants)
{
    const bool bHasExplicitObjectives = Combatants.ContainsByPredicate(
        [](const FTacticalMARLCombatantState& C) { return C.bMissionObjective; });
    for (const FTacticalMARLCombatantState& Combatant : Combatants)
    {
        const FString Id = Combatant.AgentId.ToString();
        const float OldHealth = PreviousHealth.Contains(Id) ? PreviousHealth[Id] : Combatant.Health;
        const bool bWasAlive = PreviousAlive.Contains(Id) ? PreviousAlive[Id] : Combatant.bAlive;
        const float DamageFraction = Combatant.MaxHealth > 0.0f
            ? FMath::Max(0.0f, OldHealth - Combatant.Health) / Combatant.MaxHealth : 0.0f;
        if (DamageFraction > 0.0f)
        {
            const float Reward = DamageFraction * RewardConfig.FullHealthDamage;
            AccumulateTeamReward(TEXT("damage"), Reward);
            PendingAgentRewards.FindOrAdd(Combatant.LastKillerAgentId.ToString()) += Reward;
        }
        if (bWasAlive && !Combatant.bAlive && IsObjective(Combatant, bHasExplicitObjectives))
        {
            ++State.ObjectiveKills;
            AccumulateTeamReward(TEXT("objective_kill"), RewardConfig.ObjectiveKill);
            const FString Killer = Combatant.LastKillerAgentId.ToString();
            KillsByAgent.FindOrAdd(Killer)++;
            PendingAgentRewards.FindOrAdd(Killer) += RewardConfig.ObjectiveKill;
        }
        PreviousHealth.FindOrAdd(Id) = Combatant.Health;
        PreviousAlive.FindOrAdd(Id) = Combatant.bAlive;
    }
}

void UTacticalMARLMissionSubsystem::AccumulateTeamReward(const FString& Component, float Value)
{
    PendingTeamReward += Value;
    PendingRewardComponents.FindOrAdd(Component) += Value;
}

bool UTacticalMARLMissionSubsystem::IsObjective(const FTacticalMARLCombatantState& Combatant, bool bHasExplicitObjectives) const
{
    return bHasExplicitObjectives ? Combatant.bMissionObjective : true;
}

FString UTacticalMARLMissionSubsystem::PhaseToString(ETacticalMARLMissionPhase Phase)
{
    switch (Phase)
    {
    case ETacticalMARLMissionPhase::CandidateFound: return TEXT("candidate_found");
    case ETacticalMARLMissionPhase::TargetConfirmed: return TEXT("target_confirmed");
    case ETacticalMARLMissionPhase::TrackEstablished: return TEXT("track_established");
    case ETacticalMARLMissionPhase::StrikeReady: return TEXT("strike_ready");
    case ETacticalMARLMissionPhase::Engagement: return TEXT("engagement");
    case ETacticalMARLMissionPhase::EffectAssessment: return TEXT("effect_assessment");
    case ETacticalMARLMissionPhase::Success: return TEXT("success");
    case ETacticalMARLMissionPhase::Failed: return TEXT("failed");
    default: return TEXT("search");
    }
}
