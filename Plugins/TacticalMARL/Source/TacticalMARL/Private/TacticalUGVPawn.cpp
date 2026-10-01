#include "TacticalUGVPawn.h"
#include "TacticalAgentHealthComponent.h"
#include "TacticalMARLMissionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalUGV, Log, All);

ATacticalUGVPawn::ATacticalUGVPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    SetReplicates(true);
    SetReplicateMovement(true);

    CollisionComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
    CollisionComponent->InitBoxExtent(FVector(140.0f, 90.0f, 45.0f));
    CollisionComponent->SetCollisionProfileName(TEXT("Pawn"));
    RootComponent = CollisionComponent;

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

    BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    BodyMesh->SetupAttachment(RootComponent);
    BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BodyMesh->SetRelativeScale3D(FVector(2.6f, 1.6f, 0.65f));
    if (Cube.Succeeded()) BodyMesh->SetStaticMesh(Cube.Object);

    TurretMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Turret"));
    TurretMesh->SetupAttachment(RootComponent);
    TurretMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TurretMesh->SetRelativeLocation(FVector(15.0f, 0.0f, 60.0f));
    TurretMesh->SetRelativeScale3D(FVector(0.65f, 0.65f, 0.25f));
    if (Cylinder.Succeeded()) TurretMesh->SetStaticMesh(Cylinder.Object);

    UStaticMeshComponent* Barrel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SensorMast"));
    Barrel->SetupAttachment(TurretMesh);
    Barrel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Barrel->SetRelativeLocation(FVector(95.0f, 0.0f, 10.0f));
    Barrel->SetRelativeScale3D(FVector(1.5f, 0.12f, 0.12f));
    if (Cube.Succeeded()) Barrel->SetStaticMesh(Cube.Object);

    const FVector WheelOffsets[] = {
        FVector(85, 92, -38), FVector(85, -92, -38),
        FVector(-85, 92, -38), FVector(-85, -92, -38)
    };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(WheelOffsets); ++Index)
    {
        UStaticMeshComponent* Wheel = CreateDefaultSubobject<UStaticMeshComponent>(
            *FString::Printf(TEXT("Wheel_%d"), Index + 1));
        Wheel->SetupAttachment(RootComponent);
        Wheel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Wheel->SetRelativeLocation(WheelOffsets[Index]);
        Wheel->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
        Wheel->SetRelativeScale3D(FVector(0.42f, 0.42f, 0.22f));
        if (Cylinder.Succeeded()) Wheel->SetStaticMesh(Cylinder.Object);
    }

    HealthComponent = CreateDefaultSubobject<UTacticalAgentHealthComponent>(TEXT("Health"));

    DamageStatusText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("DamageStatusText"));
    DamageStatusText->SetupAttachment(RootComponent);
    DamageStatusText->SetRelativeLocation(FVector(0.0f, 0.0f, 185.0f));
    DamageStatusText->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    DamageStatusText->SetWorldSize(58.0f);
    DamageStatusText->SetTextRenderColor(FColor::Orange);
    DamageStatusText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    DamageStatusText->SetHiddenInGame(true);

    DamageLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("DamageLight"));
    DamageLight->SetupAttachment(RootComponent);
    DamageLight->SetRelativeLocation(FVector(0.0f, 0.0f, 100.0f));
    DamageLight->SetLightColor(FLinearColor::Red);
    DamageLight->SetAttenuationRadius(650.0f);
    DamageLight->SetIntensity(0.0f);

    UStaticMeshComponent* SmokePuffs[] = {
        SmokePuff1 = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SmokePuff1")),
        SmokePuff2 = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SmokePuff2")),
        SmokePuff3 = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SmokePuff3"))
    };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(SmokePuffs); ++Index)
    {
        UStaticMeshComponent* SmokePuff = SmokePuffs[Index];
        SmokePuff->SetupAttachment(RootComponent);
        SmokePuff->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        SmokePuff->SetRelativeLocation(FVector(-35.0f, 0.0f, 125.0f + Index * 70.0f));
        SmokePuff->SetRelativeScale3D(FVector(0.28f + Index * 0.08f));
        SmokePuff->SetHiddenInGame(true);
        if (Cube.Succeeded()) SmokePuff->SetStaticMesh(Cube.Object);
    }

    Tags.Add(TEXT("MARL.Agent"));
    Tags.Add(TEXT("MARL.Blue"));
    Tags.Add(TEXT("MARL.UGV"));
}

void ATacticalUGVPawn::BeginPlay()
{
    Super::BeginPlay();
    if (HealthComponent)
    {
        HealthComponent->OnHealthChanged.AddDynamic(this, &ATacticalUGVPawn::HandleHealthChanged);
        HealthComponent->OnDisabled.AddDynamic(this, &ATacticalUGVPawn::HandleDisabled);
        HealthComponent->OnHealthReset.AddDynamic(this, &ATacticalUGVPawn::HandleHealthReset);
    }
    if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
    {
        AgentMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        SmokeMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        ApplyVisualColor(FLinearColor(0.02f, 0.16f, 0.85f));
        SmokeMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.035f, 0.035f, 0.035f));
        TInlineComponentArray<UStaticMeshComponent*> Meshes(this);
        for (UStaticMeshComponent* Mesh : Meshes)
        {
            const bool bSmoke = Mesh == SmokePuff1 || Mesh == SmokePuff2 || Mesh == SmokePuff3;
            Mesh->SetMaterial(0, bSmoke ? SmokeMaterial : AgentMaterial);
        }
    }
    HomeLocation = GetActorLocation();
    LastLocation = HomeLocation;
    ProgressSampleLocation = HomeLocation;
    if (HasAuthority() && bAutoStartInitialCommand) ReceivePolicyCommand(InitialCommand);
    UE_LOG(LogTacticalUGV, Log, TEXT("%s ready at %s"), *AgentId.ToString(), *HomeLocation.ToCompactString());
}

void ATacticalUGVPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    CurrentVelocity = DeltaSeconds > SMALL_NUMBER ? (GetActorLocation() - LastLocation) / DeltaSeconds : FVector::ZeroVector;
    LastLocation = GetActorLocation();
    if (!HasAuthority()) return;
    if (!IsOperational())
    {
        UpdateDisabledVisual(DeltaSeconds);
        return;
    }

    TaskElapsedSeconds += DeltaSeconds;
    SensorAccumulator += DeltaSeconds;
    if (bContinuousControl) ApplyDrive(ContinuousThrottle, ContinuousSteering, bContinuousBrake, DeltaSeconds);
    else ExecuteHighLevelTask(DeltaSeconds);
    if (SensorAccumulator >= SensorScanInterval) PerformSensorScan();
}

void ATacticalUGVPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ATacticalUGVPawn, AgentId);
    DOREPLIFETIME(ATacticalUGVPawn, AssignedRole);
    DOREPLIFETIME(ATacticalUGVPawn, CurrentCommand);
    DOREPLIFETIME(ATacticalUGVPawn, TaskState);
}

bool ATacticalUGVPawn::ReceivePolicyCommand(const FTacticalUGVPolicyCommand& Command)
{
    if (!HasAuthority()) return false;
    if (!IsOperational()) return false;
    if (Command.SequenceId > 0 && CurrentCommand.SequenceId > Command.SequenceId) return false;
    CurrentCommand = Command;
    CurrentCommand.MaxSpeed = FMath::Max(50.0f, Command.MaxSpeed);
    CurrentCommand.AcceptanceRadius = FMath::Max(25.0f, Command.AcceptanceRadius);
    bContinuousControl = false;
    TaskElapsedSeconds = 0.0f;
    ExecutionElapsedSeconds = 0.0f;
    PatrolAngle = 0.0f;
    ResetNavigationState();
    TaskState = Command.Task == ETacticalUGVTask::Idle ? ETacticalUGVTaskState::Idle : ETacticalUGVTaskState::EnRoute;
    if (Command.Task == ETacticalUGVTask::Idle) CurrentSpeed = 0.0f;
    UE_LOG(LogTacticalUGV, Log, TEXT("%s accepted seq=%d task=%s"), *AgentId.ToString(), Command.SequenceId, *TaskToString(Command.Task));
    return true;
}

bool ATacticalUGVPawn::SubmitDiscreteAction(int32 Action, const FVector& Target, int32 SequenceId)
{
    FTacticalUGVPolicyCommand Command;
    Command.SequenceId = SequenceId;
    Command.TargetLocation = Target;
    switch (Action)
    {
    case 0: Command.Task = ETacticalUGVTask::Idle; break;
    case 1: Command.Task = ETacticalUGVTask::MoveTo; break;
    case 2: Command.Task = ETacticalUGVTask::Patrol; break;
    case 3: Command.Task = ETacticalUGVTask::Recon; break;
    case 4: Command.Task = ETacticalUGVTask::Surveillance; break;
    case 5: Command.Task = ETacticalUGVTask::Engage; break;
    case 6: Command.Task = ETacticalUGVTask::ReturnToBase; break;
    default: return false;
    }
    return ReceivePolicyCommand(Command);
}

bool ATacticalUGVPawn::SubmitContinuousAction(float Throttle, float Steering, bool bBrake, int32 SequenceId)
{
    if (!HasAuthority()) return false;
    if (!IsOperational()) return false;
    CurrentCommand.SequenceId = SequenceId;
    CurrentCommand.Task = ETacticalUGVTask::Idle;
    ContinuousThrottle = FMath::Clamp(Throttle, -1.0f, 1.0f);
    ContinuousSteering = FMath::Clamp(Steering, -1.0f, 1.0f);
    bContinuousBrake = bBrake;
    bContinuousControl = true;
    TaskState = ETacticalUGVTaskState::Executing;
    return true;
}

bool ATacticalUGVPawn::ReceivePolicyJson(const FString& Json, FString& OutError)
{
    if (!IsOperational())
    {
        OutError = TEXT("agent_disabled");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    {
        OutError = TEXT("Invalid JSON object"); return false;
    }
    double Number = 0.0;
    FString RoleName;
    if (Root->TryGetStringField(TEXT("role"), RoleName) && !RoleName.IsEmpty())
    {
        AssignedRole = FName(*RoleName);
    }
    if (Root->HasField(TEXT("throttle")))
    {
        const float Throttle = static_cast<float>(Root->GetNumberField(TEXT("throttle")));
        const float Steering = static_cast<float>(Root->GetNumberField(TEXT("steering")));
        const bool Brake = Root->HasField(TEXT("brake")) && Root->GetBoolField(TEXT("brake"));
        const int32 Sequence = Root->TryGetNumberField(TEXT("sequence_id"), Number) ? static_cast<int32>(Number) : 0;
        OutError.Reset(); return SubmitContinuousAction(Throttle, Steering, Brake, Sequence);
    }

    FTacticalUGVPolicyCommand Command;
    FString TaskName;
    if (!Root->TryGetStringField(TEXT("task"), TaskName) || !ParseTaskName(TaskName, Command.Task))
    {
        OutError = TEXT("Unsupported UGV task"); return false;
    }
    if (Root->TryGetNumberField(TEXT("sequence_id"), Number)) Command.SequenceId = static_cast<int32>(Number);
    const TArray<TSharedPtr<FJsonValue>>* Target = nullptr;
    if (Root->TryGetArrayField(TEXT("target"), Target) && Target && Target->Num() >= 2)
    {
        Command.TargetLocation.X = (*Target)[0]->AsNumber();
        Command.TargetLocation.Y = (*Target)[1]->AsNumber();
        Command.TargetLocation.Z = GetActorLocation().Z;
    }
    if (Root->TryGetNumberField(TEXT("speed"), Number)) Command.MaxSpeed = Number;
    if (Root->TryGetNumberField(TEXT("duration"), Number)) Command.Duration = Number;
    if (Root->TryGetNumberField(TEXT("acceptance_radius"), Number)) Command.AcceptanceRadius = Number;
    if (Root->TryGetNumberField(TEXT("patrol_radius"), Number)) Command.PatrolRadius = Number;
    OutError.Reset(); return ReceivePolicyCommand(Command);
}

void ATacticalUGVPawn::CancelCurrentTask()
{
    bContinuousControl = false;
    CurrentSpeed = 0.0f;
    TaskState = ETacticalUGVTaskState::Idle;
    CurrentCommand.Task = ETacticalUGVTask::Idle;
}

void ATacticalUGVPawn::ResetForEpisode(const FTransform& SpawnTransform)
{
    if (!HasAuthority()) return;
    bContinuousControl = false;
    ContinuousThrottle = 0.0f;
    ContinuousSteering = 0.0f;
    bContinuousBrake = false;
    CurrentSpeed = 0.0f;
    CurrentVelocity = FVector::ZeroVector;
    SetActorTransform(SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
    HomeLocation = SpawnTransform.GetLocation();
    LastLocation = HomeLocation;
    CurrentCommand = FTacticalUGVPolicyCommand();
    AssignedRole = TEXT("UnassignedRole");
    TaskState = ETacticalUGVTaskState::Idle;
    TaskElapsedSeconds = 0.0f;
    ExecutionElapsedSeconds = 0.0f;
    SensorAccumulator = 0.0f;
    PatrolAngle = 0.0f;
    DetectedActors.Reset();
    ResetNavigationState();
    ProgressSampleLocation = HomeLocation;
    DisabledVisualTime = 0.0f;
    if (HealthComponent) HealthComponent->ResetHealth();
    ClearHitFeedback();
    TurretMesh->SetRelativeRotation(FRotator::ZeroRotator);
    if (SmokePuff1) SmokePuff1->SetHiddenInGame(true);
    if (SmokePuff2) SmokePuff2->SetHiddenInGame(true);
    if (SmokePuff3) SmokePuff3->SetHiddenInGame(true);
    ForceNetUpdate();
}

FTacticalUGVTelemetry ATacticalUGVPawn::GetTelemetry() const
{
    FTacticalUGVTelemetry T;
    T.AgentId = AgentId; T.AssignedRole = AssignedRole; T.Location = GetActorLocation(); T.Velocity = CurrentVelocity; T.Rotation = GetActorRotation();
    T.Task = CurrentCommand.Task; T.TaskState = TaskState; T.SequenceId = CurrentCommand.SequenceId;
    T.DetectedTargetCount = DetectedActors.Num(); T.TaskElapsedSeconds = TaskElapsedSeconds;
    return T;
}

FString ATacticalUGVPawn::GetTelemetryJson() const
{
    const FTacticalUGVTelemetry T = GetTelemetry();
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("agent_id"), T.AgentId.ToString());
    Root->SetStringField(TEXT("agent_type"), TEXT("ugv"));
    Root->SetStringField(TEXT("role"), T.AssignedRole.ToString());
    Root->SetNumberField(TEXT("sequence_id"), T.SequenceId);
    Root->SetStringField(TEXT("task"), TaskToString(T.Task));
    Root->SetStringField(TEXT("task_state"), StateToString(T.TaskState));
    Root->SetNumberField(TEXT("task_elapsed"), T.TaskElapsedSeconds);
    Root->SetNumberField(TEXT("yaw"), T.Rotation.Yaw);
    Root->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(T.Location.X), MakeShared<FJsonValueNumber>(T.Location.Y), MakeShared<FJsonValueNumber>(T.Location.Z)});
    Root->SetArrayField(TEXT("velocity"), {MakeShared<FJsonValueNumber>(T.Velocity.X), MakeShared<FJsonValueNumber>(T.Velocity.Y), MakeShared<FJsonValueNumber>(T.Velocity.Z)});
    Root->SetNumberField(TEXT("detected_target_count"), T.DetectedTargetCount);
    Root->SetNumberField(TEXT("navigation_path_points"), NavigationPathPoints.Num());
    Root->SetNumberField(TEXT("navigation_path_index"), NavigationPathIndex);
    Root->SetBoolField(TEXT("navigation_path_partial"), bNavigationPathPartial);
    Root->SetBoolField(TEXT("navigation_start_projected"), bNavigationStartProjected);
    Root->SetBoolField(TEXT("navigation_goal_projected"), bNavigationGoalProjected);
    Root->SetNumberField(TEXT("stuck_seconds"), StuckElapsed);
    Root->SetNumberField(TEXT("avoidance_neighbors"), AvoidanceNeighborCount);
    Root->SetBoolField(TEXT("yielding_to_vehicle"), bYieldingToVehicle);
    Root->SetNumberField(TEXT("static_avoidance_steering"), StaticAvoidanceSteering);
    Root->SetNumberField(TEXT("recovery_turn_count"), RecoveryTurnCount);
    const FTacticalAgentHealthState HealthState = HealthComponent
        ? HealthComponent->GetHealthState()
        : FTacticalAgentHealthState();
    Root->SetNumberField(TEXT("health"), HealthState.Health);
    Root->SetNumberField(TEXT("max_health"), HealthState.MaxHealth);
    Root->SetBoolField(TEXT("alive"), HealthState.bAlive);
    Root->SetBoolField(TEXT("disabled"), HealthState.bDisabled);
    Root->SetStringField(TEXT("status"), HealthState.bDisabled ? TEXT("disabled") : TEXT("active"));
    Root->SetStringField(TEXT("last_damage_source"), HealthState.LastDamageSourceAgentId.ToString());
    Root->SetNumberField(TEXT("damage_event_count"), HealthState.DamageEventCount);
    Root->SetArrayField(TEXT("last_hit_direction"), {
        MakeShared<FJsonValueNumber>(HealthState.LastHitDirection.X),
        MakeShared<FJsonValueNumber>(HealthState.LastHitDirection.Y),
        MakeShared<FJsonValueNumber>(HealthState.LastHitDirection.Z)});
    TSharedRef<FJsonObject> ActionMask = MakeShared<FJsonObject>();
    const bool bCanAct = !HealthState.bDisabled;
    for (const TCHAR* Action : {TEXT("idle"), TEXT("move"), TEXT("patrol"), TEXT("recon"), TEXT("surveillance"), TEXT("engage"), TEXT("rtb")})
    {
        ActionMask->SetBoolField(Action, bCanAct);
    }
    Root->SetObjectField(TEXT("action_mask"), ActionMask);
    FString Result; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Result)); return Result;
}

TArray<AActor*> ATacticalUGVPawn::PerformSensorScan()
{
    TArray<FOverlapResult> Overlaps;
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_Pawn); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(), FQuat::Identity, Objects, FCollisionShape::MakeSphere(SensorRange), FCollisionQueryParams(SCENE_QUERY_STAT(TacticalUGVSensor), false, this));
    TSet<TWeakObjectPtr<AActor>> Current;
    TArray<AActor*> Result;
    for (const FOverlapResult& Hit : Overlaps)
    {
        AActor* Actor = Hit.GetActor();
        if (!IsValid(Actor) || Actor == this || Actor->ActorHasTag(TEXT("MARL.Blue"))) continue;
        if (!Actor->IsA<APawn>() && !Actor->ActorHasTag(TEXT("ReconTarget"))) continue;
        Current.Add(Actor); Result.AddUnique(Actor);
        if (!DetectedActors.Contains(Actor)) OnTargetDetected.Broadcast(Actor, Actor->GetActorLocation());
    }
    DetectedActors = MoveTemp(Current); SensorAccumulator = 0.0f; return Result;
}

TArray<AActor*> ATacticalUGVPawn::GetDetectedActors() const
{
    TArray<AActor*> Result;
    for (const TWeakObjectPtr<AActor>& Actor : DetectedActors)
    {
        if (Actor.IsValid()) Result.Add(Actor.Get());
    }
    return Result;
}

FVector ATacticalUGVPawn::ResolveTargetLocation() const
{
    if (CurrentCommand.Task == ETacticalUGVTask::ReturnToBase) return HomeLocation;
    return IsValid(CurrentCommand.TargetActor) ? CurrentCommand.TargetActor->GetActorLocation() : CurrentCommand.TargetLocation;
}

void ATacticalUGVPawn::ApplyDrive(float Throttle, float Steering, bool bBrake, float DeltaSeconds)
{
    const float TargetSpeed = bBrake ? 0.0f : Throttle * CurrentCommand.MaxSpeed;
    CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, TargetSpeed, DeltaSeconds, MaxAcceleration);
    const float SpeedFactor = FMath::Clamp(FMath::Abs(CurrentSpeed) / FMath::Max(CurrentCommand.MaxSpeed, 1.0f), 0.2f, 1.0f);
    AddActorWorldRotation(FRotator(0.0f, Steering * MaxTurnRate * SpeedFactor * DeltaSeconds, 0.0f));
    FHitResult Hit;
    AddActorWorldOffset(GetActorForwardVector() * CurrentSpeed * DeltaSeconds, true, &Hit);
    if (Hit.bBlockingHit) CurrentSpeed = 0.0f;
}

bool ATacticalUGVPawn::DriveTowards(const FVector& Destination, float Radius, float DeltaSeconds)
{
    const FVector EffectiveDestination = ResolveFormationDestination(Destination);
    FVector FlatOffset = EffectiveDestination - GetActorLocation(); FlatOffset.Z = 0.0f;
    if (FlatOffset.Size() <= Radius) { CurrentSpeed = 0.0f; return true; }

    NavigationRepathElapsed += DeltaSeconds;
    UpdateNavigationPath(EffectiveDestination);
    FVector SteeringTarget = GetNavigationSteeringTarget(EffectiveDestination, Radius);
    float ThrottleScale = 1.0f;
    ApplyVehicleAvoidance(SteeringTarget, ThrottleScale, DeltaSeconds);
    FVector SteeringOffset = SteeringTarget - GetActorLocation(); SteeringOffset.Z = 0.0f;
    float YawError = FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, SteeringOffset.Rotation().Yaw);

    FCollisionQueryParams ProbeParams(SCENE_QUERY_STAT(TacticalUGVObstacleProbe), false, this);
    FCollisionObjectQueryParams StaticObjects;
    StaticObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    const FVector ProbeStart = GetActorLocation();
    const auto ProbeClearance = [this, &ProbeParams, &StaticObjects, &ProbeStart](float YawOffset)
    {
        const FVector Direction = GetActorForwardVector().RotateAngleAxis(YawOffset, FVector::UpVector);
        FHitResult Hit;
        // Sweep a conservative representation of the complete vehicle rather
        // than a zero-width ray. This sees wall corners before the collision
        // box clips them and reserves enough lateral clearance for the turn.
        const FCollisionShape VehicleEnvelope = FCollisionShape::MakeBox(FVector(145.0f, 105.0f, 38.0f));
        const bool bHit = GetWorld()->SweepSingleByObjectType(
            Hit, ProbeStart, ProbeStart + Direction * ObstacleProbeDistance,
            Direction.Rotation().Quaternion(), StaticObjects, VehicleEnvelope, ProbeParams);
        return bHit ? Hit.Distance : ObstacleProbeDistance;
    };
    const float CenterClearance = ProbeClearance(0.0f);
    const float LeftClearance = FMath::Max(ProbeClearance(-40.0f), ProbeClearance(-75.0f));
    const float RightClearance = FMath::Max(ProbeClearance(40.0f), ProbeClearance(75.0f));
    StaticAvoidanceHoldRemaining = FMath::Max(0.0f, StaticAvoidanceHoldRemaining - DeltaSeconds);
    const bool bObstacleImminent = CenterClearance < ObstacleProbeDistance * 0.88f;
    if (bObstacleImminent)
    {
        // Pick a side only when entering avoidance. Retaining it prevents the
        // left/right oscillation that wedges a rectangular vehicle on a corner.
        if (StaticAvoidanceHoldRemaining <= 0.0f)
        {
            if (!FMath::IsNearlyEqual(LeftClearance, RightClearance, 25.0f))
            {
                StaticAvoidanceSteering = LeftClearance > RightClearance ? -1.0f : 1.0f;
            }
            else
            {
                StaticAvoidanceSteering = (GetTypeHash(AgentId) & 1) == 0 ? 1.0f : -1.0f;
            }
        }
        StaticAvoidanceHoldRemaining = 1.25f;
        StaticAvoidanceClearElapsed = 0.0f;
        NavigationRepathElapsed = NavigationRepathInterval;
    }
    else if (StaticAvoidanceHoldRemaining > 0.0f)
    {
        const float ChosenSideClearance = StaticAvoidanceSteering < 0.0f ? LeftClearance : RightClearance;
        StaticAvoidanceClearElapsed = ChosenSideClearance > ObstacleProbeDistance * 0.92f
            ? StaticAvoidanceClearElapsed + DeltaSeconds
            : 0.0f;
        if (StaticAvoidanceClearElapsed < 0.45f) StaticAvoidanceHoldRemaining = FMath::Max(StaticAvoidanceHoldRemaining, 0.2f);
    }

    const float GoalDistance = FVector::Dist2D(GetActorLocation(), EffectiveDestination);
    GoalProgressElapsed += DeltaSeconds;
    if (LastGoalDistance < 0.0f) LastGoalDistance = GoalDistance;
    if (GoalProgressElapsed >= 3.0f)
    {
        if (LastGoalDistance - GoalDistance < 120.0f)
        {
            const float BestOpenSide = LeftClearance > RightClearance ? -1.0f : 1.0f;
            StaticAvoidanceSteering = !FMath::IsNearlyZero(StaticAvoidanceSteering)
                ? -StaticAvoidanceSteering
                : BestOpenSide;
            StaticAvoidanceHoldRemaining = 1.5f;
            StaticAvoidanceClearElapsed = 0.0f;
            ++RecoveryTurnCount;
            UpdateNavigationPath(EffectiveDestination, true);
        }
        LastGoalDistance = GoalDistance;
        GoalProgressElapsed = 0.0f;
    }

    if (StaticAvoidanceHoldRemaining > 0.0f)
    {
        // Override the target pull while clearing the corner. Adding the two
        // angles allowed the goal steering to cancel avoidance at 90-degree walls.
        YawError = 78.0f * StaticAvoidanceSteering;
        ThrottleScale = FMath::Min(ThrottleScale, CenterClearance < 260.0f ? 0.28f : 0.52f);
    }
    else
    {
        StaticAvoidanceSteering = 0.0f;
    }

    if (bYieldingToVehicle)
    {
        StuckElapsed = 0.0f;
        ProgressSampleLocation = GetActorLocation();
    }
    else if (FVector::DistSquared2D(GetActorLocation(), ProgressSampleLocation) < FMath::Square(25.0f)) StuckElapsed += DeltaSeconds;
    else { StuckElapsed = 0.0f; ProgressSampleLocation = GetActorLocation(); }
    if (StuckElapsed > 1.5f)
    {
        const float EscapeSign = !FMath::IsNearlyZero(StaticAvoidanceSteering)
            ? StaticAvoidanceSteering
            : ((GetTypeHash(AgentId) & 1) == 0 ? 1.0f : -1.0f);
        ApplyDrive(-0.35f, EscapeSign, false, DeltaSeconds);
        if (StuckElapsed > 2.2f) { StuckElapsed = 0.0f; UpdateNavigationPath(EffectiveDestination, true); }
        return false;
    }

    const float Steering = FMath::Clamp(YawError / 45.0f, -1.0f, 1.0f);
    const float Throttle = (FMath::Abs(YawError) > 100.0f ? 0.15f : 1.0f) * ThrottleScale;
    ApplyDrive(Throttle, Steering, bYieldingToVehicle, DeltaSeconds); return false;
}

FVector ATacticalUGVPawn::ResolveFormationDestination(const FVector& Destination) const
{
    if (CurrentCommand.Task == ETacticalUGVTask::ReturnToBase) return Destination;

    TArray<const ATacticalUGVPawn*> Cohort;
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        const ATacticalUGVPawn* Other = *It;
        if (!IsValid(Other) || Other->TaskState == ETacticalUGVTaskState::Idle || Other->bContinuousControl) continue;
        if (FVector::DistSquared2D(Other->ResolveTargetLocation(), Destination) <= FMath::Square(800.0f)) Cohort.Add(Other);
    }
    if (Cohort.Num() < 2) return Destination;

    Cohort.Sort([](const ATacticalUGVPawn& Left, const ATacticalUGVPawn& Right)
    {
        return Left.AgentId.LexicalLess(Right.AgentId);
    });
    const int32 Rank = Cohort.IndexOfByKey(this);
    if (Rank == INDEX_NONE) return Destination;

    FVector Approach = Destination - HomeLocation;
    Approach.Z = 0.0f;
    Approach = Approach.GetSafeNormal();
    if (Approach.IsNearlyZero()) Approach = GetActorForwardVector().GetSafeNormal2D();
    const FVector Right(-Approach.Y, Approach.X, 0.0f);
    const float CenteredSlot = static_cast<float>(Rank) - (static_cast<float>(Cohort.Num() - 1) * 0.5f);
    return Destination + Right * CenteredSlot * FormationSpacing;
}

void ATacticalUGVPawn::ApplyVehicleAvoidance(FVector& SteeringTarget, float& ThrottleScale, float DeltaSeconds)
{
    AvoidanceNeighborCount = 0;
    bYieldingToVehicle = false;
    YieldHoldRemaining = FMath::Max(0.0f, YieldHoldRemaining - DeltaSeconds);

    FVector Separation = FVector::ZeroVector;
    const FVector MyLocation = GetActorLocation();
    const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        const ATacticalUGVPawn* Other = *It;
        if (!IsValid(Other) || Other == this) continue;

        FVector ToOther = Other->GetActorLocation() - MyLocation;
        ToOther.Z = 0.0f;
        const float Distance = ToOther.Size();
        if (Distance <= KINDA_SMALL_NUMBER || Distance >= VehicleAvoidanceRadius) continue;

        ++AvoidanceNeighborCount;
        const FVector DirectionToOther = ToOther / Distance;
        const float Weight = 1.0f - Distance / VehicleAvoidanceRadius;
        Separation -= DirectionToOther * Weight;

        const bool bOtherAhead = FVector::DotProduct(Forward, DirectionToOther) > 0.15f;
        const bool bOtherHasPriority = AgentId.ToString().Compare(Other->AgentId.ToString()) > 0;
        if (bOtherAhead && bOtherHasPriority && Distance < VehicleYieldDistance)
        {
            YieldHoldRemaining = 0.45f;
        }
    }

    if (!Separation.IsNearlyZero()) SteeringTarget += Separation.GetSafeNormal() * 420.0f;
    if (YieldHoldRemaining > 0.0f)
    {
        bYieldingToVehicle = true;
        ThrottleScale = 0.0f;
    }
    else if (AvoidanceNeighborCount > 0)
    {
        ThrottleScale = 0.65f;
    }
}

void ATacticalUGVPawn::UpdateNavigationPath(const FVector& Destination, bool bForce)
{
    if (!bUseNavMeshPathfinding)
    {
        NavigationPathPoints.Reset();
        return;
    }
    const bool bGoalChanged = FVector::DistSquared2D(NavigationGoal, Destination) > FMath::Square(200.0f);
    if (!bForce && !bGoalChanged && NavigationPathPoints.Num() > 0 && NavigationRepathElapsed < NavigationRepathInterval) return;

    NavigationRepathElapsed = 0.0f;
    NavigationGoal = Destination;
    NavigationPathPoints.Reset();
    NavigationPathIndex = 0;
    bNavigationPathPartial = false;
    bNavigationStartProjected = false;
    bNavigationGoalProjected = false;

    UNavigationSystemV1* NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavigationSystem) return;

    // The UGV spawn pads and task markers are deliberately placed beside the
    // drivable surface. Project both ends to a generous search extent before
    // asking Recast for a route; querying the raw actor/marker coordinates can
    // otherwise fail even though a valid road polygon is only metres away.
    const FVector ProjectionExtent(2500.0f, 2500.0f, 1500.0f);
    FNavLocation ProjectedStart;
    FNavLocation ProjectedGoal;
    bNavigationStartProjected = NavigationSystem->ProjectPointToNavigation(
        GetActorLocation(), ProjectedStart, ProjectionExtent);
    bNavigationGoalProjected = NavigationSystem->ProjectPointToNavigation(
        Destination, ProjectedGoal, ProjectionExtent);
    if (!bNavigationStartProjected || !bNavigationGoalProjected) return;

    if (UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
        GetWorld(), ProjectedStart.Location, ProjectedGoal.Location, this))
    {
        // A command target can sit just outside the walkable polygon (for
        // example beside a container). Follow the reachable part, then let
        // the local probe/escape steering close the final short distance.
        if (Path->IsValid() && Path->PathPoints.Num() > 1)
        {
            NavigationPathPoints = Path->PathPoints;
            NavigationPathIndex = NavigationPathPoints.Num() > 1 ? 1 : 0;
            bNavigationPathPartial = Path->IsPartial();
        }
    }
}

FVector ATacticalUGVPawn::GetNavigationSteeringTarget(const FVector& Destination, float AcceptanceRadius)
{
    if (NavigationPathPoints.Num() == 0) return Destination;
    const float PointRadius = FMath::Max(150.0f, FMath::Min(AcceptanceRadius, 300.0f));
    while (NavigationPathIndex < NavigationPathPoints.Num() - 1 &&
        FVector::DistSquared2D(GetActorLocation(), NavigationPathPoints[NavigationPathIndex]) <= FMath::Square(PointRadius))
    {
        ++NavigationPathIndex;
    }
    // A partial path only leads to the edge of the currently connected nav
    // island. Once that edge is reached, continue toward the requested point
    // under local obstacle/vehicle avoidance instead of parking forever on
    // the final Recast point with the rest of the convoy.
    if (bNavigationPathPartial && NavigationPathIndex == NavigationPathPoints.Num() - 1 &&
        FVector::DistSquared2D(GetActorLocation(), NavigationPathPoints.Last()) <= FMath::Square(PointRadius))
    {
        return Destination;
    }
    return NavigationPathPoints.IsValidIndex(NavigationPathIndex) ? NavigationPathPoints[NavigationPathIndex] : Destination;
}

void ATacticalUGVPawn::ResetNavigationState()
{
    NavigationPathPoints.Reset();
    NavigationPathIndex = 0;
    NavigationGoal = FVector::ZeroVector;
    bNavigationPathPartial = false;
    bNavigationStartProjected = false;
    bNavigationGoalProjected = false;
    NavigationRepathElapsed = NavigationRepathInterval;
    StuckElapsed = 0.0f;
    AvoidanceNeighborCount = 0;
    bYieldingToVehicle = false;
    YieldHoldRemaining = 0.0f;
    StaticAvoidanceHoldRemaining = 0.0f;
    StaticAvoidanceSteering = 0.0f;
    StaticAvoidanceClearElapsed = 0.0f;
    GoalProgressElapsed = 0.0f;
    LastGoalDistance = -1.0f;
    RecoveryTurnCount = 0;
}

void ATacticalUGVPawn::ExecuteHighLevelTask(float DeltaSeconds)
{
    if (TaskState == ETacticalUGVTaskState::Idle || TaskState == ETacticalUGVTaskState::Completed || TaskState == ETacticalUGVTaskState::Failed) return;
    const FVector Target = ResolveTargetLocation();
    if (TaskState == ETacticalUGVTaskState::EnRoute)
    {
        const float Radius = CurrentCommand.Task == ETacticalUGVTask::Engage ? EngageRange : CurrentCommand.AcceptanceRadius;
        if (!DriveTowards(Target, Radius, DeltaSeconds)) return;
        TaskState = ETacticalUGVTaskState::Executing; ExecutionElapsedSeconds = 0.0f;
    }
    ExecutionElapsedSeconds += DeltaSeconds;
    if (CurrentCommand.Task == ETacticalUGVTask::MoveTo || CurrentCommand.Task == ETacticalUGVTask::ReturnToBase) CompleteTask();
    else if (CurrentCommand.Task == ETacticalUGVTask::Engage) { ExecuteEngage(); CompleteTask(); }
    else if (CurrentCommand.Task == ETacticalUGVTask::Patrol || CurrentCommand.Task == ETacticalUGVTask::Recon || CurrentCommand.Task == ETacticalUGVTask::Surveillance)
    {
        PatrolAngle += DeltaSeconds * 0.35f;
        const FVector PatrolPoint = Target + FVector(FMath::Cos(PatrolAngle), FMath::Sin(PatrolAngle), 0.0f) * CurrentCommand.PatrolRadius;
        DriveTowards(PatrolPoint, 120.0f, DeltaSeconds);
        if (CurrentCommand.Duration <= 0.0f || ExecutionElapsedSeconds >= CurrentCommand.Duration) CompleteTask();
    }
}

void ATacticalUGVPawn::CompleteTask()
{
    CurrentSpeed = 0.0f; TaskState = ETacticalUGVTaskState::Completed;
    OnTaskCompleted.Broadcast(CurrentCommand.SequenceId, CurrentCommand.Task);
    UE_LOG(LogTacticalUGV, Log, TEXT("%s completed seq=%d task=%s"), *AgentId.ToString(), CurrentCommand.SequenceId, *TaskToString(CurrentCommand.Task));
}

void ATacticalUGVPawn::ExecuteEngage()
{
    const FVector Location = ResolveTargetLocation();
    OnEngageExecuted.Broadcast(Location, CurrentCommand.TargetActor);
    if (UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        Mission->ReportEngagement(AgentId, this, CurrentCommand.TargetActor, Location, EngageDamage, bApplyEngageDamage);
    }
    UE_LOG(LogTacticalUGV, Log, TEXT("%s simulated engage at %s"), *AgentId.ToString(), *Location.ToCompactString());
}

void ATacticalUGVPawn::HandleHealthChanged(UTacticalAgentHealthComponent* Component, float OldHealth, float NewHealth, AActor* DamageSource)
{
    if (!Component || NewHealth >= OldHealth || Component->IsDisabled()) return;
    DamageStatusText->SetText(FText::FromString(FString::Printf(TEXT("HIT -%.0f  |  HP %.0f/%.0f"), OldHealth - NewHealth, NewHealth, Component->MaxHealth)));
    DamageStatusText->SetTextRenderColor(FColor::Orange);
    DamageStatusText->SetHiddenInGame(false);
    DamageLight->SetIntensity(16000.0f);
    ApplyVisualColor(FLinearColor(1.0f, 0.08f, 0.01f));
    GetWorldTimerManager().ClearTimer(HitFeedbackTimer);
    GetWorldTimerManager().SetTimer(HitFeedbackTimer, this, &ATacticalUGVPawn::ClearHitFeedback, 0.8f, false);
}

void ATacticalUGVPawn::HandleDisabled(UTacticalAgentHealthComponent* Component, AActor* DamageSource)
{
    bContinuousControl = false;
    CurrentSpeed = 0.0f;
    CurrentVelocity = FVector::ZeroVector;
    CurrentCommand.Task = ETacticalUGVTask::Idle;
    TaskState = ETacticalUGVTaskState::Failed;
    DisabledVisualTime = 0.0f;
    GetWorldTimerManager().ClearTimer(HitFeedbackTimer);
    DamageStatusText->SetText(FText::FromString(TEXT("DISABLED  |  PARKED / SMOKE")));
    DamageStatusText->SetTextRenderColor(FColor::Red);
    DamageStatusText->SetHiddenInGame(false);
    DamageLight->SetIntensity(9000.0f);
    ApplyVisualColor(FLinearColor(0.12f, 0.015f, 0.01f));
    TurretMesh->SetRelativeRotation(FRotator(0.0f, 25.0f, 12.0f));
    OnTaskFailed.Broadcast(CurrentCommand.SequenceId, CurrentCommand.Task);
    ForceNetUpdate();
}

void ATacticalUGVPawn::HandleHealthReset(UTacticalAgentHealthComponent* Component)
{
    DisabledVisualTime = 0.0f;
    ClearHitFeedback();
    TurretMesh->SetRelativeRotation(FRotator::ZeroRotator);
    if (SmokePuff1) SmokePuff1->SetHiddenInGame(true);
    if (SmokePuff2) SmokePuff2->SetHiddenInGame(true);
    if (SmokePuff3) SmokePuff3->SetHiddenInGame(true);
}

void ATacticalUGVPawn::ClearHitFeedback()
{
    if (!HealthComponent || !HealthComponent->IsDisabled())
    {
        if (DamageStatusText) DamageStatusText->SetHiddenInGame(true);
        if (DamageLight) DamageLight->SetIntensity(0.0f);
        ApplyVisualColor(FLinearColor(0.02f, 0.16f, 0.85f));
    }
}

void ATacticalUGVPawn::ApplyVisualColor(const FLinearColor& Color)
{
    if (AgentMaterial) AgentMaterial->SetVectorParameterValue(TEXT("Color"), Color);
}

void ATacticalUGVPawn::UpdateDisabledVisual(float DeltaSeconds)
{
    CurrentSpeed = 0.0f;
    CurrentVelocity = FVector::ZeroVector;
    DisabledVisualTime += DeltaSeconds;
    DamageLight->SetIntensity(6500.0f + 2500.0f * (0.5f + 0.5f * FMath::Sin(DisabledVisualTime * 5.0f)));
    UStaticMeshComponent* SmokePuffs[] = {SmokePuff1, SmokePuff2, SmokePuff3};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(SmokePuffs); ++Index)
    {
        UStaticMeshComponent* SmokePuff = SmokePuffs[Index];
        if (!SmokePuff) continue;
        SmokePuff->SetHiddenInGame(false);
        const float Phase = DisabledVisualTime * (0.8f + Index * 0.17f) + Index * 1.8f;
        const float Scale = 0.25f + Index * 0.08f + 0.07f * (0.5f + 0.5f * FMath::Sin(Phase));
        SmokePuff->SetRelativeScale3D(FVector(Scale));
        SmokePuff->SetRelativeLocation(FVector(-35.0f + FMath::Sin(Phase) * 22.0f, FMath::Cos(Phase * 0.7f) * 18.0f, 125.0f + Index * 70.0f));
    }
}

bool ATacticalUGVPawn::IsOperational() const
{
    return !HealthComponent || !HealthComponent->IsDisabled();
}

bool ATacticalUGVPawn::ParseTaskName(const FString& Name, ETacticalUGVTask& Out)
{
    const FString N = Name.ToLower();
    if (N == TEXT("idle")) Out = ETacticalUGVTask::Idle;
    else if (N == TEXT("move") || N == TEXT("move_to")) Out = ETacticalUGVTask::MoveTo;
    else if (N == TEXT("patrol")) Out = ETacticalUGVTask::Patrol;
    else if (N == TEXT("recon")) Out = ETacticalUGVTask::Recon;
    else if (N == TEXT("surveillance") || N == TEXT("observe")) Out = ETacticalUGVTask::Surveillance;
    else if (N == TEXT("engage") || N == TEXT("attack")) Out = ETacticalUGVTask::Engage;
    else if (N == TEXT("rtb") || N == TEXT("return_to_base")) Out = ETacticalUGVTask::ReturnToBase;
    else return false; return true;
}

FString ATacticalUGVPawn::TaskToString(ETacticalUGVTask Task)
{
    switch (Task) { case ETacticalUGVTask::MoveTo:return TEXT("move"); case ETacticalUGVTask::Patrol:return TEXT("patrol"); case ETacticalUGVTask::Recon:return TEXT("recon"); case ETacticalUGVTask::Surveillance:return TEXT("surveillance"); case ETacticalUGVTask::Engage:return TEXT("engage"); case ETacticalUGVTask::ReturnToBase:return TEXT("rtb"); default:return TEXT("idle"); }
}

FString ATacticalUGVPawn::StateToString(ETacticalUGVTaskState State)
{
    switch (State) { case ETacticalUGVTaskState::EnRoute:return TEXT("en_route"); case ETacticalUGVTaskState::Executing:return TEXT("executing"); case ETacticalUGVTaskState::Completed:return TEXT("completed"); case ETacticalUGVTaskState::Failed:return TEXT("failed"); default:return TEXT("idle"); }
}
