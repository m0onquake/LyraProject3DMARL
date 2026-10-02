#include "TacticalMARLEpisodeSubsystem.h"

#include "TacticalAirDefenseSubsystem.h"
#include "TacticalMARLMissionSubsystem.h"
#include "TacticalThreatSubsystem.h"
#include "TacticalUAVPawn.h"
#include "TacticalUGVPawn.h"

#include "EngineUtils.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Math/UnrealMathUtility.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalEpisode, Log, All);

namespace
{
struct FAgentMetric
{
    FString Id;
    FString TaskState;
    int32 DetectionCount = 0;
    AActor* Actor = nullptr;
};

void CollectBlueAgentMetrics(UWorld* World, TArray<FAgentMetric>& OutMetrics)
{
    for (TActorIterator<ATacticalUAVPawn> It(World); It; ++It)
    {
        const FTacticalUAVTelemetry Telemetry = It->GetTelemetry();
        FString TaskState = TEXT("idle");
        if (Telemetry.TaskState == ETacticalUAVTaskState::Completed) TaskState = TEXT("completed");
        else if (Telemetry.TaskState == ETacticalUAVTaskState::Failed) TaskState = TEXT("failed");
        else if (Telemetry.TaskState == ETacticalUAVTaskState::Executing) TaskState = TEXT("executing");
        else if (Telemetry.TaskState == ETacticalUAVTaskState::EnRoute) TaskState = TEXT("en_route");
        OutMetrics.Add({Telemetry.AgentId.ToString(), TaskState, Telemetry.DetectedTargetCount, *It});
    }

    for (TActorIterator<ATacticalUGVPawn> It(World); It; ++It)
    {
        const FTacticalUGVTelemetry Telemetry = It->GetTelemetry();
        FString TaskState = TEXT("idle");
        if (Telemetry.TaskState == ETacticalUGVTaskState::Completed) TaskState = TEXT("completed");
        else if (Telemetry.TaskState == ETacticalUGVTaskState::Failed) TaskState = TEXT("failed");
        else if (Telemetry.TaskState == ETacticalUGVTaskState::Executing) TaskState = TEXT("executing");
        else if (Telemetry.TaskState == ETacticalUGVTaskState::EnRoute) TaskState = TEXT("en_route");
        OutMetrics.Add({Telemetry.AgentId.ToString(), TaskState, Telemetry.DetectedTargetCount, *It});
    }
}
}

void UTacticalMARLEpisodeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

bool UTacticalMARLEpisodeSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

TStatId UTacticalMARLEpisodeSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTacticalMARLEpisodeSubsystem, STATGROUP_Tickables);
}

void UTacticalMARLEpisodeSubsystem::Tick(float DeltaTime)
{
    if (!IsEpisodeRunning()) return;
    State.ElapsedSeconds = GetElapsedSeconds();
    if (State.ElapsedSeconds >= Config.MaxEpisodeSeconds)
    {
        EndEpisode(TEXT("time_limit"), true);
    }
}

bool UTacticalMARLEpisodeSubsystem::ResetEpisode(int32 Seed)
{
    if (!GetWorld() || GetWorld()->GetNetMode() == NM_Client) return false;
    // Keep engine-side procedural behavior repeatable for evaluation.  The
    // external scripted policy owns a separate Python RNG with the same seed.
    FMath::RandInit(Seed);
    CaptureAgentStartStates();
    ResetActors();
    bool bMissionReady = true;
    if (UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        bMissionReady = Mission->ResetMission();
    }
    if (UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>())
    {
        Threat->ResetForEpisode(Seed);
    }
    if (UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>())
    {
        AirDefense->ResetForEpisode(Seed);
    }

    State = FTacticalMARLEpisodeState();
    State.EpisodeId = NextEpisodeId++;
    State.Seed = Seed;
    State.Phase = ETacticalMARLEpisodePhase::Running;
    EpisodeStartWorldSeconds = GetWorld()->GetTimeSeconds();
    LastAdvanceWorldSeconds = EpisodeStartWorldSeconds;
    Rewards.Reset();
    Terminations.Reset();
    Truncations.Reset();
    CaptureBaselineMetrics();
    UE_LOG(LogTacticalEpisode, Log, TEXT("Episode %d reset (seed=%d, agents=%d)"), State.EpisodeId, Seed, PreviousDetectionCounts.Num());
    if (PreviousDetectionCounts.Num() == 0)
    {
        State.Phase = ETacticalMARLEpisodePhase::Terminated;
        State.EndReason = TEXT("no_controllable_agents");
        return false;
    }
    if (!bMissionReady)
    {
        State.Phase = ETacticalMARLEpisodePhase::Terminated;
        State.EndReason = TEXT("combatants_not_ready");
        return false;
    }
    return true;
}

bool UTacticalMARLEpisodeSubsystem::AdvanceEpisode()
{
    if (!IsEpisodeRunning()) return false;
    ++State.Step;
    State.ElapsedSeconds = GetElapsedSeconds();
    if (UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        const double Now = GetWorld()->GetTimeSeconds();
        Mission->AdvanceMissionStep(FMath::Max(0.0, Now - LastAdvanceWorldSeconds));
        LastAdvanceWorldSeconds = Now;
    }
    ComputeRewardsAndDoneFlags();
    if (State.Step >= Config.MaxSteps && IsEpisodeRunning())
    {
        EndEpisode(TEXT("step_limit"), true);
    }
    return true;
}

void UTacticalMARLEpisodeSubsystem::EndEpisode(const FString& Reason, bool bTruncated)
{
    if (!IsEpisodeRunning()) return;
    State.ElapsedSeconds = GetElapsedSeconds();
    State.Phase = bTruncated ? ETacticalMARLEpisodePhase::Truncated : ETacticalMARLEpisodePhase::Terminated;
    State.EndReason = Reason;
    for (TPair<FString, bool>& Pair : bTruncated ? Truncations : Terminations)
    {
        Pair.Value = true;
    }
    UE_LOG(LogTacticalEpisode, Log, TEXT("Episode %d ended: %s at step %d"), State.EpisodeId, *Reason, State.Step);
}

FTacticalMARLEpisodeState UTacticalMARLEpisodeSubsystem::GetEpisodeState() const
{
    FTacticalMARLEpisodeState Result = State;
    if (IsEpisodeRunning()) Result.ElapsedSeconds = GetElapsedSeconds();
    return Result;
}

FString UTacticalMARLEpisodeSubsystem::GetEpisodeJson() const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    AppendStepFields(Root);
    FString Result;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

void UTacticalMARLEpisodeSubsystem::AppendStepFields(const TSharedRef<FJsonObject>& Root) const
{
    const FTacticalMARLEpisodeState Current = GetEpisodeState();
    TSharedRef<FJsonObject> Episode = MakeShared<FJsonObject>();
    Episode->SetNumberField(TEXT("episode_id"), Current.EpisodeId);
    Episode->SetNumberField(TEXT("seed"), Current.Seed);
    Episode->SetNumberField(TEXT("step"), Current.Step);
    Episode->SetNumberField(TEXT("elapsed_seconds"), Current.ElapsedSeconds);
    Episode->SetStringField(TEXT("phase"), PhaseToString(Current.Phase));
    Episode->SetNumberField(TEXT("max_steps"), Config.MaxSteps);
    Episode->SetNumberField(TEXT("max_seconds"), Config.MaxEpisodeSeconds);
    if (!Current.EndReason.IsEmpty()) Episode->SetStringField(TEXT("end_reason"), Current.EndReason);
    Root->SetObjectField(TEXT("episode"), Episode);

    TSharedRef<FJsonObject> RewardObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> TerminationObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> TruncationObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> InfoObject = MakeShared<FJsonObject>();
    for (const TPair<FString, float>& Pair : Rewards) RewardObject->SetNumberField(Pair.Key, Pair.Value);
    for (const TPair<FString, bool>& Pair : Terminations) TerminationObject->SetBoolField(Pair.Key, Pair.Value);
    for (const TPair<FString, bool>& Pair : Truncations) TruncationObject->SetBoolField(Pair.Key, Pair.Value);
    const bool bTerminated = Current.Phase == ETacticalMARLEpisodePhase::Terminated;
    const bool bTruncated = Current.Phase == ETacticalMARLEpisodePhase::Truncated;
    TerminationObject->SetBoolField(TEXT("__all__"), bTerminated);
    TruncationObject->SetBoolField(TEXT("__all__"), bTruncated);
    InfoObject->SetStringField(TEXT("mode"), TEXT("real_time"));
    InfoObject->SetStringField(TEXT("api"), TEXT("tactical_marl_v1"));
    Root->SetObjectField(TEXT("rewards"), RewardObject);
    Root->SetObjectField(TEXT("terminations"), TerminationObject);
    Root->SetObjectField(TEXT("truncations"), TruncationObject);
    Root->SetObjectField(TEXT("infos"), InfoObject);
}

void UTacticalMARLEpisodeSubsystem::CaptureAgentStartStates()
{
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        if (!It->ActorHasTag(TEXT("MARL.Agent"))) continue;
        const bool bAlreadyCaptured = AgentStartStates.ContainsByPredicate(
            [Actor = *It](const FAgentStartState& Start) { return Start.Actor.Get() == Actor; });
        if (!bAlreadyCaptured) AgentStartStates.Add({*It, It->GetActorTransform()});
    }
}

void UTacticalMARLEpisodeSubsystem::ResetActors()
{
    for (const FAgentStartState& Start : AgentStartStates)
    {
        AActor* Actor = Start.Actor.Get();
        if (!IsValid(Actor)) continue;
        if (ATacticalUAVPawn* UAV = Cast<ATacticalUAVPawn>(Actor)) UAV->ResetForEpisode(Start.Transform);
        else if (ATacticalUGVPawn* UGV = Cast<ATacticalUGVPawn>(Actor)) UGV->ResetForEpisode(Start.Transform);
        else Actor->SetActorTransform(Start.Transform, false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void UTacticalMARLEpisodeSubsystem::CaptureBaselineMetrics()
{
    PreviousDetectionCounts.Reset();
    PreviousTaskStates.Reset();
    TArray<FAgentMetric> Metrics;
    CollectBlueAgentMetrics(GetWorld(), Metrics);
    for (const FAgentMetric& Metric : Metrics)
    {
        PreviousDetectionCounts.Add(Metric.Id, Metric.DetectionCount);
        PreviousTaskStates.Add(Metric.Id, Metric.TaskState);
        Rewards.Add(Metric.Id, 0.0f);
        Terminations.Add(Metric.Id, false);
        Truncations.Add(Metric.Id, false);
    }
}

void UTacticalMARLEpisodeSubsystem::ComputeRewardsAndDoneFlags()
{
    TArray<FAgentMetric> Metrics;
    CollectBlueAgentMetrics(GetWorld(), Metrics);
    bool bAnyFailure = false;
    for (const FAgentMetric& Metric : Metrics)
    {
        float Reward = Config.StepPenalty;
        const int32 OldCount = PreviousDetectionCounts.FindRef(Metric.Id);
        Reward += FMath::Max(0, Metric.DetectionCount - OldCount) * Config.NewDetectionReward;
        const FString OldState = PreviousTaskStates.FindRef(Metric.Id);
        if (Metric.TaskState == TEXT("completed") && OldState != TEXT("completed")) Reward += Config.TaskCompletedReward;
        if (Metric.TaskState == TEXT("failed") && OldState != TEXT("failed")) Reward += Config.TaskFailedPenalty;

        bool bOutOfBounds = false;
        if (IsValid(Metric.Actor))
        {
            const FVector Offset = (Metric.Actor->GetActorLocation() - Config.WorldCenter).GetAbs();
            bOutOfBounds = Offset.X > Config.WorldHalfExtent.X || Offset.Y > Config.WorldHalfExtent.Y || Offset.Z > Config.WorldHalfExtent.Z;
        }
        if (bOutOfBounds)
        {
            Reward += Config.OutOfBoundsPenalty;
            Terminations.FindOrAdd(Metric.Id) = true;
            bAnyFailure = true;
        }
        Rewards.FindOrAdd(Metric.Id) = Reward;
        PreviousDetectionCounts.FindOrAdd(Metric.Id) = Metric.DetectionCount;
        PreviousTaskStates.FindOrAdd(Metric.Id) = Metric.TaskState;
    }
    if (UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        const float TeamReward = Mission->ConsumeTeamReward();
        TMap<FString, float> AgentRewards;
        Mission->ConsumeAgentRewards(AgentRewards);
        for (TPair<FString, float>& Pair : Rewards)
        {
            Pair.Value += TeamReward + AgentRewards.FindRef(Pair.Key);
        }
        const FTacticalMARLMissionState MissionState = Mission->GetMissionState();
        if (MissionState.bMissionSuccess && IsEpisodeRunning()) EndEpisode(TEXT("mission_success"), false);
        else if (MissionState.bMissionFailure && IsEpisodeRunning()) EndEpisode(TEXT("mission_failure"), false);
    }
    if (bAnyFailure && Config.bTerminateOnAnyAgentFailure) EndEpisode(TEXT("agent_out_of_bounds"), false);
}

float UTacticalMARLEpisodeSubsystem::GetElapsedSeconds() const
{
    return GetWorld() ? FMath::Max(0.0, GetWorld()->GetTimeSeconds() - EpisodeStartWorldSeconds) : 0.0f;
}

FString UTacticalMARLEpisodeSubsystem::PhaseToString(ETacticalMARLEpisodePhase Phase)
{
    switch (Phase)
    {
    case ETacticalMARLEpisodePhase::Running: return TEXT("running");
    case ETacticalMARLEpisodePhase::Terminated: return TEXT("terminated");
    case ETacticalMARLEpisodePhase::Truncated: return TEXT("truncated");
    default: return TEXT("not_started");
    }
}
