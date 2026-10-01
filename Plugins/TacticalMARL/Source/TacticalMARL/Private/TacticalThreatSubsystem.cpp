#include "TacticalThreatSubsystem.h"

#include "TacticalAgentHealthComponent.h"
#include "TacticalMARLCombatantProvider.h"
#include "TacticalUAVPawn.h"
#include "TacticalUGVPawn.h"

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

DEFINE_LOG_CATEGORY_STATIC(LogTacticalThreat, Log, All);

namespace
{
TAutoConsoleVariable<int32> CVarTacticalMARLDifficulty(
    TEXT("tacticalmarl.Difficulty"), 0,
    TEXT("TacticalMARL difficulty: 0 passive, 1 local red sensing/shared alert."), ECVF_Default);

constexpr float SuspiciousThreshold = 0.15f;
constexpr float AlertedThreshold = 0.42f;
constexpr float TrackingThreshold = 0.72f;
}

void UTacticalThreatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ResetForEpisode(0);
}

void UTacticalThreatSubsystem::Deinitialize()
{
    DestroyAcceptanceObstacle();
    Sensors.Reset();
    PendingMessages.Reset();
    Super::Deinitialize();
}

bool UTacticalThreatSubsystem::DoesSupportWorldType(const EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

TStatId UTacticalThreatSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTacticalThreatSubsystem, STATGROUP_Tickables);
}

bool UTacticalThreatSubsystem::IsThreatSensingEnabled() const
{
    return CVarTacticalMARLDifficulty.GetValueOnGameThread() >= 1 ||
        FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2AutoDemo")) ||
        FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2Test"));
}

void UTacticalThreatSubsystem::Tick(const float DeltaTime)
{
    RosterRefreshAccumulator += DeltaTime;
    if (RosterRefreshAccumulator >= 0.5f || Sensors.Num() == 0)
    {
        RosterRefreshAccumulator = 0.0f;
        RefreshSensorRoster();
    }
    if (!IsThreatSensingEnabled()) return;
    for (FTacticalThreatSensorState& Sensor : Sensors) UpdateSensor(Sensor, DeltaTime);
    ProcessPendingMessages(DeltaTime);
}

void UTacticalThreatSubsystem::ResetForEpisode(const int32 Seed)
{
    RefreshSensorRoster();
    EpisodeSeed = Seed;
    PendingMessages.Reset();
    SharedAlert = FTacticalSharedAlertState();
    EventSequence.Reset();
    EventSequence.Add(TEXT("shared:unaware"));
    AcceptanceStage = TEXT("none");
    AcceptanceTarget.Reset();
    AcceptanceSource.Reset();
    DestroyAcceptanceObstacle();
    for (FTacticalThreatSensorState& Sensor : Sensors)
    {
        const FName SensorId = Sensor.SensorAgentId;
        const FName Role = Sensor.Role;
        const TWeakObjectPtr<AActor> Actor = Sensor.SensorActor;
        float Range = 0.0f;
        float Fov = 0.0f;
        GetSensorConfiguration(Role, Range, Fov);
        Sensor = FTacticalThreatSensorState();
        Sensor.SensorAgentId = SensorId;
        Sensor.Role = Role;
        Sensor.SensorActor = Actor;
        Sensor.SensorRange = Range;
        Sensor.FieldOfViewDegrees = Fov;
    }
    UE_LOG(LogTacticalThreat, Log, TEXT("S2 threat state reset seed=%d sensors=%d"), EpisodeSeed, Sensors.Num());
}

void UTacticalThreatSubsystem::RefreshSensorRoster()
{
    struct FFoundSensor
    {
        FName AgentId;
        FName Role;
        TWeakObjectPtr<AActor> Actor;
    };
    TArray<FFoundSensor> Found;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UActorComponent*> Components;
        It->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component || !Component->GetClass()->ImplementsInterface(UTacticalMARLCombatantProvider::StaticClass())) continue;
            const FTacticalMARLCombatantState State = ITacticalMARLCombatantProvider::Execute_GetCombatantState(Component);
            if (State.Actor && !State.AgentId.IsNone() && State.bAlive)
            {
                Found.Add({State.AgentId, State.Role, State.Actor});
            }
        }
    }
    Found.Sort([](const FFoundSensor& Left, const FFoundSensor& Right)
    {
        return Left.AgentId.LexicalLess(Right.AgentId);
    });

    for (const FFoundSensor& Item : Found)
    {
        FTacticalThreatSensorState* Existing = Sensors.FindByPredicate(
            [&Item](const FTacticalThreatSensorState& State) { return State.SensorAgentId == Item.AgentId; });
        if (!Existing)
        {
            FTacticalThreatSensorState NewSensor;
            NewSensor.SensorAgentId = Item.AgentId;
            NewSensor.Role = Item.Role;
            NewSensor.SensorActor = Item.Actor;
            GetSensorConfiguration(Item.Role, NewSensor.SensorRange, NewSensor.FieldOfViewDegrees);
            Sensors.Add(NewSensor);
        }
        else
        {
            Existing->SensorActor = Item.Actor;
            Existing->Role = Item.Role;
            GetSensorConfiguration(Item.Role, Existing->SensorRange, Existing->FieldOfViewDegrees);
        }
    }
    Sensors.RemoveAll([&Found](const FTacticalThreatSensorState& State)
    {
        return !Found.ContainsByPredicate([&State](const FFoundSensor& Item) { return Item.AgentId == State.SensorAgentId; });
    });
    Sensors.Sort([](const FTacticalThreatSensorState& Left, const FTacticalThreatSensorState& Right)
    {
        return Left.SensorAgentId.LexicalLess(Right.SensorAgentId);
    });
}

void UTacticalThreatSubsystem::UpdateSensor(FTacticalThreatSensorState& Sensor, const float DeltaTime)
{
    AActor* SensorActor = Sensor.SensorActor.Get();
    if (!SensorActor) return;
    const FVector Eye = SensorActor->GetActorLocation() + FVector(0.0f, 0.0f, 110.0f);
    const FVector Forward = SensorActor->GetActorForwardVector().GetSafeNormal();
    const float MinDot = FMath::Cos(FMath::DegreesToRadians(Sensor.FieldOfViewDegrees * 0.5f));
    AActor* BestTarget = nullptr;
    float BestMeasurement = 0.0f;
    bool bSawBlockedTrace = false;
    FVector BlockedTraceEnd = FVector::ZeroVector;

    TArray<AActor*> BlueCandidates;
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It) BlueCandidates.Add(*It);
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It) BlueCandidates.Add(*It);
    BlueCandidates.Sort([](const AActor& Left, const AActor& Right)
    {
        return ResolveBlueAgentId(&Left).LexicalLess(ResolveBlueAgentId(&Right));
    });

    FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(TacticalRedThreatLOS), true, SensorActor);
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        if (It->ActorHasTag(TEXT("MARL.Red"))) TraceParams.AddIgnoredActor(*It);
    }
    for (AActor* Candidate : BlueCandidates)
    {
        const UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Candidate);
        if (!Candidate || (Health && !Health->IsAlive())) continue;
        const FVector AimPoint = Candidate->GetActorLocation();
        const FVector Offset = AimPoint - Eye;
        const float Distance = Offset.Size();
        if (Distance > Sensor.SensorRange || Distance <= KINDA_SMALL_NUMBER) continue;
        const FVector Direction = Offset / Distance;
        const float FacingDot = FVector::DotProduct(Forward, Direction);
        if (FacingDot < MinDot) continue;

        FHitResult Hit;
        const bool bHit = GetWorld()->LineTraceSingleByChannel(
            Hit, Eye, AimPoint, ECC_Visibility, TraceParams);
        const bool bVisible = !bHit || Hit.GetActor() == Candidate;
        if (!bVisible)
        {
            bSawBlockedTrace = true;
            BlockedTraceEnd = Hit.ImpactPoint;
            ++Sensor.BlockedTraceCount;
            continue;
        }
        const float DistanceQuality = 1.0f - Distance / Sensor.SensorRange;
        const float AngleQuality = (FacingDot - MinDot) / FMath::Max(0.01f, 1.0f - MinDot);
        const float Measurement = FMath::Clamp(0.20f + DistanceQuality * 0.45f + AngleQuality * 0.35f, 0.05f, 1.0f);
        if (!BestTarget || Measurement > BestMeasurement)
        {
            BestTarget = Candidate;
            BestMeasurement = Measurement;
        }
    }

    Sensor.bLastTraceBlocked = bSawBlockedTrace;
    if (bSawBlockedTrace) Sensor.LastTraceEnd = BlockedTraceEnd;
    if (BestTarget)
    {
        Sensor.bDirectLineOfSight = true;
        Sensor.LostContactSeconds = 0.0f;
        Sensor.TargetAgentId = ResolveBlueAgentId(BestTarget);
        Sensor.TargetType = ResolveBlueTargetType(BestTarget);
        Sensor.LastKnownLocation = BestTarget->GetActorLocation();
        Sensor.LastSeenWorldSeconds = GetWorld()->GetTimeSeconds();
        Sensor.Confidence = FMath::Clamp(
            Sensor.Confidence + DetectionGainPerSecond * BestMeasurement * DeltaTime, 0.0f, 1.0f);
        const ETacticalRedAlertState NewState = Sensor.Confidence >= TrackingThreshold
            ? ETacticalRedAlertState::Tracking
            : Sensor.Confidence >= AlertedThreshold
                ? ETacticalRedAlertState::Alerted
                : Sensor.Confidence >= SuspiciousThreshold
                    ? ETacticalRedAlertState::Suspicious
                    : ETacticalRedAlertState::Unaware;
        const bool bStateChanged = NewState != Sensor.AlertState;
        SetSensorAlertState(Sensor, NewState);
        if (NewState != ETacticalRedAlertState::Unaware &&
            (bStateChanged || Sensor.LastPublishWorldSeconds < 0.0f ||
             GetWorld()->GetTimeSeconds() - Sensor.LastPublishWorldSeconds >= SharedPublishInterval))
        {
            QueueSharedAlert(Sensor);
        }
    }
    else
    {
        if (Sensor.bDirectLineOfSight) Sensor.LostContactSeconds = 0.0f;
        Sensor.bDirectLineOfSight = false;
        Sensor.LostContactSeconds += DeltaTime;
        Sensor.Confidence = FMath::Max(0.0f, Sensor.Confidence - DirectConfidenceDecayPerSecond * DeltaTime);
        if (!Sensor.TargetAgentId.IsNone() && Sensor.LostContactSeconds >= LostContactDelaySeconds)
        {
            SetSensorAlertState(Sensor, ETacticalRedAlertState::LostContact);
        }
        if (Sensor.Confidence <= KINDA_SMALL_NUMBER && Sensor.LostContactSeconds >= ForgetAfterSeconds)
        {
            Sensor.TargetAgentId = NAME_None;
            Sensor.TargetType = NAME_None;
            Sensor.LastKnownLocation = FVector::ZeroVector;
            Sensor.LastSeenWorldSeconds = -1.0f;
            SetSensorAlertState(Sensor, ETacticalRedAlertState::Unaware);
        }
    }
}

void UTacticalThreatSubsystem::QueueSharedAlert(FTacticalThreatSensorState& Sensor)
{
    const float Delay = GetPropagationDelay(Sensor.SensorAgentId, Sensor.TargetAgentId);
    FPendingAlertMessage Message;
    Message.DeliverAt = GetWorld()->GetTimeSeconds() + Delay;
    Message.SeenAt = Sensor.LastSeenWorldSeconds;
    Message.PropagationDelay = Delay;
    Message.Confidence = Sensor.Confidence;
    Message.SourceAgentId = Sensor.SensorAgentId;
    Message.TargetAgentId = Sensor.TargetAgentId;
    Message.TargetType = Sensor.TargetType;
    Message.LastKnownLocation = Sensor.LastKnownLocation;
    Message.AlertState = Sensor.AlertState;
    PendingMessages.Add(Message);
    Sensor.LastPublishWorldSeconds = GetWorld()->GetTimeSeconds();
}

void UTacticalThreatSubsystem::ProcessPendingMessages(const float DeltaTime)
{
    const double Now = GetWorld()->GetTimeSeconds();
    PendingMessages.Sort([](const FPendingAlertMessage& Left, const FPendingAlertMessage& Right)
    {
        if (!FMath::IsNearlyEqual(Left.DeliverAt, Right.DeliverAt)) return Left.DeliverAt < Right.DeliverAt;
        return Left.SourceAgentId.LexicalLess(Right.SourceAgentId);
    });
    for (int32 Index = PendingMessages.Num() - 1; Index >= 0; --Index)
    {
        const FPendingAlertMessage& Message = PendingMessages[Index];
        if (Message.DeliverAt > Now) continue;
        SharedAlert.Confidence = FMath::Max(SharedAlert.Confidence, Message.Confidence);
        SharedAlert.SourceAgentId = Message.SourceAgentId;
        SharedAlert.TargetAgentId = Message.TargetAgentId;
        SharedAlert.TargetType = Message.TargetType;
        SharedAlert.LastKnownLocation = Message.LastKnownLocation;
        SharedAlert.LastSeenWorldSeconds = Message.SeenAt;
        SharedAlert.LastDeliveryWorldSeconds = Now;
        SharedAlert.LastPropagationDelay = Message.PropagationDelay;
        ++SharedAlert.DeliveredMessageCount;
        ETacticalRedAlertState DeliveredState = SharedAlert.Confidence >= TrackingThreshold
            ? ETacticalRedAlertState::Tracking
            : SharedAlert.Confidence >= AlertedThreshold
                ? ETacticalRedAlertState::Alerted
                : ETacticalRedAlertState::Suspicious;
        if (SharedAlert.AlertState == ETacticalRedAlertState::LostContact)
        {
            const FTacticalThreatSensorState* SourceSensor = Sensors.FindByPredicate(
                [&Message](const FTacticalThreatSensorState& Sensor)
                {
                    return Sensor.SensorAgentId == Message.SourceAgentId;
                });
            if (!SourceSensor || !SourceSensor->bDirectLineOfSight)
            {
                DeliveredState = ETacticalRedAlertState::LostContact;
            }
        }
        if (SharedAlert.AlertState == ETacticalRedAlertState::Tracking)
        {
            DeliveredState = ETacticalRedAlertState::Tracking;
        }
        else if (SharedAlert.AlertState == ETacticalRedAlertState::Alerted &&
            DeliveredState == ETacticalRedAlertState::Suspicious)
        {
            DeliveredState = ETacticalRedAlertState::Alerted;
        }
        SetSharedAlertState(DeliveredState);
        PendingMessages.RemoveAt(Index);
    }

    if (SharedAlert.AlertState == ETacticalRedAlertState::Unaware) return;
    SharedAlert.Confidence = FMath::Max(0.0f, SharedAlert.Confidence - SharedConfidenceDecayPerSecond * DeltaTime);
    const double FreshnessReference = FMath::Max(
        static_cast<double>(SharedAlert.LastSeenWorldSeconds),
        static_cast<double>(SharedAlert.LastDeliveryWorldSeconds));
    const float SinceFreshInformation = FreshnessReference < 0.0
        ? BIG_NUMBER : static_cast<float>(Now - FreshnessReference);
    if (SinceFreshInformation >= LostContactDelaySeconds && SharedAlert.AlertState != ETacticalRedAlertState::LostContact)
    {
        SetSharedAlertState(ETacticalRedAlertState::LostContact);
    }
    if (SharedAlert.Confidence <= KINDA_SMALL_NUMBER || SinceFreshInformation >= ForgetAfterSeconds)
    {
        SetSharedAlertState(ETacticalRedAlertState::Unaware);
        SharedAlert = FTacticalSharedAlertState();
    }
}

void UTacticalThreatSubsystem::SetSensorAlertState(
    FTacticalThreatSensorState& Sensor, const ETacticalRedAlertState NewState)
{
    if (Sensor.AlertState == NewState) return;
    Sensor.AlertState = NewState;
    EventSequence.Add(FString::Printf(TEXT("%s:%s"),
        *Sensor.SensorAgentId.ToString(), *AlertStateToString(NewState)));
    UE_LOG(LogTacticalThreat, Display, TEXT("Sensor %s -> %s confidence=%.3f target=%s direct_los=%s"),
        *Sensor.SensorAgentId.ToString(), *AlertStateToString(NewState), Sensor.Confidence,
        *Sensor.TargetAgentId.ToString(), Sensor.bDirectLineOfSight ? TEXT("true") : TEXT("false"));
}

void UTacticalThreatSubsystem::SetSharedAlertState(const ETacticalRedAlertState NewState)
{
    if (SharedAlert.AlertState == NewState) return;
    SharedAlert.AlertState = NewState;
    EventSequence.Add(FString::Printf(TEXT("shared:%s"), *AlertStateToString(NewState)));
    UE_LOG(LogTacticalThreat, Display, TEXT("Shared alert -> %s confidence=%.3f source=%s target=%s"),
        *AlertStateToString(NewState), SharedAlert.Confidence,
        *SharedAlert.SourceAgentId.ToString(), *SharedAlert.TargetAgentId.ToString());
}

float UTacticalThreatSubsystem::GetPropagationDelay(const FName SourceAgentId, const FName TargetAgentId) const
{
    const uint32 Hash = HashCombine(HashCombine(GetTypeHash(EpisodeSeed), GetTypeHash(SourceAgentId)), GetTypeHash(TargetAgentId));
    return 0.65f + static_cast<float>(Hash % 31u) * 0.01f;
}

void UTacticalThreatSubsystem::AppendThreatFields(const TSharedRef<FJsonObject>& Root) const
{
    TSharedRef<FJsonObject> Threat = MakeShared<FJsonObject>();
    Threat->SetBoolField(TEXT("enabled"), IsThreatSensingEnabled());
    Threat->SetStringField(TEXT("difficulty"), IsThreatSensingEnabled() ? TEXT("D1") : TEXT("D0"));
    Threat->SetNumberField(TEXT("seed"), EpisodeSeed);
    Threat->SetStringField(TEXT("acceptance_stage"), AcceptanceStage);
    Threat->SetNumberField(TEXT("pending_message_count"), PendingMessages.Num());

    TSharedRef<FJsonObject> Shared = MakeShared<FJsonObject>();
    Shared->SetStringField(TEXT("state"), AlertStateToString(SharedAlert.AlertState));
    Shared->SetNumberField(TEXT("confidence"), SharedAlert.Confidence);
    Shared->SetStringField(TEXT("source_agent_id"), SharedAlert.SourceAgentId.ToString());
    Shared->SetStringField(TEXT("target_agent_id"), SharedAlert.TargetAgentId.ToString());
    Shared->SetStringField(TEXT("target_type"), SharedAlert.TargetType.ToString());
    Shared->SetNumberField(TEXT("last_seen_world_seconds"), SharedAlert.LastSeenWorldSeconds);
    Shared->SetNumberField(TEXT("last_delivery_world_seconds"), SharedAlert.LastDeliveryWorldSeconds);
    Shared->SetNumberField(TEXT("propagation_delay"), SharedAlert.LastPropagationDelay);
    Shared->SetNumberField(TEXT("delivered_message_count"), SharedAlert.DeliveredMessageCount);
    if (!SharedAlert.TargetAgentId.IsNone())
    {
        Shared->SetArrayField(TEXT("last_known_location"), {
            MakeShared<FJsonValueNumber>(SharedAlert.LastKnownLocation.X),
            MakeShared<FJsonValueNumber>(SharedAlert.LastKnownLocation.Y),
            MakeShared<FJsonValueNumber>(SharedAlert.LastKnownLocation.Z)});
    }
    else
    {
        Shared->SetField(TEXT("last_known_location"), MakeShared<FJsonValueNull>());
    }
    Threat->SetObjectField(TEXT("shared_alert"), Shared);

    TArray<TSharedPtr<FJsonValue>> SensorValues;
    for (const FTacticalThreatSensorState& Sensor : Sensors)
    {
        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("sensor_agent_id"), Sensor.SensorAgentId.ToString());
        Item->SetStringField(TEXT("role"), Sensor.Role.ToString());
        Item->SetStringField(TEXT("state"), AlertStateToString(Sensor.AlertState));
        Item->SetNumberField(TEXT("confidence"), Sensor.Confidence);
        Item->SetBoolField(TEXT("direct_los"), Sensor.bDirectLineOfSight);
        Item->SetBoolField(TEXT("last_trace_blocked"), Sensor.bLastTraceBlocked);
        Item->SetNumberField(TEXT("blocked_trace_count"), Sensor.BlockedTraceCount);
        Item->SetNumberField(TEXT("range"), Sensor.SensorRange);
        Item->SetNumberField(TEXT("fov_degrees"), Sensor.FieldOfViewDegrees);
        Item->SetStringField(TEXT("target_agent_id"), Sensor.TargetAgentId.ToString());
        Item->SetStringField(TEXT("target_type"), Sensor.TargetType.ToString());
        Item->SetNumberField(TEXT("last_seen_world_seconds"), Sensor.LastSeenWorldSeconds);
        if (!Sensor.TargetAgentId.IsNone())
        {
            Item->SetArrayField(TEXT("last_known_location"), {
                MakeShared<FJsonValueNumber>(Sensor.LastKnownLocation.X),
                MakeShared<FJsonValueNumber>(Sensor.LastKnownLocation.Y),
                MakeShared<FJsonValueNumber>(Sensor.LastKnownLocation.Z)});
        }
        else
        {
            Item->SetField(TEXT("last_known_location"), MakeShared<FJsonValueNull>());
        }
        SensorValues.Add(MakeShared<FJsonValueObject>(Item));
    }
    Threat->SetArrayField(TEXT("sensors"), SensorValues);

    TArray<TSharedPtr<FJsonValue>> Events;
    for (const FString& Event : EventSequence) Events.Add(MakeShared<FJsonValueString>(Event));
    Threat->SetArrayField(TEXT("event_sequence"), Events);
    Root->SetObjectField(TEXT("red_threat"), Threat);
}

bool UTacticalThreatSubsystem::SetAcceptanceStage(const FString& Stage, FString& OutError)
{
    const bool bTestEnabled = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2Test")) ||
        FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2AutoDemo"));
    if (!bTestEnabled)
    {
        OutError = TEXT("s2_test_stage_disabled");
        return false;
    }
    const FString Normalized = Stage.ToLower();
    if (Normalized == TEXT("occluded"))
    {
        if (!PositionAcceptanceTarget(true, OutError)) return false;
    }
    else if (Normalized == TEXT("visible"))
    {
        if (!PositionAcceptanceTarget(false, OutError)) return false;
    }
    else if (Normalized == TEXT("lost_contact"))
    {
        if (!PositionAcceptanceTarget(true, OutError)) return false;
    }
    else
    {
        OutError = TEXT("unsupported_s2_test_stage");
        return false;
    }
    AcceptanceStage = Normalized;
    OutError.Reset();
    UE_LOG(LogTacticalThreat, Display, TEXT("S2 acceptance stage -> %s"), *AcceptanceStage);
    return true;
}

bool UTacticalThreatSubsystem::PositionAcceptanceTarget(const bool bBlocked, FString& OutError)
{
    RefreshSensorRoster();
    FTacticalThreatSensorState* SourceSensor = Sensors.FindByPredicate([](const FTacticalThreatSensorState& Sensor)
    {
        return Sensor.SensorAgentId == TEXT("RED_ANTIUAV_01");
    });
    if (!SourceSensor && Sensors.Num() > 0) SourceSensor = &Sensors[0];
    AActor* Source = SourceSensor ? SourceSensor->SensorActor.Get() : nullptr;
    ATacticalUAVPawn* Target = nullptr;
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        if (It->AgentId == TEXT("BLUE_UAV_01")) { Target = *It; break; }
    }
    if (!Source || !Target)
    {
        OutError = TEXT("s2_acceptance_actors_not_ready");
        return false;
    }

    const FVector Forward = Source->GetActorForwardVector().GetSafeNormal2D();
    const FVector TargetLocation = Source->GetActorLocation() + Forward * 1800.0f + FVector(0.0f, 0.0f, 850.0f);
    Target->CancelCurrentTask();
    Target->SetActorLocation(TargetLocation, false, nullptr, ETeleportType::TeleportPhysics);
    Target->SetActorRotation((-Forward).Rotation());
    AcceptanceSource = Source;
    AcceptanceTarget = Target;
    DestroyAcceptanceObstacle();

    if (bBlocked)
    {
        FActorSpawnParameters Params;
        Params.Name = MakeUniqueObjectName(GetWorld(), AStaticMeshActor::StaticClass(), TEXT("TacticalMARL_S2_LOS_Blocker"));
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Params.ObjectFlags |= RF_Transient;
        AStaticMeshActor* Obstacle = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Params);
        if (!Obstacle)
        {
            OutError = TEXT("s2_obstacle_spawn_failed");
            return false;
        }
        UStaticMeshComponent* Mesh = Obstacle->GetStaticMeshComponent();
        Mesh->SetMobility(EComponentMobility::Movable);
        Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Mesh->SetCollisionProfileName(TEXT("BlockAll"));
        const FVector Eye = Source->GetActorLocation() + FVector(0.0f, 0.0f, 110.0f);
        const FVector Midpoint = FMath::Lerp(Eye, TargetLocation, 0.52f);
        Obstacle->SetActorLocationAndRotation(Midpoint, Forward.Rotation());
        Obstacle->SetActorScale3D(FVector(0.60f, 14.0f, 10.0f));
        Obstacle->Tags.Add(TEXT("MARL.S2AcceptanceObstacle"));
        if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
        {
            UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(BaseMaterial, Obstacle);
            Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.08f, 0.09f, 0.11f));
            Mesh->SetMaterial(0, Material);
        }
        AcceptanceObstacle = Obstacle;
    }
    return true;
}

void UTacticalThreatSubsystem::DestroyAcceptanceObstacle()
{
    if (AActor* Obstacle = AcceptanceObstacle.Get()) Obstacle->Destroy();
    AcceptanceObstacle.Reset();
}

FName UTacticalThreatSubsystem::ResolveBlueAgentId(const AActor* Actor)
{
    if (const ATacticalUAVPawn* UAV = Cast<ATacticalUAVPawn>(Actor)) return UAV->AgentId;
    if (const ATacticalUGVPawn* UGV = Cast<ATacticalUGVPawn>(Actor)) return UGV->AgentId;
    return NAME_None;
}

FName UTacticalThreatSubsystem::ResolveBlueTargetType(const AActor* Actor)
{
    if (Actor && Actor->IsA<ATacticalUAVPawn>()) return TEXT("uav");
    if (Actor && Actor->IsA<ATacticalUGVPawn>()) return TEXT("ugv");
    return NAME_None;
}

void UTacticalThreatSubsystem::GetSensorConfiguration(const FName Role, float& OutRange, float& OutFovDegrees)
{
    if (Role == TEXT("MARL.Role.AntiUAV")) { OutRange = 6500.0f; OutFovDegrees = 140.0f; }
    else if (Role == TEXT("MARL.Role.Sniper")) { OutRange = 5500.0f; OutFovDegrees = 100.0f; }
    else { OutRange = 3500.0f; OutFovDegrees = 120.0f; }
}

FString UTacticalThreatSubsystem::AlertStateToString(const ETacticalRedAlertState State)
{
    switch (State)
    {
    case ETacticalRedAlertState::Suspicious: return TEXT("suspicious");
    case ETacticalRedAlertState::Alerted: return TEXT("alerted");
    case ETacticalRedAlertState::Tracking: return TEXT("tracking");
    case ETacticalRedAlertState::Engaging: return TEXT("engaging");
    case ETacticalRedAlertState::LostContact: return TEXT("lost_contact");
    default: return TEXT("unaware");
    }
}
