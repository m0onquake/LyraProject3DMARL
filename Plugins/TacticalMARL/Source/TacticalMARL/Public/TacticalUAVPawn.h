#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "TacticalUAVTypes.h"
#include "TacticalUAVPawn.generated.h"

class UCameraComponent;
class UFloatingPawnMovement;
class USphereComponent;
class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUAVTaskEvent, int32, SequenceId, ETacticalUAVTask, Task);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUAVDetectionEvent, AActor*, DetectedActor, FVector, LastKnownLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTacticalUAVStrikeEvent, FVector, StrikeLocation, AActor*, TargetActor);

UCLASS(Blueprintable)
class TACTICALMARL_API ATacticalUAVPawn : public APawn
{
    GENERATED_BODY()

public:
    ATacticalUAVPawn();

    virtual void Tick(float DeltaSeconds) override;
    virtual void BeginPlay() override;
    virtual UPawnMovementComponent* GetMovementComponent() const override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Policy")
    bool ReceivePolicyCommand(const FTacticalUAVPolicyCommand& Command);

    // Compact action bridge for RL: 0 idle, 1 move, 2 recon, 3 surveillance,
    // 4 strike, 5 return-to-base.
    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Policy")
    bool SubmitDiscreteAction(int32 Action, const FVector& TargetLocation, int32 SequenceId = 0);

    // Low-level RL action. MoveInput is normalized world-space XYZ and is held
    // until another high/low-level command arrives. YawRate is degrees/second.
    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Policy")
    bool SubmitContinuousAction(const FVector& MoveInput, float YawRate, int32 SequenceId = 0);

    // JSON schema example:
    // {"sequence_id":1,"task":"recon","target":[100,200,800],"duration":10}
    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Policy")
    bool ReceivePolicyJson(const FString& JsonCommand, FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Policy")
    void CancelCurrentTask();

    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Episode")
    void ResetForEpisode(const FTransform& SpawnTransform);

    UFUNCTION(BlueprintPure, Category = "Tactical UAV|Telemetry")
    FTacticalUAVTelemetry GetTelemetry() const;

    UFUNCTION(BlueprintPure, Category = "Tactical UAV|Telemetry")
    FString GetTelemetryJson() const;

    UFUNCTION(BlueprintCallable, Category = "Tactical UAV|Sensors")
    TArray<AActor*> PerformSensorScan();

    UFUNCTION(BlueprintPure, Category = "Tactical UAV|Sensors")
    TArray<AActor*> GetDetectedActors() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Tactical UAV")
    FName AgentId = TEXT("UAV_01");

    // TacML high-level role assigned by the external policy. The role is
    // observable state; low-level flight remains owned by UE.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Tactical UAV|Policy")
    FName AssignedRole = TEXT("UnassignedRole");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Policy")
    bool bAutoStartInitialCommand = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Policy", meta = (EditCondition = "bAutoStartInitialCommand"))
    FTacticalUAVPolicyCommand InitialCommand;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Sensors", meta = (ClampMin = "100.0"))
    float SensorRange = 3000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Sensors", meta = (ClampMin = "0.05"))
    float SensorScanInterval = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Strike", meta = (ClampMin = "100.0"))
    float StrikeRange = 800.0f;

    // Enabled by default for the recon-strike mission; disable for sensor-only curricula.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Strike")
    bool bApplyStrikeDamage = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tactical UAV|Strike", meta = (EditCondition = "bApplyStrikeDamage"))
    float StrikeDamage = 50.0f;

    UPROPERTY(BlueprintAssignable, Category = "Tactical UAV|Events")
    FTacticalUAVTaskEvent OnTaskCompleted;

    UPROPERTY(BlueprintAssignable, Category = "Tactical UAV|Events")
    FTacticalUAVTaskEvent OnTaskFailed;

    UPROPERTY(BlueprintAssignable, Category = "Tactical UAV|Events")
    FTacticalUAVDetectionEvent OnTargetDetected;

    UPROPERTY(BlueprintAssignable, Category = "Tactical UAV|Events")
    FTacticalUAVStrikeEvent OnStrikeExecuted;

protected:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<USphereComponent> CollisionComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> BodyMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UCameraComponent> ReconCamera;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UFloatingPawnMovement> MovementComponent;

    UPROPERTY(ReplicatedUsing = OnRep_CurrentCommand, BlueprintReadOnly, Category = "Tactical UAV|Policy")
    FTacticalUAVPolicyCommand CurrentCommand;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Tactical UAV|Policy")
    ETacticalUAVTaskState TaskState = ETacticalUAVTaskState::Idle;

    UFUNCTION()
    void OnRep_CurrentCommand();

private:
    UFUNCTION(Server, Reliable)
    void ServerReceivePolicyCommand(const FTacticalUAVPolicyCommand& Command);

    FVector ResolveTargetLocation() const;
    bool FlyTowards(const FVector& Destination, float AcceptanceRadius);
    void ExecuteCurrentTask(float DeltaSeconds);
    void CompleteCurrentTask();
    void FailCurrentTask();
    void ExecuteStrike();
    void SetTaskState(ETacticalUAVTaskState NewState);
    static bool ParseTaskName(const FString& TaskName, ETacticalUAVTask& OutTask);
    static FString TaskToString(ETacticalUAVTask Task);
    static FString TaskStateToString(ETacticalUAVTaskState State);

    FVector HomeLocation = FVector::ZeroVector;
    float TaskElapsedSeconds = 0.0f;
    float ExecutionElapsedSeconds = 0.0f;
    float SensorAccumulator = 0.0f;
    float OrbitAngleRadians = 0.0f;
    TSet<TWeakObjectPtr<AActor>> DetectedActors;
    bool bContinuousControl = false;
    FVector ContinuousMoveInput = FVector::ZeroVector;
    float ContinuousYawRate = 0.0f;
};
