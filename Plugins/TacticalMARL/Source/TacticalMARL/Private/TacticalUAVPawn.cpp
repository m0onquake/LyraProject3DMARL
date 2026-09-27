#include "TacticalUAVPawn.h"
#include "TacticalMARLMissionSubsystem.h"

#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalUAV, Log, All);

ATacticalUAVPawn::ATacticalUAVPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    SetReplicates(true);
    SetReplicateMovement(true);

    CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
    CollisionComponent->InitSphereRadius(95.0f);
    CollisionComponent->SetCollisionProfileName(TEXT("Pawn"));
    RootComponent = CollisionComponent;

    BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    BodyMesh->SetupAttachment(RootComponent);
    BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BodyMesh->SetRelativeScale3D(FVector(1.2f, 1.2f, 0.25f));

    static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (CylinderMesh.Succeeded())
    {
        BodyMesh->SetStaticMesh(CylinderMesh.Object);
    }

    const FVector RotorOffsets[] =
    {
        FVector(115.0f, 115.0f, 10.0f),
        FVector(115.0f, -115.0f, 10.0f),
        FVector(-115.0f, 115.0f, 10.0f),
        FVector(-115.0f, -115.0f, 10.0f)
    };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(RotorOffsets); ++Index)
    {
        const FName ComponentName(*FString::Printf(TEXT("Rotor_%d"), Index + 1));
        UStaticMeshComponent* Rotor = CreateDefaultSubobject<UStaticMeshComponent>(ComponentName);
        Rotor->SetupAttachment(RootComponent);
        Rotor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Rotor->SetRelativeLocation(RotorOffsets[Index]);
        Rotor->SetRelativeScale3D(FVector(1.0f, 1.0f, 0.06f));
        if (CylinderMesh.Succeeded())
        {
            Rotor->SetStaticMesh(CylinderMesh.Object);
        }
    }

    if (CubeMesh.Succeeded())
    {
        UStaticMeshComponent* ForwardMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ForwardMarker"));
        ForwardMarker->SetupAttachment(RootComponent);
        ForwardMarker->SetStaticMesh(CubeMesh.Object);
        ForwardMarker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ForwardMarker->SetRelativeLocation(FVector(100.0f, 0.0f, -10.0f));
        ForwardMarker->SetRelativeScale3D(FVector(0.7f, 0.15f, 0.1f));
    }

    ReconCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ReconCamera"));
    ReconCamera->SetupAttachment(RootComponent);
    ReconCamera->SetRelativeLocation(FVector(45.0f, 0.0f, -45.0f));
    ReconCamera->SetRelativeRotation(FRotator(-55.0f, 0.0f, 0.0f));
    ReconCamera->FieldOfView = 75.0f;

    MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
    MovementComponent->SetUpdatedComponent(CollisionComponent);
    MovementComponent->MaxSpeed = 1200.0f;
    MovementComponent->Acceleration = 2400.0f;
    MovementComponent->Deceleration = 3200.0f;
    MovementComponent->TurningBoost = 8.0f;

    Tags.Add(TEXT("MARL.Agent"));
    Tags.Add(TEXT("MARL.Blue"));
    Tags.Add(TEXT("MARL.UAV"));
}

void ATacticalUAVPawn::BeginPlay()
{
    Super::BeginPlay();
    HomeLocation = GetActorLocation();
    UE_LOG(LogTacticalUAV, Log, TEXT("%s (%s) ready at %s"), *GetName(), *AgentId.ToString(), *HomeLocation.ToCompactString());
    if (HasAuthority() && bAutoStartInitialCommand)
    {
        ReceivePolicyCommand(InitialCommand);
    }
}

void ATacticalUAVPawn::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (!HasAuthority())
    {
        return;
    }

    TaskElapsedSeconds += DeltaSeconds;
    SensorAccumulator += DeltaSeconds;
    if (bContinuousControl)
    {
        AddMovementInput(ContinuousMoveInput.GetClampedToMaxSize(1.0f), 1.0f, true);
        AddActorWorldRotation(FRotator(0.0f, ContinuousYawRate * DeltaSeconds, 0.0f));
        if (SensorAccumulator >= SensorScanInterval)
        {
            PerformSensorScan();
        }
        return;
    }
    ExecuteCurrentTask(DeltaSeconds);
}

UPawnMovementComponent* ATacticalUAVPawn::GetMovementComponent() const
{
    return MovementComponent;
}

void ATacticalUAVPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ATacticalUAVPawn, AgentId);
    DOREPLIFETIME(ATacticalUAVPawn, AssignedRole);
    DOREPLIFETIME(ATacticalUAVPawn, CurrentCommand);
    DOREPLIFETIME(ATacticalUAVPawn, TaskState);
}

bool ATacticalUAVPawn::ReceivePolicyCommand(const FTacticalUAVPolicyCommand& Command)
{
    if (!HasAuthority())
    {
        ServerReceivePolicyCommand(Command);
        return true;
    }

    if (Command.SequenceId > 0 && CurrentCommand.SequenceId > Command.SequenceId)
    {
        return false;
    }

    CurrentCommand = Command;
    bContinuousControl = false;
    CurrentCommand.MaxSpeed = FMath::Max(100.0f, CurrentCommand.MaxSpeed);
    CurrentCommand.AcceptanceRadius = FMath::Max(25.0f, CurrentCommand.AcceptanceRadius);
    CurrentCommand.OrbitRadius = FMath::Max(100.0f, CurrentCommand.OrbitRadius);
    CurrentCommand.Duration = FMath::Max(0.0f, CurrentCommand.Duration);
    MovementComponent->MaxSpeed = CurrentCommand.MaxSpeed;

    TaskElapsedSeconds = 0.0f;
    ExecutionElapsedSeconds = 0.0f;
    SensorAccumulator = SensorScanInterval;
    OrbitAngleRadians = 0.0f;

    if (CurrentCommand.Task == ETacticalUAVTask::Idle)
    {
        SetTaskState(ETacticalUAVTaskState::Idle);
        MovementComponent->StopMovementImmediately();
    }
    else
    {
        SetTaskState(ETacticalUAVTaskState::EnRoute);
    }
    UE_LOG(
        LogTacticalUAV,
        Log,
        TEXT("%s accepted policy seq=%d task=%s target=%s"),
        *AgentId.ToString(),
        CurrentCommand.SequenceId,
        *TaskToString(CurrentCommand.Task),
        *ResolveTargetLocation().ToCompactString());
    return true;
}

void ATacticalUAVPawn::ServerReceivePolicyCommand_Implementation(const FTacticalUAVPolicyCommand& Command)
{
    ReceivePolicyCommand(Command);
}

bool ATacticalUAVPawn::SubmitDiscreteAction(const int32 Action, const FVector& TargetLocation, const int32 SequenceId)
{
    FTacticalUAVPolicyCommand Command;
    Command.SequenceId = SequenceId;
    Command.TargetLocation = TargetLocation;
    Command.DesiredAltitude = TargetLocation.Z > 0.0f ? TargetLocation.Z : 800.0f;

    switch (Action)
    {
    case 0: Command.Task = ETacticalUAVTask::Idle; break;
    case 1: Command.Task = ETacticalUAVTask::MoveTo; break;
    case 2: Command.Task = ETacticalUAVTask::Recon; break;
    case 3: Command.Task = ETacticalUAVTask::Surveillance; break;
    case 4: Command.Task = ETacticalUAVTask::Strike; break;
    case 5: Command.Task = ETacticalUAVTask::ReturnToBase; break;
    default: return false;
    }
    return ReceivePolicyCommand(Command);
}

bool ATacticalUAVPawn::SubmitContinuousAction(const FVector& MoveInput, const float YawRate, const int32 SequenceId)
{
    if (!HasAuthority())
    {
        return false;
    }

    CurrentCommand.SequenceId = SequenceId;
    CurrentCommand.Task = ETacticalUAVTask::Idle;
    bContinuousControl = true;
    ContinuousMoveInput = MoveInput.GetClampedToMaxSize(1.0f);
    ContinuousYawRate = FMath::Clamp(YawRate, -180.0f, 180.0f);
    TaskElapsedSeconds = 0.0f;
    SetTaskState(ETacticalUAVTaskState::Executing);
    return true;
}

bool ATacticalUAVPawn::ReceivePolicyJson(const FString& JsonCommand, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("Invalid JSON object");
        return false;
    }

    FTacticalUAVPolicyCommand Command;
    double NumberValue = 0.0;
    FString RoleName;
    if (Root->TryGetStringField(TEXT("role"), RoleName) && !RoleName.IsEmpty())
    {
        AssignedRole = FName(*RoleName);
    }
    if (Root->TryGetNumberField(TEXT("sequence_id"), NumberValue))
    {
        Command.SequenceId = static_cast<int32>(NumberValue);
    }

    FString TaskName;
    if (!Root->TryGetStringField(TEXT("task"), TaskName) || !ParseTaskName(TaskName, Command.Task))
    {
        OutError = TEXT("Missing or unsupported task. Use idle, move, recon, surveillance, strike, or rtb.");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* TargetArray = nullptr;
    if (Root->TryGetArrayField(TEXT("target"), TargetArray) && TargetArray && TargetArray->Num() >= 2)
    {
        Command.TargetLocation.X = static_cast<float>((*TargetArray)[0]->AsNumber());
        Command.TargetLocation.Y = static_cast<float>((*TargetArray)[1]->AsNumber());
        Command.TargetLocation.Z = TargetArray->Num() >= 3
            ? static_cast<float>((*TargetArray)[2]->AsNumber())
            : 0.0f;
    }

    if (Root->TryGetNumberField(TEXT("altitude"), NumberValue)) Command.DesiredAltitude = static_cast<float>(NumberValue);
    else if (Command.TargetLocation.Z > 0.0f) Command.DesiredAltitude = Command.TargetLocation.Z;
    if (Root->TryGetNumberField(TEXT("speed"), NumberValue)) Command.MaxSpeed = static_cast<float>(NumberValue);
    if (Root->TryGetNumberField(TEXT("acceptance_radius"), NumberValue)) Command.AcceptanceRadius = static_cast<float>(NumberValue);
    if (Root->TryGetNumberField(TEXT("duration"), NumberValue)) Command.Duration = static_cast<float>(NumberValue);
    if (Root->TryGetNumberField(TEXT("orbit_radius"), NumberValue)) Command.OrbitRadius = static_cast<float>(NumberValue);

    OutError.Reset();
    return ReceivePolicyCommand(Command);
}

void ATacticalUAVPawn::CancelCurrentTask()
{
    bContinuousControl = false;
    FTacticalUAVPolicyCommand StopCommand;
    StopCommand.SequenceId = CurrentCommand.SequenceId + 1;
    StopCommand.Task = ETacticalUAVTask::Idle;
    ReceivePolicyCommand(StopCommand);
}

void ATacticalUAVPawn::ResetForEpisode(const FTransform& SpawnTransform)
{
    if (!HasAuthority()) return;
    bContinuousControl = false;
    ContinuousMoveInput = FVector::ZeroVector;
    ContinuousYawRate = 0.0f;
    MovementComponent->StopMovementImmediately();
    SetActorTransform(SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
    HomeLocation = SpawnTransform.GetLocation();
    CurrentCommand = FTacticalUAVPolicyCommand();
    AssignedRole = TEXT("UnassignedRole");
    TaskState = ETacticalUAVTaskState::Idle;
    TaskElapsedSeconds = 0.0f;
    ExecutionElapsedSeconds = 0.0f;
    SensorAccumulator = 0.0f;
    OrbitAngleRadians = 0.0f;
    DetectedActors.Reset();
    ForceNetUpdate();
}

FTacticalUAVTelemetry ATacticalUAVPawn::GetTelemetry() const
{
    FTacticalUAVTelemetry Result;
    Result.AgentId = AgentId;
    Result.AssignedRole = AssignedRole;
    Result.Location = GetActorLocation();
    Result.Velocity = GetVelocity();
    Result.Task = CurrentCommand.Task;
    Result.TaskState = TaskState;
    Result.SequenceId = CurrentCommand.SequenceId;
    Result.DetectedTargetCount = DetectedActors.Num();
    Result.TaskElapsedSeconds = TaskElapsedSeconds;
    return Result;
}

FString ATacticalUAVPawn::GetTelemetryJson() const
{
    const FTacticalUAVTelemetry Telemetry = GetTelemetry();
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("agent_id"), Telemetry.AgentId.ToString());
    Root->SetStringField(TEXT("agent_type"), TEXT("uav"));
    Root->SetStringField(TEXT("role"), Telemetry.AssignedRole.ToString());
    Root->SetNumberField(TEXT("sequence_id"), Telemetry.SequenceId);
    Root->SetStringField(TEXT("task"), TaskToString(Telemetry.Task));
    Root->SetStringField(TEXT("task_state"), TaskStateToString(Telemetry.TaskState));
    Root->SetNumberField(TEXT("task_elapsed"), Telemetry.TaskElapsedSeconds);

    TArray<TSharedPtr<FJsonValue>> Location;
    Location.Add(MakeShared<FJsonValueNumber>(Telemetry.Location.X));
    Location.Add(MakeShared<FJsonValueNumber>(Telemetry.Location.Y));
    Location.Add(MakeShared<FJsonValueNumber>(Telemetry.Location.Z));
    Root->SetArrayField(TEXT("location"), Location);

    TArray<TSharedPtr<FJsonValue>> Velocity;
    Velocity.Add(MakeShared<FJsonValueNumber>(Telemetry.Velocity.X));
    Velocity.Add(MakeShared<FJsonValueNumber>(Telemetry.Velocity.Y));
    Velocity.Add(MakeShared<FJsonValueNumber>(Telemetry.Velocity.Z));
    Root->SetArrayField(TEXT("velocity"), Velocity);

    TArray<TSharedPtr<FJsonValue>> Targets;
    for (const TWeakObjectPtr<AActor>& ActorPtr : DetectedActors)
    {
        if (const AActor* Actor = ActorPtr.Get())
        {
            Targets.Add(MakeShared<FJsonValueString>(Actor->GetName()));
        }
    }
    Root->SetArrayField(TEXT("detected_targets"), Targets);
    Root->SetNumberField(TEXT("detected_target_count"), Telemetry.DetectedTargetCount);

    FString Result;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
    FJsonSerializer::Serialize(Root, Writer);
    return Result;
}

TArray<AActor*> ATacticalUAVPawn::PerformSensorScan()
{
    TArray<FOverlapResult> Overlaps;
    FCollisionObjectQueryParams ObjectQuery;
    ObjectQuery.AddObjectTypesToQuery(ECC_Pawn);
    ObjectQuery.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TacticalUAVSensor), false, this);
    GetWorld()->OverlapMultiByObjectType(
        Overlaps,
        GetActorLocation(),
        FQuat::Identity,
        ObjectQuery,
        FCollisionShape::MakeSphere(SensorRange),
        QueryParams);

    TSet<TWeakObjectPtr<AActor>> CurrentDetections;
    TArray<AActor*> Result;
    for (const FOverlapResult& Overlap : Overlaps)
    {
        AActor* Actor = Overlap.GetActor();
        if (!IsValid(Actor) || Actor == this || Actor->ActorHasTag(TEXT("MARL.Blue")))
        {
            continue;
        }
        if (!Actor->IsA<APawn>() && !Actor->ActorHasTag(TEXT("ReconTarget")))
        {
            continue;
        }

        CurrentDetections.Add(Actor);
        Result.AddUnique(Actor);
        if (!DetectedActors.Contains(Actor))
        {
            OnTargetDetected.Broadcast(Actor, Actor->GetActorLocation());
        }
    }

    DetectedActors = MoveTemp(CurrentDetections);
    SensorAccumulator = 0.0f;
    return Result;
}

TArray<AActor*> ATacticalUAVPawn::GetDetectedActors() const
{
    TArray<AActor*> Result;
    for (const TWeakObjectPtr<AActor>& ActorPtr : DetectedActors)
    {
        if (AActor* Actor = ActorPtr.Get())
        {
            Result.Add(Actor);
        }
    }
    return Result;
}

void ATacticalUAVPawn::OnRep_CurrentCommand()
{
    MovementComponent->MaxSpeed = CurrentCommand.MaxSpeed;
}

FVector ATacticalUAVPawn::ResolveTargetLocation() const
{
    FVector Target = IsValid(CurrentCommand.TargetActor)
        ? CurrentCommand.TargetActor->GetActorLocation()
        : CurrentCommand.TargetLocation;
    if (CurrentCommand.Task == ETacticalUAVTask::ReturnToBase)
    {
        Target = HomeLocation;
    }
    if (CurrentCommand.DesiredAltitude > 0.0f)
    {
        Target.Z = CurrentCommand.DesiredAltitude;
    }
    return Target;
}

bool ATacticalUAVPawn::FlyTowards(const FVector& Destination, const float AcceptanceRadius)
{
    const FVector Offset = Destination - GetActorLocation();
    if (Offset.Size() <= AcceptanceRadius)
    {
        MovementComponent->StopMovementImmediately();
        return true;
    }

    const FVector Direction = Offset.GetSafeNormal();
    AddMovementInput(Direction, 1.0f, true);
    const FRotator Facing(0.0f, Direction.Rotation().Yaw, 0.0f);
    SetActorRotation(FMath::RInterpTo(GetActorRotation(), Facing, GetWorld()->GetDeltaSeconds(), 4.0f));
    return false;
}

void ATacticalUAVPawn::ExecuteCurrentTask(const float DeltaSeconds)
{
    if (TaskState == ETacticalUAVTaskState::Idle ||
        TaskState == ETacticalUAVTaskState::Completed ||
        TaskState == ETacticalUAVTaskState::Failed)
    {
        return;
    }

    const FVector Target = ResolveTargetLocation();
    if (TaskState == ETacticalUAVTaskState::EnRoute)
    {
        float ArrivalRadius = CurrentCommand.AcceptanceRadius;
        if (CurrentCommand.Task == ETacticalUAVTask::Strike)
        {
            ArrivalRadius = StrikeRange;
        }

        if (!FlyTowards(Target, ArrivalRadius))
        {
            return;
        }

        SetTaskState(ETacticalUAVTaskState::Executing);
        ExecutionElapsedSeconds = 0.0f;
    }

    ExecutionElapsedSeconds += DeltaSeconds;
    switch (CurrentCommand.Task)
    {
    case ETacticalUAVTask::MoveTo:
    case ETacticalUAVTask::ReturnToBase:
        CompleteCurrentTask();
        break;

    case ETacticalUAVTask::Recon:
    case ETacticalUAVTask::Surveillance:
    {
        OrbitAngleRadians += DeltaSeconds * 0.45f;
        const FVector OrbitPoint = Target + FVector(
            FMath::Cos(OrbitAngleRadians) * CurrentCommand.OrbitRadius,
            FMath::Sin(OrbitAngleRadians) * CurrentCommand.OrbitRadius,
            0.0f);
        FlyTowards(OrbitPoint, 100.0f);
        if (SensorAccumulator >= SensorScanInterval)
        {
            PerformSensorScan();
        }
        if (CurrentCommand.Duration <= 0.0f || ExecutionElapsedSeconds >= CurrentCommand.Duration)
        {
            CompleteCurrentTask();
        }
        break;
    }

    case ETacticalUAVTask::Strike:
        ExecuteStrike();
        CompleteCurrentTask();
        break;

    case ETacticalUAVTask::Idle:
    default:
        SetTaskState(ETacticalUAVTaskState::Idle);
        break;
    }
}

void ATacticalUAVPawn::CompleteCurrentTask()
{
    MovementComponent->StopMovementImmediately();
    SetTaskState(ETacticalUAVTaskState::Completed);
    UE_LOG(
        LogTacticalUAV,
        Log,
        TEXT("%s completed seq=%d task=%s at %s detected=%d"),
        *AgentId.ToString(),
        CurrentCommand.SequenceId,
        *TaskToString(CurrentCommand.Task),
        *GetActorLocation().ToCompactString(),
        DetectedActors.Num());
    OnTaskCompleted.Broadcast(CurrentCommand.SequenceId, CurrentCommand.Task);
}

void ATacticalUAVPawn::FailCurrentTask()
{
    MovementComponent->StopMovementImmediately();
    SetTaskState(ETacticalUAVTaskState::Failed);
    OnTaskFailed.Broadcast(CurrentCommand.SequenceId, CurrentCommand.Task);
}

void ATacticalUAVPawn::ExecuteStrike()
{
    const FVector StrikeLocation = IsValid(CurrentCommand.TargetActor)
        ? CurrentCommand.TargetActor->GetActorLocation()
        : CurrentCommand.TargetLocation;

    if (UTacticalMARLMissionSubsystem* Mission = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        Mission->ReportEngagement(AgentId, this, CurrentCommand.TargetActor, StrikeLocation, StrikeDamage, bApplyStrikeDamage);
    }
    else if (bApplyStrikeDamage && IsValid(CurrentCommand.TargetActor))
    {
        UGameplayStatics::ApplyDamage(CurrentCommand.TargetActor, StrikeDamage, GetController(), this, UDamageType::StaticClass());
    }
    UE_LOG(
        LogTacticalUAV,
        Log,
        TEXT("%s executed simulated strike at %s target=%s damage_enabled=%s"),
        *AgentId.ToString(),
        *StrikeLocation.ToCompactString(),
        *GetNameSafe(CurrentCommand.TargetActor),
        bApplyStrikeDamage ? TEXT("true") : TEXT("false"));
    OnStrikeExecuted.Broadcast(StrikeLocation, CurrentCommand.TargetActor);
}

void ATacticalUAVPawn::SetTaskState(const ETacticalUAVTaskState NewState)
{
    TaskState = NewState;
}

bool ATacticalUAVPawn::ParseTaskName(const FString& TaskName, ETacticalUAVTask& OutTask)
{
    const FString Normalized = TaskName.ToLower();
    if (Normalized == TEXT("idle")) OutTask = ETacticalUAVTask::Idle;
    else if (Normalized == TEXT("move") || Normalized == TEXT("move_to")) OutTask = ETacticalUAVTask::MoveTo;
    else if (Normalized == TEXT("recon") || Normalized == TEXT("reconnaissance")) OutTask = ETacticalUAVTask::Recon;
    else if (Normalized == TEXT("surveillance") || Normalized == TEXT("observe")) OutTask = ETacticalUAVTask::Surveillance;
    else if (Normalized == TEXT("strike") || Normalized == TEXT("attack")) OutTask = ETacticalUAVTask::Strike;
    else if (Normalized == TEXT("rtb") || Normalized == TEXT("return_to_base")) OutTask = ETacticalUAVTask::ReturnToBase;
    else return false;
    return true;
}

FString ATacticalUAVPawn::TaskToString(const ETacticalUAVTask Task)
{
    switch (Task)
    {
    case ETacticalUAVTask::MoveTo: return TEXT("move");
    case ETacticalUAVTask::Recon: return TEXT("recon");
    case ETacticalUAVTask::Surveillance: return TEXT("surveillance");
    case ETacticalUAVTask::Strike: return TEXT("strike");
    case ETacticalUAVTask::ReturnToBase: return TEXT("rtb");
    default: return TEXT("idle");
    }
}

FString ATacticalUAVPawn::TaskStateToString(const ETacticalUAVTaskState State)
{
    switch (State)
    {
    case ETacticalUAVTaskState::EnRoute: return TEXT("en_route");
    case ETacticalUAVTaskState::Executing: return TEXT("executing");
    case ETacticalUAVTaskState::Completed: return TEXT("completed");
    case ETacticalUAVTaskState::Failed: return TEXT("failed");
    default: return TEXT("idle");
    }
}
