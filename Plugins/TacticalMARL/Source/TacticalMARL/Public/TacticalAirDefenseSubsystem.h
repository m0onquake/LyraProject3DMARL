#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalAirDefenseSubsystem.generated.h"

class AActor;
class AStaticMeshActor;
class FJsonObject;
struct FTacticalPerceivedContact;

UENUM(BlueprintType)
enum class ETacticalAirDefenseState : uint8
{
    Disabled,
    Scanning,
    Confirming,
    Locking,
    Warning,
    Launching,
    Cooldown,
    LostLock,
    OutOfAmmo
};

USTRUCT(BlueprintType)
struct TACTICALMARL_API FTacticalAirDefenseState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") ETacticalAirDefenseState State = ETacticalAirDefenseState::Disabled;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FName LauncherAgentId = TEXT("RED_ANTIUAV_01");
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FName TargetAgentId = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FName TargetRole = NAME_None;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FVector LastPerceivedLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FVector IncomingDirection = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") bool bDirectLineOfSight = false;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") bool bIncoming = false;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float StateElapsedSeconds = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float ConfirmationProgress = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float LockProgress = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float WarningRemainingSeconds = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float EstimatedTimeToImpact = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float CooldownRemainingSeconds = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") int32 AmmoRemaining = 0;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float Heat = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") int32 ShotCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") int32 HitCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") int32 MissCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float LastHitProbability = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") float LastRandomRoll = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FString LastShotResult = TEXT("none");
    UPROPERTY(BlueprintReadOnly, Category="Tactical MARL|Air Defense") FString LastTransitionReason = TEXT("not_started");
};

/** D2 rule controller for the red Anti-UAV launcher. It consumes only S2 direct contacts. */
UCLASS()
class TACTICALMARL_API UTacticalAirDefenseSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Air Defense") void ResetForEpisode(int32 Seed);
    UFUNCTION(BlueprintPure, Category="Tactical MARL|Air Defense") bool IsAirDefenseEnabled() const;
    UFUNCTION(BlueprintPure, Category="Tactical MARL|Air Defense") FTacticalAirDefenseState GetAirDefenseState() const { return Runtime; }

    void AppendAirDefenseFields(const TSharedRef<FJsonObject>& Root) const;
    void AppendAgentThreatFields(const TSharedRef<FJsonObject>& Agent, FName AgentId) const;
    bool SetAcceptanceStage(const FString& Stage, FString& OutError);
    const TArray<FString>& GetEventSequence() const { return EventSequence; }
    const FString& GetAcceptanceStage() const { return AcceptanceStage; }
    AActor* GetLauncherActor() const { return LauncherActor.Get(); }
    AActor* GetCurrentTargetActor() const { return CurrentTargetActor.Get(); }
    FVector GetProjectileLocation() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float ConfirmationDuration = 0.65f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float LockDuration = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float WarningDuration = 1.25f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float ProjectileTravelDuration = 0.70f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float CooldownDuration = 2.25f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float LostLockHoldDuration = 0.75f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float DamagePerHit = 55.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float MaximumEngagementRange = 6000.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float BaseHitProbability = 0.98f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") int32 AmmoCapacity = 4;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float HeatPerShot = 38.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float MaximumHeat = 100.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tactical MARL|Air Defense") float HeatCoolPerSecond = 5.0f;

    static FString StateToString(ETacticalAirDefenseState State);

private:
    const FTacticalPerceivedContact* SelectPriorityContact() const;
    const FTacticalPerceivedContact* FindCurrentContact() const;
    void RefreshLauncherActor();
    void SetState(ETacticalAirDefenseState NewState, const FString& Reason);
    void ClearTarget();
    void BeginLaunch(const FTacticalPerceivedContact& Contact);
    void UpdateProjectile(float DeltaTime);
    void ResolveImpact();
    void SpawnProjectileVisual(const FVector& Start);
    void SpawnExplosionVisual(const FVector& Location, bool bHit);
    void UpdateExplosionVisual(float DeltaTime);
    void DestroyTransientVisuals();
    bool HasPhysicalLineOfSight(AActor* Target, FVector& OutTargetLocation) const;
    float ComputeHitProbability(const FTacticalPerceivedContact& Contact) const;
    static int32 GetRolePriority(FName Role);

    FTacticalAirDefenseState Runtime;
    TArray<FString> EventSequence;
    FRandomStream RandomStream;
    int32 EpisodeSeed = 0;
    FString AcceptanceStage = TEXT("none");
    TWeakObjectPtr<AActor> LauncherActor;
    TWeakObjectPtr<AActor> DamageSourceActor;
    TWeakObjectPtr<AActor> CurrentTargetActor;
    TWeakObjectPtr<AStaticMeshActor> ProjectileVisual;
    TWeakObjectPtr<AStaticMeshActor> ExplosionVisual;
    FVector ProjectileStart = FVector::ZeroVector;
    FVector ProjectileEnd = FVector::ZeroVector;
    float ProjectileElapsed = 0.0f;
    float ExplosionElapsed = 0.0f;
    bool bPendingRandomHit = false;
};
