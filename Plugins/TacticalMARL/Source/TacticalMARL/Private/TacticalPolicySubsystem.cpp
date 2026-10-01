#include "TacticalPolicySubsystem.h"

#include "TacticalAgentHealthComponent.h"
#include "TacticalMARLEpisodeSubsystem.h"
#include "TacticalMARLMissionSubsystem.h"
#include "TacticalUAVPawn.h"
#include "TacticalUGVPawn.h"

#include "Common/UdpSocketBuilder.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalPolicy, Log, All);

void UTacticalPolicySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    StartUdpBridge(7777);
}

void UTacticalPolicySubsystem::Deinitialize()
{
    StopUdpBridge();
    Super::Deinitialize();
}

bool UTacticalPolicySubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

TStatId UTacticalPolicySubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTacticalPolicySubsystem, STATGROUP_Tickables);
}

bool UTacticalPolicySubsystem::StartUdpBridge(int32 Port)
{
    if (Socket) return ListeningPort == Port;
    Socket = FUdpSocketBuilder(TEXT("TacticalMARLPolicy"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToAddress(FIPv4Address::InternalLoopback)
        .BoundToPort(Port)
        .WithReceiveBufferSize(2 * 1024 * 1024);
    if (!Socket)
    {
        UE_LOG(LogTacticalPolicy, Error, TEXT("Unable to bind UDP policy bridge to 127.0.0.1:%d"), Port);
        return false;
    }
    ListeningPort = Port;
    UE_LOG(LogTacticalPolicy, Log, TEXT("Python policy bridge listening on udp://127.0.0.1:%d"), Port);
    return true;
}

void UTacticalPolicySubsystem::StopUdpBridge()
{
    if (!Socket) return;
    Socket->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
    Socket = nullptr;
    ListeningPort = 0;
}

void UTacticalPolicySubsystem::Tick(float DeltaTime)
{
    if (!Socket) return;
    uint32 Pending = 0;
    while (Socket->HasPendingData(Pending))
    {
        TArray<uint8> Data;
        Data.SetNumUninitialized(FMath::Min(Pending, 65507u) + 1);
        TSharedRef<FInternetAddr> Sender = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
        int32 Read = 0;
        if (!Socket->RecvFrom(Data.GetData(), Data.Num() - 1, Read, *Sender) || Read <= 0) break;
        Data[Read] = 0;
        const FString Request = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Data.GetData()));
        FString Response;
        SubmitCommandJson(Request, Response);
        FTCHARToUTF8 Encoded(*Response);
        int32 Sent = 0;
        Socket->SendTo(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length(), Sent, *Sender);
    }
}

bool UTacticalPolicySubsystem::SubmitCommandJson(const FString& JsonCommand, FString& OutResponse)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Root) || !Root.IsValid())
    {
        OutResponse = BuildResponse(false, TEXT("invalid_json"));
        return false;
    }
    FString Request;
    if (Root->TryGetStringField(TEXT("request"), Request))
    {
        UTacticalMARLEpisodeSubsystem* Episode = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>();
        if (Request.Equals(TEXT("observations"), ESearchCase::IgnoreCase) ||
            Request.Equals(TEXT("episode"), ESearchCase::IgnoreCase))
        {
            OutResponse = BuildResponse(true);
            return true;
        }
        if (Request.Equals(TEXT("reset"), ESearchCase::IgnoreCase))
        {
            double SeedValue = 0.0;
            Root->TryGetNumberField(TEXT("seed"), SeedValue);
            const bool bOk = Episode && Episode->ResetEpisode(static_cast<int32>(SeedValue));
            FString ResetError = TEXT("episode_reset_failed");
            if (Episode && !bOk) ResetError = Episode->GetEpisodeState().EndReason;
            OutResponse = BuildResponse(bOk, bOk ? FString() : ResetError);
            return bOk;
        }
        if (Request.Equals(TEXT("end"), ESearchCase::IgnoreCase))
        {
            FString Reason = TEXT("external_end");
            Root->TryGetStringField(TEXT("reason"), Reason);
            if (Episode) Episode->EndEpisode(Reason, false);
            OutResponse = BuildResponse(Episode != nullptr, Episode ? FString() : TEXT("episode_subsystem_unavailable"));
            return Episode != nullptr;
        }
        if (Request.Equals(TEXT("advance"), ESearchCase::IgnoreCase))
        {
            const bool bOk = Episode && Episode->AdvanceEpisode();
            OutResponse = BuildResponse(bOk, bOk ? FString() : TEXT("episode_not_running"));
            return bOk;
        }
        if (Request.Equals(TEXT("apply_test_damage"), ESearchCase::IgnoreCase))
        {
            const bool bTestEnabled = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1Test")) ||
                FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"));
            if (!bTestEnabled)
            {
                OutResponse = BuildResponse(false, TEXT("s1_test_damage_disabled"));
                return false;
            }
            FString AgentId;
            double Damage = 0.0;
            if (!Root->TryGetStringField(TEXT("agent_id"), AgentId) ||
                !Root->TryGetNumberField(TEXT("damage"), Damage) || Damage <= 0.0)
            {
                OutResponse = BuildResponse(false, TEXT("invalid_test_damage_request"));
                return false;
            }
            AActor* Target = nullptr;
            for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It && !Target; ++It)
            {
                if (It->AgentId.ToString().Equals(AgentId, ESearchCase::IgnoreCase)) Target = *It;
            }
            for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It && !Target; ++It)
            {
                if (It->AgentId.ToString().Equals(AgentId, ESearchCase::IgnoreCase)) Target = *It;
            }
            UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Target);
            const bool bOk = Health && Health->ApplyAgentDamage(static_cast<float>(Damage), nullptr);
            OutResponse = BuildResponse(bOk, bOk ? FString() : TEXT("test_damage_failed"));
            return bOk;
        }
        const bool bStepRequest = Request.Equals(TEXT("step"), ESearchCase::IgnoreCase);
        const bool bActionsRequest = Request.Equals(TEXT("actions"), ESearchCase::IgnoreCase);
        if (bStepRequest || bActionsRequest)
        {
            if (!Episode || !Episode->IsEpisodeRunning())
            {
                OutResponse = BuildResponse(false, TEXT("episode_not_running"));
                return false;
            }
            const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
            if (!Root->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
            {
                OutResponse = BuildResponse(false, TEXT("missing_actions_array"));
                return false;
            }
            for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
            {
                const TSharedPtr<FJsonObject> Action = ActionValue.IsValid() ? ActionValue->AsObject() : nullptr;
                if (!Action.IsValid())
                {
                    OutResponse = BuildResponse(false, TEXT("action_must_be_an_object"));
                    return false;
                }
                FString ActionJson;
                FJsonSerializer::Serialize(Action.ToSharedRef(), TJsonWriterFactory<>::Create(&ActionJson));
                FString Error;
                if (!RouteCommand(ActionJson, Error))
                {
                    OutResponse = BuildResponse(false, Error);
                    return false;
                }
            }
            // A one-datagram step is useful for manual control. Training wrappers
            // use actions -> wait decision interval -> advance, so rewards and
            // observations describe the transition caused by those actions.
            if (bStepRequest) Episode->AdvanceEpisode();
            OutResponse = BuildResponse(true);
            return true;
        }
        OutResponse = BuildResponse(false, TEXT("unsupported_request"));
        return false;
    }
    FString Error;
    const bool bOk = RouteCommand(JsonCommand, Error);
    OutResponse = BuildResponse(bOk, Error);
    return bOk;
}

bool UTacticalPolicySubsystem::RouteCommand(const FString& JsonCommand, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Root);
    FString AgentId;
    if (!Root.IsValid() || !Root->TryGetStringField(TEXT("agent_id"), AgentId))
    {
        OutError = TEXT("missing_agent_id"); return false;
    }

    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        if (It->AgentId.ToString().Equals(AgentId, ESearchCase::IgnoreCase))
        {
            if (Root->HasField(TEXT("move_input")))
            {
                const TArray<TSharedPtr<FJsonValue>> Input = Root->GetArrayField(TEXT("move_input"));
                if (Input.Num() < 3) { OutError = TEXT("move_input_requires_xyz"); return false; }
                const float YawRate = Root->HasField(TEXT("yaw_rate")) ? Root->GetNumberField(TEXT("yaw_rate")) : 0.0f;
                const int32 Sequence = Root->HasField(TEXT("sequence_id")) ? Root->GetIntegerField(TEXT("sequence_id")) : 0;
                const bool bOk = It->SubmitContinuousAction(FVector(Input[0]->AsNumber(), Input[1]->AsNumber(), Input[2]->AsNumber()), YawRate, Sequence);
                if (!bOk) OutError = TEXT("continuous_action_rejected"); return bOk;
            }
            return It->ReceivePolicyJson(JsonCommand, OutError);
        }
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        if (It->AgentId.ToString().Equals(AgentId, ESearchCase::IgnoreCase))
        {
            return It->ReceivePolicyJson(JsonCommand, OutError);
        }
    }
    OutError = TEXT("agent_not_found"); return false;
}

FString UTacticalPolicySubsystem::GetObservationsJson() const
{
    const UTacticalMARLMissionSubsystem* MissionSubsystem = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>();
    auto MakeMissionObservation = [MissionSubsystem]()
    {
        TSharedRef<FJsonObject> Mission = MakeShared<FJsonObject>();
        if (!MissionSubsystem) return Mission;
        const FTacticalMARLMissionState State = MissionSubsystem->GetMissionState();
        FString Phase = TEXT("search");
        switch (State.Phase)
        {
        case ETacticalMARLMissionPhase::CandidateFound: Phase = TEXT("candidate_found"); break;
        case ETacticalMARLMissionPhase::TargetConfirmed: Phase = TEXT("target_confirmed"); break;
        case ETacticalMARLMissionPhase::TrackEstablished: Phase = TEXT("track_established"); break;
        case ETacticalMARLMissionPhase::StrikeReady: Phase = TEXT("strike_ready"); break;
        case ETacticalMARLMissionPhase::Engagement: Phase = TEXT("engagement"); break;
        case ETacticalMARLMissionPhase::EffectAssessment: Phase = TEXT("effect_assessment"); break;
        case ETacticalMARLMissionPhase::Success: Phase = TEXT("success"); break;
        case ETacticalMARLMissionPhase::Failed: Phase = TEXT("failed"); break;
        default: break;
        }
        Mission->SetStringField(TEXT("phase"), Phase);
        Mission->SetBoolField(TEXT("candidate_found"), State.bCandidateFound);
        Mission->SetBoolField(TEXT("target_confirmed"), State.bTargetConfirmed);
        Mission->SetBoolField(TEXT("track_valid"), State.bTrackValid);
        Mission->SetNumberField(TEXT("track_quality"), State.TrackQuality);
        Mission->SetBoolField(TEXT("strike_ready"), State.bStrikeReady);
        Mission->SetStringField(TEXT("target_zone"), State.TargetZone.ToString());
        return Mission;
    };
    TArray<TSharedPtr<FJsonValue>> Agents;
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        TSharedPtr<FJsonObject> Agent;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(It->GetTelemetryJson()), Agent) && Agent.IsValid())
        {
            Agent->SetStringField(TEXT("agent_type"), TEXT("uav"));
            Agent->SetObjectField(TEXT("mission"), MakeMissionObservation());
            Agents.Add(MakeShared<FJsonValueObject>(Agent));
        }
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        TSharedPtr<FJsonObject> Agent;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(It->GetTelemetryJson()), Agent) && Agent.IsValid())
        {
            Agent->SetObjectField(TEXT("mission"), MakeMissionObservation());
            Agents.Add(MakeShared<FJsonValueObject>(Agent));
        }
    }
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("agents"), Agents);
    FString Result; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result)); return Result;
}

FString UTacticalPolicySubsystem::BuildResponse(bool bOk, const FString& Error) const
{
    TSharedPtr<FJsonObject> Observations;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(GetObservationsJson()), Observations);
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("ok"), bOk);
    if (!Error.IsEmpty()) Root->SetStringField(TEXT("error"), Error);
    Root->SetObjectField(TEXT("observations"), Observations.ToSharedRef());
    if (const UTacticalMARLEpisodeSubsystem* Episode = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>())
    {
        Episode->AppendStepFields(Root);
    }
    if (const UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        Mission->AppendMissionFields(Root);
    }

    // Lightweight health data used by the long-running Python evaluator.  It
    // is deliberately observational: the evaluator still controls the world
    // exclusively through the public JSON action/reset protocol.
    TMap<FString, int32> AgentIdCounts;
    TMap<const AController*, int32> ControllerCounts;
    int32 AgentActorCount = 0;
    int32 UnpossessedAgentCount = 0;
    int32 BlueAliveCount = 0;
    int32 BlueDisabledCount = 0;
    auto AddAgentDiagnostic = [&AgentIdCounts, &ControllerCounts, &AgentActorCount, &UnpossessedAgentCount](const APawn* Pawn, FName AgentId)
    {
        ++AgentActorCount;
        AgentIdCounts.FindOrAdd(AgentId.ToString())++;
        if (const AController* Controller = Pawn->GetController()) ControllerCounts.FindOrAdd(Controller)++;
        else ++UnpossessedAgentCount;
    };
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        AddAgentDiagnostic(*It, It->AgentId);
        BlueAliveCount += !It->HealthComponent || It->HealthComponent->IsAlive() ? 1 : 0;
        BlueDisabledCount += It->HealthComponent && It->HealthComponent->IsDisabled() ? 1 : 0;
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        AddAgentDiagnostic(*It, It->AgentId);
        BlueAliveCount += !It->HealthComponent || It->HealthComponent->IsAlive() ? 1 : 0;
        BlueDisabledCount += It->HealthComponent && It->HealthComponent->IsDisabled() ? 1 : 0;
    }

    int32 DuplicateAgentIds = 0;
    for (const TPair<FString, int32>& Pair : AgentIdCounts) DuplicateAgentIds += FMath::Max(0, Pair.Value - 1);
    int32 DuplicateControllers = 0;
    for (const TPair<const AController*, int32>& Pair : ControllerCounts) DuplicateControllers += FMath::Max(0, Pair.Value - 1);
    SIZE_T ProcessMemoryBytes = 0;
    FPlatformProcess::GetApplicationMemoryUsage(FPlatformProcess::GetCurrentProcessId(), &ProcessMemoryBytes);
    TSharedRef<FJsonObject> Diagnostics = MakeShared<FJsonObject>();
    Diagnostics->SetNumberField(TEXT("agent_actor_count"), AgentActorCount);
    Diagnostics->SetNumberField(TEXT("unique_agent_id_count"), AgentIdCounts.Num());
    Diagnostics->SetNumberField(TEXT("duplicate_agent_id_count"), DuplicateAgentIds);
    Diagnostics->SetNumberField(TEXT("controller_count"), ControllerCounts.Num());
    Diagnostics->SetNumberField(TEXT("duplicate_controller_count"), DuplicateControllers);
    Diagnostics->SetNumberField(TEXT("unpossessed_agent_count"), UnpossessedAgentCount);
    Diagnostics->SetNumberField(TEXT("blue_alive_count"), BlueAliveCount);
    Diagnostics->SetNumberField(TEXT("blue_disabled_count"), BlueDisabledCount);
    Diagnostics->SetNumberField(TEXT("world_time_seconds"), GetWorld()->GetTimeSeconds());
    Diagnostics->SetNumberField(TEXT("process_memory_mb"), static_cast<double>(ProcessMemoryBytes) / (1024.0 * 1024.0));
    Root->SetObjectField(TEXT("diagnostics"), Diagnostics);
    FString Result; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result)); return Result;
}
