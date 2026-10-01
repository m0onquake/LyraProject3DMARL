#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalMARLVisualizationSubsystem.generated.h"

class AActor;
class ACameraActor;
class SWidget;
class UTextRenderComponent;

/**
 * Runtime-only S0 visualization for the UrbanDepot baseline.
 *
 * The overlay deliberately reads the same public telemetry and combatant
 * provider state used by the JSON bridge. It never changes observations,
 * rewards, or tactical state. Console variables tacticalmarl.S0HUD,
 * tacticalmarl.S0Labels, tacticalmarl.S0Zones and tacticalmarl.S0OverviewCamera
 * independently control the visible layers.
 */
UCLASS()
class TACTICALMARL_API UTacticalMARLVisualizationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    struct FSceneCounts
    {
        int32 BlueTotal = 0;
        int32 BlueAlive = 0;
        int32 RedTotal = 0;
        int32 RedAlive = 0;
        bool bRosterValid = false;
    };

    void RefreshSceneState();
    void UpdateBlueLabels();
    void UpdateRedLabels();
    void UpdateLabelFacing();
    void UpdateHUD() const;
    void EnsureViewportOverlay();
    void RemoveViewportOverlay();
    FString BuildHUDPanelText() const;
    FString BuildRosterPanelText() const;
    void DrawSceneMarkers() const;
    void UpdateOverviewCamera();
    void RestorePreviousViewTarget();
    void TryStartAcceptanceDemo();
    void UpdateS1AcceptanceDemo();
    void UpdateS2AcceptanceDemo();
    void TryCaptureAcceptanceScreenshot();
    UTextRenderComponent* FindOrCreateLabel(AActor* Actor, const FColor& Color, float Height);
    void RemoveStaleLabels(const TSet<TWeakObjectPtr<AActor>>& ActiveActors);
    static FString MissionPhaseToString(uint8 PhaseValue);

    TMap<TWeakObjectPtr<AActor>, TWeakObjectPtr<UTextRenderComponent>> Labels;
    TSharedPtr<SWidget> ViewportOverlay;
    TArray<FString> RosterRows;
    TWeakObjectPtr<ACameraActor> OverviewCamera;
    TWeakObjectPtr<AActor> PreviousViewTarget;
    TOptional<FVector> BlueBaseLocation;
    TOptional<FVector> RedDefenseLocation;
    TOptional<FVector> PrimaryObjectiveLocation;
    FSceneCounts Counts;
    float RefreshAccumulator = 0.0f;
    double AutoDemoStartedAt = 0.0;
    bool bLastRosterValid = false;
    bool bAutoDemoStarted = false;
    bool bS1PartialDamageApplied = false;
    bool bS1DisableDamageApplied = false;
    bool bS2OccludedStageApplied = false;
    bool bS2VisibleStageApplied = false;
    bool bS2LostStageApplied = false;
    bool bS2OccludedScreenshotRequested = false;
    bool bS2TrackingScreenshotRequested = false;
    bool bS2LostScreenshotRequested = false;
    bool bScreenshotRequested = false;
    TWeakObjectPtr<AActor> S1UAVTarget;
    TWeakObjectPtr<AActor> S1UGVTarget;
};
