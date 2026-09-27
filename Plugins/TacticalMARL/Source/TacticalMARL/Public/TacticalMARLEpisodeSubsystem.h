#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalMARLEpisodeSubsystem.generated.h"

class AActor;
class FJsonObject;

UENUM(BlueprintType)
enum class ETacticalMARLEpisodePhase : uint8
{
    NotStarted,
    Running,
    Terminated,
    Truncated
};

/** Configuration for one real-time multi-agent training episode. */
USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalMARLEpisodeConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Episode", meta=(ClampMin="1"))
    int32 MaxSteps = 1000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Episode", meta=(ClampMin="1.0"))
    float MaxEpisodeSeconds = 300.0f;

    // Bounds are relative to WorldCenter. The default matches the compact 200 m map.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Episode")
    FVector WorldCenter = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Episode")
    FVector WorldHalfExtent = FVector(10000.0f, 10000.0f, 5000.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reward")
    float StepPenalty = -0.001f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reward")
    float NewDetectionReward = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reward")
    float TaskCompletedReward = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reward")
    float TaskFailedPenalty = -0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reward")
    float OutOfBoundsPenalty = -1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Episode")
    bool bTerminateOnAnyAgentFailure = true;
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalMARLEpisodeState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Episode") int32 EpisodeId = 0;
    UPROPERTY(BlueprintReadOnly, Category="Episode") int32 Seed = 0;
    UPROPERTY(BlueprintReadOnly, Category="Episode") int32 Step = 0;
    UPROPERTY(BlueprintReadOnly, Category="Episode") float ElapsedSeconds = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Episode") ETacticalMARLEpisodePhase Phase = ETacticalMARLEpisodePhase::NotStarted;
    UPROPERTY(BlueprintReadOnly, Category="Episode") FString EndReason;
};

/**
 * Owns episode reset, step accounting, rewards and done flags.
 * The subsystem is independent of a particular Python MARL library; JSON output
 * follows the observations/rewards/terminations/truncations/infos convention.
 */
UCLASS()
class TACTICALMARL_API UTacticalMARLEpisodeSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Episode")
    bool ResetEpisode(int32 Seed = 0);

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Episode")
    bool AdvanceEpisode();

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Episode")
    void EndEpisode(const FString& Reason = TEXT("external_end"), bool bTruncated = false);

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Episode")
    FTacticalMARLEpisodeState GetEpisodeState() const;

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Episode")
    bool IsEpisodeRunning() const { return State.Phase == ETacticalMARLEpisodePhase::Running; }

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Episode")
    FString GetEpisodeJson() const;

    void AppendStepFields(const TSharedRef<FJsonObject>& Root) const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Episode")
    FTacticalMARLEpisodeConfig Config;

private:
    struct FAgentStartState
    {
        TWeakObjectPtr<AActor> Actor;
        FTransform Transform;
    };

    void CaptureAgentStartStates();
    void ResetActors();
    void CaptureBaselineMetrics();
    void ComputeRewardsAndDoneFlags();
    float GetElapsedSeconds() const;
    static FString PhaseToString(ETacticalMARLEpisodePhase Phase);

    FTacticalMARLEpisodeState State;
    double EpisodeStartWorldSeconds = 0.0;
    double LastAdvanceWorldSeconds = 0.0;
    int32 NextEpisodeId = 1;
    TArray<FAgentStartState> AgentStartStates;
    TMap<FString, int32> PreviousDetectionCounts;
    TMap<FString, FString> PreviousTaskStates;
    TMap<FString, float> Rewards;
    TMap<FString, bool> Terminations;
    TMap<FString, bool> Truncations;
};
