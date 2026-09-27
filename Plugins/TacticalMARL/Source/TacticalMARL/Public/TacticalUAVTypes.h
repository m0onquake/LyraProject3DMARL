#pragma once

#include "CoreMinimal.h"
#include "TacticalUAVTypes.generated.h"

class AActor;

UENUM(BlueprintType)
enum class ETacticalUAVTask : uint8
{
    Idle UMETA(DisplayName = "Idle"),
    MoveTo UMETA(DisplayName = "Move To"),
    Recon UMETA(DisplayName = "Reconnaissance"),
    Surveillance UMETA(DisplayName = "Surveillance"),
    Strike UMETA(DisplayName = "Strike"),
    ReturnToBase UMETA(DisplayName = "Return To Base")
};

UENUM(BlueprintType)
enum class ETacticalUAVTaskState : uint8
{
    Idle,
    EnRoute,
    Executing,
    Completed,
    Failed
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalUAVPolicyCommand
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy")
    int32 SequenceId = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy")
    ETacticalUAVTask Task = ETacticalUAVTask::Idle;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy")
    FVector TargetLocation = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy")
    TObjectPtr<AActor> TargetActor = nullptr;

    // Absolute world altitude in centimetres. Values <= 0 keep TargetLocation.Z.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy", meta = (ClampMin = "0.0"))
    float DesiredAltitude = 800.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy", meta = (ClampMin = "100.0"))
    float MaxSpeed = 1200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy", meta = (ClampMin = "25.0"))
    float AcceptanceRadius = 150.0f;

    // Recon/surveillance dwell time. Zero means one sensor scan.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy", meta = (ClampMin = "0.0"))
    float Duration = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy", meta = (ClampMin = "100.0"))
    float OrbitRadius = 600.0f;
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalUAVTelemetry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    FName AgentId = NAME_None;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    FName AssignedRole = TEXT("UnassignedRole");

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    FVector Location = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    FVector Velocity = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    ETacticalUAVTask Task = ETacticalUAVTask::Idle;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    ETacticalUAVTaskState TaskState = ETacticalUAVTaskState::Idle;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    int32 SequenceId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    int32 DetectedTargetCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Telemetry")
    float TaskElapsedSeconds = 0.0f;
};
