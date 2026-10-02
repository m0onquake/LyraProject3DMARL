#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalThreatSubsystem.generated.h"

class AActor;
class FJsonObject;

/** A contact produced by one red sensor after range, FOV and LOS all pass. */
USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalPerceivedContact
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetType = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName ObservedRole = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FVector ObservedLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FVector ObservedVelocity = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float Distance = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float MeasurementQuality = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float ObservedWorldSeconds = -1.0f;

    // Runtime-only handle. It is populated only for a direct visible contact
    // and is never serialized into decentralized observations.
    TWeakObjectPtr<AActor> TargetActor;
};

UENUM(BlueprintType)
enum class ETacticalRedAlertState : uint8
{
    Unaware,
    Suspicious,
    Alerted,
    Tracking,
    Engaging,
    LostContact
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalThreatSensorState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName SensorAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName Role = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") ETacticalRedAlertState AlertState = ETacticalRedAlertState::Unaware;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float Confidence = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") bool bDirectLineOfSight = false;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") bool bLastTraceBlocked = false;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetType = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FVector LastKnownLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float LastSeenWorldSeconds = -1.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float SensorRange = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float FieldOfViewDegrees = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") int32 BlockedTraceCount = 0;

    TArray<FTacticalPerceivedContact> DirectContacts;

    TWeakObjectPtr<AActor> SensorActor;
    FVector LastTraceEnd = FVector::ZeroVector;
    float LastPublishWorldSeconds = -1.0f;
    float LostContactSeconds = 0.0f;
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalSharedAlertState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") ETacticalRedAlertState AlertState = ETacticalRedAlertState::Unaware;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float Confidence = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName SourceAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FName TargetType = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") FVector LastKnownLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float LastSeenWorldSeconds = -1.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float LastDeliveryWorldSeconds = -1.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") float LastPropagationDelay = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Threat") int32 DeliveredMessageCount = 0;
};

/** Local red perception and delayed shared alert state. It never performs attacks. */
UCLASS()
class TACTICALMARL_API UTacticalThreatSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Threat")
    void ResetForEpisode(int32 Seed);

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Threat")
    bool IsThreatSensingEnabled() const;

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Threat")
    FTacticalSharedAlertState GetSharedAlertState() const { return SharedAlert; }

    const TArray<FTacticalThreatSensorState>& GetSensorStates() const { return Sensors; }
    const FTacticalThreatSensorState* FindSensorState(FName SensorAgentId) const;
    const TArray<FString>& GetEventSequence() const { return EventSequence; }
    const FString& GetAcceptanceStage() const { return AcceptanceStage; }

    void AppendThreatFields(const TSharedRef<FJsonObject>& Root) const;
    bool SetAcceptanceStage(const FString& Stage, FString& OutError);

    static FString AlertStateToString(ETacticalRedAlertState State);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float DetectionGainPerSecond = 0.85f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float DirectConfidenceDecayPerSecond = 0.30f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float SharedConfidenceDecayPerSecond = 0.10f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float LostContactDelaySeconds = 0.75f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float ForgetAfterSeconds = 7.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Threat") float SharedPublishInterval = 0.40f;

private:
    struct FPendingAlertMessage
    {
        double DeliverAt = 0.0;
        double SeenAt = 0.0;
        float PropagationDelay = 0.0f;
        float Confidence = 0.0f;
        FName SourceAgentId = NAME_None;
        FName TargetAgentId = NAME_None;
        FName TargetType = NAME_None;
        FVector LastKnownLocation = FVector::ZeroVector;
        ETacticalRedAlertState AlertState = ETacticalRedAlertState::Unaware;
    };

    void RefreshSensorRoster();
    void UpdateSensor(FTacticalThreatSensorState& Sensor, float DeltaTime);
    void ProcessPendingMessages(float DeltaTime);
    void QueueSharedAlert(FTacticalThreatSensorState& Sensor);
    void SetSensorAlertState(FTacticalThreatSensorState& Sensor, ETacticalRedAlertState NewState);
    void SetSharedAlertState(ETacticalRedAlertState NewState);
    float GetPropagationDelay(FName SourceAgentId, FName TargetAgentId) const;
    void DestroyAcceptanceObstacle();
    bool PositionAcceptanceTarget(bool bBlocked, FString& OutError);
    static FName ResolveBlueAgentId(const AActor* Actor);
    static FName ResolveBlueTargetType(const AActor* Actor);
    static void GetSensorConfiguration(FName Role, float& OutRange, float& OutFovDegrees);

    TArray<FTacticalThreatSensorState> Sensors;
    TArray<FPendingAlertMessage> PendingMessages;
    FTacticalSharedAlertState SharedAlert;
    TArray<FString> EventSequence;
    int32 EpisodeSeed = 0;
    float RosterRefreshAccumulator = 0.0f;
    FString AcceptanceStage = TEXT("none");
    TWeakObjectPtr<AActor> AcceptanceTarget;
    TWeakObjectPtr<AActor> AcceptanceSource;
    TWeakObjectPtr<AActor> AcceptanceObstacle;
};
