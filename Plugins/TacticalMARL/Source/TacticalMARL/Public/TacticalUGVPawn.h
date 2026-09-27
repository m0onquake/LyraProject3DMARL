#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "TacticalUGVTypes.h"
#include "TacticalUGVPawn.generated.h"

class UBoxComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUGVTaskEvent, int32, SequenceId, ETacticalUGVTask, Task);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUGVDetectionEvent, AActor*, DetectedActor, FVector, LastKnownLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUGVEngageEvent, FVector, EngageLocation, AActor*, TargetActor);

/** Lightweight wheeled blue-force agent with high-level and continuous RL control. */
UCLASS(Blueprintable)
class TACTICALMARL_API ATacticalUGVPawn : public APawn
{
    GENERATED_BODY()

public:
    ATacticalUGVPawn();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Policy") bool ReceivePolicyCommand(const FTacticalUGVPolicyCommand& Command);
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Policy") bool ReceivePolicyJson(const FString& JsonCommand, FString& OutError);
    // Actions: 0 idle, 1 move, 2 patrol, 3 recon, 4 surveillance, 5 engage, 6 RTB.
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Policy") bool SubmitDiscreteAction(int32 Action, const FVector& TargetLocation, int32 SequenceId = 0);
    // Normalized throttle/steering in [-1,1].
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Policy") bool SubmitContinuousAction(float Throttle, float Steering, bool bBrake, int32 SequenceId = 0);
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Policy") void CancelCurrentTask();
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Episode") void ResetForEpisode(const FTransform& SpawnTransform);

    UFUNCTION(BlueprintPure, Category="Tactical UGV|Telemetry") FTacticalUGVTelemetry GetTelemetry() const;
    UFUNCTION(BlueprintPure, Category="Tactical UGV|Telemetry") FString GetTelemetryJson() const;
    UFUNCTION(BlueprintCallable, Category="Tactical UGV|Sensors") TArray<AActor*> PerformSensorScan();
    UFUNCTION(BlueprintPure, Category="Tactical UGV|Sensors") TArray<AActor*> GetDetectedActors() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Tactical UGV") FName AgentId = TEXT("UGV_01");
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category="Tactical UGV|Policy") FName AssignedRole = TEXT("UnassignedRole");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Policy") bool bAutoStartInitialCommand = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Policy", meta=(EditCondition="bAutoStartInitialCommand")) FTacticalUGVPolicyCommand InitialCommand;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Movement") float MaxAcceleration = 500.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Movement") float MaxTurnRate = 90.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation") bool bUseNavMeshPathfinding = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation", meta=(ClampMin="0.1")) float NavigationRepathInterval = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation", meta=(ClampMin="100.0")) float ObstacleProbeDistance = 550.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation", meta=(ClampMin="200.0")) float FormationSpacing = 380.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation", meta=(ClampMin="200.0")) float VehicleAvoidanceRadius = 700.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Navigation", meta=(ClampMin="150.0")) float VehicleYieldDistance = 440.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Sensors") float SensorRange = 2500.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Sensors") float SensorScanInterval = 0.5f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Engage") float EngageRange = 1600.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Engage") bool bApplyEngageDamage = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical UGV|Engage", meta=(EditCondition="bApplyEngageDamage")) float EngageDamage = 35.0f;

    UPROPERTY(BlueprintAssignable, Category="Tactical UGV|Events") FTacticalUGVTaskEvent OnTaskCompleted;
    UPROPERTY(BlueprintAssignable, Category="Tactical UGV|Events") FTacticalUGVTaskEvent OnTaskFailed;
    UPROPERTY(BlueprintAssignable, Category="Tactical UGV|Events") FTacticalUGVDetectionEvent OnTargetDetected;
    UPROPERTY(BlueprintAssignable, Category="Tactical UGV|Events") FTacticalUGVEngageEvent OnEngageExecuted;

protected:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components") TObjectPtr<UBoxComponent> CollisionComponent;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components") TObjectPtr<UStaticMeshComponent> BodyMesh;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components") TObjectPtr<UStaticMeshComponent> TurretMesh;
    UPROPERTY(Replicated, BlueprintReadOnly, Category="Tactical UGV|Policy") FTacticalUGVPolicyCommand CurrentCommand;
    UPROPERTY(Replicated, BlueprintReadOnly, Category="Tactical UGV|Policy") ETacticalUGVTaskState TaskState = ETacticalUGVTaskState::Idle;

private:
    FVector ResolveTargetLocation() const;
    void ExecuteHighLevelTask(float DeltaSeconds);
    bool DriveTowards(const FVector& Destination, float AcceptanceRadius, float DeltaSeconds);
    FVector ResolveFormationDestination(const FVector& Destination) const;
    void ApplyVehicleAvoidance(FVector& SteeringTarget, float& ThrottleScale, float DeltaSeconds);
    void UpdateNavigationPath(const FVector& Destination, bool bForce = false);
    FVector GetNavigationSteeringTarget(const FVector& Destination, float AcceptanceRadius);
    void ResetNavigationState();
    void ApplyDrive(float Throttle, float Steering, bool bBrake, float DeltaSeconds);
    void CompleteTask();
    void ExecuteEngage();
    static bool ParseTaskName(const FString& Name, ETacticalUGVTask& OutTask);
    static FString TaskToString(ETacticalUGVTask Task);
    static FString StateToString(ETacticalUGVTaskState State);

    FVector HomeLocation = FVector::ZeroVector;
    FVector LastLocation = FVector::ZeroVector;
    FVector CurrentVelocity = FVector::ZeroVector;
    float CurrentSpeed = 0.0f;
    float TaskElapsedSeconds = 0.0f;
    float ExecutionElapsedSeconds = 0.0f;
    float SensorAccumulator = 0.0f;
    float PatrolAngle = 0.0f;
    bool bContinuousControl = false;
    float ContinuousThrottle = 0.0f;
    float ContinuousSteering = 0.0f;
    bool bContinuousBrake = false;
    TSet<TWeakObjectPtr<AActor>> DetectedActors;
    TArray<FVector> NavigationPathPoints;
    int32 NavigationPathIndex = 0;
    FVector NavigationGoal = FVector::ZeroVector;
    float NavigationRepathElapsed = 0.0f;
    float StuckElapsed = 0.0f;
    FVector ProgressSampleLocation = FVector::ZeroVector;
    bool bNavigationPathPartial = false;
    bool bNavigationStartProjected = false;
    bool bNavigationGoalProjected = false;
    int32 AvoidanceNeighborCount = 0;
    bool bYieldingToVehicle = false;
    float YieldHoldRemaining = 0.0f;
    float StaticAvoidanceHoldRemaining = 0.0f;
    float StaticAvoidanceSteering = 0.0f;
    float StaticAvoidanceClearElapsed = 0.0f;
    float GoalProgressElapsed = 0.0f;
    float LastGoalDistance = -1.0f;
    int32 RecoveryTurnCount = 0;
};
