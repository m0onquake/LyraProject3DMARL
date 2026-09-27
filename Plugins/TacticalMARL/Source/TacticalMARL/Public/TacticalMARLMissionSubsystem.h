#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalMARLCombatantProvider.h"
#include "TacticalMARLMissionSubsystem.generated.h"

class FJsonObject;

UENUM(BlueprintType)
enum class ETacticalMARLMissionPhase : uint8
{
    Search,
    CandidateFound,
    TargetConfirmed,
    TrackEstablished,
    StrikeReady,
    Engagement,
    EffectAssessment,
    Success,
    Failed
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalMARLTeamRewardConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Search") float StepCost = -0.01f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Search") float NewCoverageCell = 0.02f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Search") float RepeatedSearchCost = -0.005f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float FirstDetection = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float TargetConfirmed = 2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float TrackEstablished = 2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float TrackMaintained = 0.02f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float TrackLost = -0.5f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recon") float TrackReacquired = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Strike") float ValidEngagement = 0.5f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Strike") float InvalidEngagement = -2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Strike") float FullHealthDamage = 2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Strike") float ObjectiveKill = 5.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mission") float MissionSuccess = 10.0f;
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalMARLMissionState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Mission") ETacticalMARLMissionPhase Phase = ETacticalMARLMissionPhase::Search;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bCandidateFound = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bTargetConfirmed = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bTrackValid = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bStrikeReady = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bEngagementStarted = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bEffectAchieved = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bMissionSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") bool bMissionFailure = false;
    UPROPERTY(BlueprintReadOnly, Category="Mission") int32 AliveHostiles = 0;
    UPROPERTY(BlueprintReadOnly, Category="Mission") int32 ObjectiveKills = 0;
    UPROPERTY(BlueprintReadOnly, Category="Mission") int32 InvalidEngagements = 0;
    UPROPERTY(BlueprintReadOnly, Category="Mission") float TrackQuality = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Mission") FName TargetZone = TEXT("CandidateArea");
};

/** TacML-inspired Search -> Confirm -> Track -> Strike -> Assess mission runtime. */
UCLASS()
class TACTICALMARL_API UTacticalMARLMissionSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Mission") bool ResetMission();
    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Mission") void AdvanceMissionStep(float DeltaSeconds);
    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Mission")
    bool ReportEngagement(FName AgentId, AActor* InstigatorActor, AActor* RequestedTarget, FVector EngagementLocation, float Damage, bool bApplyDamage);

    UFUNCTION(BlueprintPure, Category="Tactical MARL|Mission") FTacticalMARLMissionState GetMissionState() const { return State; }
    UFUNCTION(BlueprintPure, Category="Tactical MARL|Mission") FString GetMissionJson() const;

    float ConsumeTeamReward();
    void ConsumeAgentRewards(TMap<FString, float>& OutRewards);
    void AppendMissionFields(const TSharedRef<FJsonObject>& Root) const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission") FTacticalMARLTeamRewardConfig RewardConfig;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="0.0")) float ConfirmationSeconds = 2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="0.0")) float TrackLossGraceSeconds = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="100.0")) float CoverageCellSize = 1000.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="0.0")) float EngagementTargetTolerance = 500.0f;
    // MARL neutralization deliberately leaves a Lyra combatant alive so the
    // stock Elimination experience does not tear down the entire red team.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="0.0", ClampMax="1.0"))
    float ObjectiveNeutralizedHealthFraction = 0.01f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Mission", meta=(ClampMin="1")) int32 MaxInvalidEngagements = 3;

private:
    void CollectCombatants(TArray<UObject*>& OutProviders, TArray<FTacticalMARLCombatantState>& OutStates) const;
    void CollectDetectedTargets(TSet<TWeakObjectPtr<AActor>>& OutTargets, int32& OutObservers) const;
    void UpdateCoverage();
    void UpdateCombatantRewards(const TArray<FTacticalMARLCombatantState>& Combatants);
    void AccumulateTeamReward(const FString& Component, float Value);
    bool IsObjective(const FTacticalMARLCombatantState& Combatant, bool bHasExplicitObjectives) const;
    static FString PhaseToString(ETacticalMARLMissionPhase Phase);

    FTacticalMARLMissionState State;
    TMap<FString, float> PreviousHealth;
    TMap<FString, bool> PreviousAlive;
    TMap<FString, int32> KillsByAgent;
    TMap<FString, float> PendingAgentRewards;
    TSet<FIntPoint> VisitedCoverageCells;
    float PendingTeamReward = 0.0f;
    float LastTeamReward = 0.0f;
    TMap<FString, float> PendingRewardComponents;
    TMap<FString, float> LastRewardComponents;
    float CandidateObservedSeconds = 0.0f;
    float TimeSinceTargetObserved = 0.0f;
    bool bHadValidTrack = false;
};
