#pragma once

#include "CoreMinimal.h"
#include "TacticalUGVTypes.generated.h"

class AActor;

UENUM(BlueprintType)
enum class ETacticalUGVTask : uint8
{
    Idle,
    MoveTo,
    Patrol,
    Recon,
    Surveillance,
    Engage,
    ReturnToBase
};

UENUM(BlueprintType)
enum class ETacticalUGVTaskState : uint8
{
    Idle,
    EnRoute,
    Executing,
    Completed,
    Failed
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalUGVPolicyCommand
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy") int32 SequenceId = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy") ETacticalUGVTask Task = ETacticalUGVTask::Idle;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy") FVector TargetLocation = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy") TObjectPtr<AActor> TargetActor = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy", meta=(ClampMin="50.0")) float MaxSpeed = 600.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy", meta=(ClampMin="25.0")) float AcceptanceRadius = 150.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy", meta=(ClampMin="0.0")) float Duration = 5.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Policy", meta=(ClampMin="100.0")) float PatrolRadius = 800.0f;
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalUGVTelemetry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Telemetry") FName AgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") FName AssignedRole = TEXT("UnassignedRole");
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") FVector Location = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") FVector Velocity = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") ETacticalUGVTask Task = ETacticalUGVTask::Idle;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") ETacticalUGVTaskState TaskState = ETacticalUGVTaskState::Idle;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") int32 SequenceId = 0;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") int32 DetectedTargetCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Telemetry") float TaskElapsedSeconds = 0.0f;
};
