#include "TacticalMARLVisualizationSubsystem.h"

#include "TacticalAgentHealthComponent.h"
#include "TacticalAirDefenseSubsystem.h"
#include "TacticalMARLCombatantProvider.h"
#include "TacticalMARLEpisodeSubsystem.h"
#include "TacticalMARLMissionSubsystem.h"
#include "TacticalThreatSubsystem.h"
#include "TacticalUAVPawn.h"
#include "TacticalUGVPawn.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/ActorComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Styling/CoreStyle.h"
#include "UnrealClient.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogTacticalS0Visuals, Log, All);

namespace
{
TAutoConsoleVariable<int32> CVarTacticalS0HUD(
    TEXT("tacticalmarl.S0HUD"), 1,
    TEXT("Show the TacticalMARL S0 status HUD (0/1)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarTacticalS0Labels(
    TEXT("tacticalmarl.S0Labels"), 1,
    TEXT("Show TacticalMARL agent ID/role/task/health labels (0/1)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarTacticalS0Zones(
    TEXT("tacticalmarl.S0Zones"), 1,
    TEXT("Show the blue base, red defense zone and primary objective marker (0/1)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarTacticalS0OverviewCamera(
    TEXT("tacticalmarl.S0OverviewCamera"), 1,
    TEXT("Use the automatic UrbanDepot S0 overview camera (0/1)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarTacticalS2Debug(
    TEXT("tacticalmarl.S2Debug"), 1,
    TEXT("Show red sensor FOV, LOS and shared-alert markers (0/1)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarTacticalS3Debug(
    TEXT("tacticalmarl.S3Debug"), 1,
    TEXT("Show D2 launcher scan, lock, warning and missile markers (0/1)."), ECVF_Default);

constexpr int32 ExpectedBlueAgents = 6;
constexpr int32 ExpectedRedAgents = 5;
constexpr int32 DefaultS0Seed = 42;
const FColor BlueColor(40, 180, 255);
const FColor RedColor(255, 80, 60);
const FColor ObjectiveColor(255, 215, 40);

FString UAVTaskToString(const ETacticalUAVTask Task)
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

FString UAVStateToString(const ETacticalUAVTaskState State)
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

FString UGVTaskToString(const ETacticalUGVTask Task)
{
    switch (Task)
    {
    case ETacticalUGVTask::MoveTo: return TEXT("move");
    case ETacticalUGVTask::Patrol: return TEXT("patrol");
    case ETacticalUGVTask::Recon: return TEXT("recon");
    case ETacticalUGVTask::Surveillance: return TEXT("surveillance");
    case ETacticalUGVTask::Engage: return TEXT("engage");
    case ETacticalUGVTask::ReturnToBase: return TEXT("rtb");
    default: return TEXT("idle");
    }
}

FString UGVStateToString(const ETacticalUGVTaskState State)
{
    switch (State)
    {
    case ETacticalUGVTaskState::EnRoute: return TEXT("en_route");
    case ETacticalUGVTaskState::Executing: return TEXT("executing");
    case ETacticalUGVTaskState::Completed: return TEXT("completed");
    case ETacticalUGVTaskState::Failed: return TEXT("failed");
    default: return TEXT("idle");
    }
}

FString CompactRole(const FName Role)
{
    FString Value = Role.ToString();
    Value.RemoveFromStart(TEXT("MARL.Role."));
    return Value.IsEmpty() ? TEXT("Unassigned") : Value;
}

FColor AlertColor(const ETacticalRedAlertState State)
{
    switch (State)
    {
    case ETacticalRedAlertState::Suspicious: return FColor::Yellow;
    case ETacticalRedAlertState::Alerted: return FColor(255, 140, 0);
    case ETacticalRedAlertState::Tracking: return FColor::Red;
    case ETacticalRedAlertState::LostContact: return FColor(180, 70, 255);
    case ETacticalRedAlertState::Engaging: return FColor(255, 0, 180);
    default: return FColor(140, 140, 140);
    }
}
}

void UTacticalMARLVisualizationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogTacticalS0Visuals, Log, TEXT("S0 visualization initialized: Difficulty D0, default seed 42"));
}

void UTacticalMARLVisualizationSubsystem::Deinitialize()
{
    RestorePreviousViewTarget();
    RemoveViewportOverlay();
    for (const TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<UTextRenderComponent>>& Pair : Labels)
    {
        if (UTextRenderComponent* Label = Pair.Value.Get()) Label->DestroyComponent();
    }
    Labels.Reset();
    if (ACameraActor* Camera = OverviewCamera.Get()) Camera->Destroy();
    OverviewCamera.Reset();
    Super::Deinitialize();
}

bool UTacticalMARLVisualizationSubsystem::DoesSupportWorldType(const EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

TStatId UTacticalMARLVisualizationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTacticalMARLVisualizationSubsystem, STATGROUP_Tickables);
}

void UTacticalMARLVisualizationSubsystem::Tick(const float DeltaTime)
{
    EnsureViewportOverlay();
    RefreshAccumulator += DeltaTime;
    if (RefreshAccumulator >= 0.20f)
    {
        RefreshAccumulator = 0.0f;
        RefreshSceneState();
        UpdateBlueLabels();
        UpdateRedLabels();
    }

    const bool bShowLabels = CVarTacticalS0Labels.GetValueOnGameThread() != 0;
    for (const TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<UTextRenderComponent>>& Pair : Labels)
    {
        if (UTextRenderComponent* Label = Pair.Value.Get()) Label->SetVisibility(bShowLabels, true);
    }
    if (bShowLabels) UpdateLabelFacing();
    if (CVarTacticalS0HUD.GetValueOnGameThread() != 0) UpdateHUD();
    if (CVarTacticalS0Zones.GetValueOnGameThread() != 0) DrawSceneMarkers();
    UpdateOverviewCamera();
    TryStartAcceptanceDemo();
    UpdateS1AcceptanceDemo();
    UpdateS2AcceptanceDemo();
    UpdateS3AcceptanceDemo();
    TryCaptureAcceptanceScreenshot();
}

void UTacticalMARLVisualizationSubsystem::RefreshSceneState()
{
    Counts = FSceneCounts();
    TSet<FString> BlueIds;
    TSet<FString> RedIds;
    FVector BlueSum = FVector::ZeroVector;
    FVector RedSum = FVector::ZeroVector;

    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        ++Counts.BlueTotal;
        Counts.BlueAlive += !It->HealthComponent || It->HealthComponent->IsAlive() ? 1 : 0;
        BlueIds.Add(It->AgentId.ToString());
        BlueSum += It->GetActorLocation();
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        ++Counts.BlueTotal;
        Counts.BlueAlive += !It->HealthComponent || It->HealthComponent->IsAlive() ? 1 : 0;
        BlueIds.Add(It->AgentId.ToString());
        BlueSum += It->GetActorLocation();
    }

    TSet<const UObject*> VisitedProviders;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UActorComponent*> Components;
        It->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component || VisitedProviders.Contains(Component) ||
                !Component->GetClass()->ImplementsInterface(UTacticalMARLCombatantProvider::StaticClass()))
            {
                continue;
            }
            VisitedProviders.Add(Component);
            const FTacticalMARLCombatantState State =
                ITacticalMARLCombatantProvider::Execute_GetCombatantState(Component);
            if (!State.Actor || State.AgentId.IsNone()) continue;
            ++Counts.RedTotal;
            Counts.RedAlive += State.bAlive ? 1 : 0;
            RedIds.Add(State.AgentId.ToString());
            RedSum += State.Actor->GetActorLocation();
            if (State.bMissionObjective) PrimaryObjectiveLocation = State.Actor->GetActorLocation();
        }
    }

    Counts.bRosterValid = Counts.BlueTotal == ExpectedBlueAgents && BlueIds.Num() == ExpectedBlueAgents &&
        Counts.RedTotal == ExpectedRedAgents && RedIds.Num() == ExpectedRedAgents;
    if (!BlueBaseLocation.IsSet() && Counts.BlueTotal == ExpectedBlueAgents)
    {
        BlueBaseLocation = BlueSum / static_cast<float>(Counts.BlueTotal);
    }
    if (!RedDefenseLocation.IsSet() && Counts.RedTotal == ExpectedRedAgents)
    {
        RedDefenseLocation = RedSum / static_cast<float>(Counts.RedTotal);
    }
    if (Counts.bRosterValid != bLastRosterValid)
    {
        bLastRosterValid = Counts.bRosterValid;
        if (Counts.bRosterValid)
        {
            UE_LOG(LogTacticalS0Visuals, Log,
                TEXT("S0 roster VALID: blue=%d unique=%d red=%d unique=%d"),
                Counts.BlueTotal, BlueIds.Num(), Counts.RedTotal, RedIds.Num());
        }
        else
        {
            UE_LOG(LogTacticalS0Visuals, Error,
                TEXT("S0 roster INVALID: blue=%d unique=%d red=%d unique=%d"),
                Counts.BlueTotal, BlueIds.Num(), Counts.RedTotal, RedIds.Num());
        }
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateBlueLabels()
{
    RosterRows.Reset();
    TSet<TWeakObjectPtr<AActor>> ActiveActors;
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        const FTacticalUAVTelemetry T = It->GetTelemetry();
        const FTacticalAgentHealthState Health = It->HealthComponent
            ? It->HealthComponent->GetHealthState() : FTacticalAgentHealthState();
        UTextRenderComponent* Label = FindOrCreateLabel(*It, BlueColor, 220.0f);
        if (!Label) continue;
        ActiveActors.Add(*It);
        Label->SetText(FText::FromString(FString::Printf(
            TEXT("[BLUE] %s\nUAV | %s\nTask %s / %s\nHP %.0f/%.0f | %s"),
            *T.AgentId.ToString(), *T.AssignedRole.ToString(),
            *UAVTaskToString(T.Task), *UAVStateToString(T.TaskState),
            Health.Health, Health.MaxHealth, Health.bDisabled ? TEXT("DISABLED") : TEXT("ACTIVE"))));
        Label->SetTextRenderColor(Health.bDisabled ? FColor::Red : BlueColor);
        RosterRows.Add(FString::Printf(TEXT("[B] %-12s UAV | %-17s | %-12s | HP %3.0f/%3.0f | %s"),
            *T.AgentId.ToString(), *T.AssignedRole.ToString(), *UAVTaskToString(T.Task),
            Health.Health, Health.MaxHealth, Health.bDisabled ? TEXT("DISABLED") : TEXT("ACTIVE")));
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        const FTacticalUGVTelemetry T = It->GetTelemetry();
        const FTacticalAgentHealthState Health = It->HealthComponent
            ? It->HealthComponent->GetHealthState() : FTacticalAgentHealthState();
        UTextRenderComponent* Label = FindOrCreateLabel(*It, BlueColor, 170.0f);
        if (!Label) continue;
        ActiveActors.Add(*It);
        Label->SetText(FText::FromString(FString::Printf(
            TEXT("[BLUE] %s\nUGV | %s\nTask %s / %s\nHP %.0f/%.0f | %s"),
            *T.AgentId.ToString(), *T.AssignedRole.ToString(),
            *UGVTaskToString(T.Task), *UGVStateToString(T.TaskState),
            Health.Health, Health.MaxHealth, Health.bDisabled ? TEXT("DISABLED") : TEXT("ACTIVE"))));
        Label->SetTextRenderColor(Health.bDisabled ? FColor::Red : BlueColor);
        RosterRows.Add(FString::Printf(TEXT("[B] %-12s UGV | %-17s | %-12s | HP %3.0f/%3.0f | %s"),
            *T.AgentId.ToString(), *T.AssignedRole.ToString(), *UGVTaskToString(T.Task),
            Health.Health, Health.MaxHealth, Health.bDisabled ? TEXT("DISABLED") : TEXT("ACTIVE")));
    }
    RemoveStaleLabels(ActiveActors);
}

void UTacticalMARLVisualizationSubsystem::UpdateRedLabels()
{
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
    const FTacticalAirDefenseState AirState = AirDefense ? AirDefense->GetAirDefenseState() : FTacticalAirDefenseState();
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UActorComponent*> Components;
        It->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component || !Component->GetClass()->ImplementsInterface(UTacticalMARLCombatantProvider::StaticClass())) continue;
            const FTacticalMARLCombatantState State =
                ITacticalMARLCombatantProvider::Execute_GetCombatantState(Component);
            if (!State.Actor || State.AgentId.IsNone()) continue;
            UTextRenderComponent* Label = FindOrCreateLabel(State.Actor, RedColor, 235.0f);
            if (!Label) continue;
            const FTacticalThreatSensorState* Sensor = Threat
                ? Threat->GetSensorStates().FindByPredicate([&State](const FTacticalThreatSensorState& Candidate)
                    { return Candidate.SensorAgentId == State.AgentId; })
                : nullptr;
            const FString AlertState = Sensor
                ? UTacticalThreatSubsystem::AlertStateToString(Sensor->AlertState).ToUpper()
                : TEXT("UNAWARE");
            const float AlertConfidence = Sensor ? Sensor->Confidence : 0.0f;
            const bool bD1 = Threat && Threat->IsThreatSensingEnabled();
            const bool bLauncher = State.AgentId == TEXT("RED_ANTIUAV_01") && AirDefense && AirDefense->IsAirDefenseEnabled();
            const FString PolicyState = bLauncher
                ? FString::Printf(TEXT("D2 %s | Ammo %d | Heat %.0f"),
                    *UTacticalAirDefenseSubsystem::StateToString(AirState.State).ToUpper(),
                    AirState.AmmoRemaining, AirState.Heat)
                : (bD1 ? TEXT("D1 SENSING") : TEXT("D0 PASSIVE"));
            Label->SetText(FText::FromString(FString::Printf(
                TEXT("[RED] %s%s\n%s | %s\nSensor %s %.2f | LOS %s\nHP %.0f/%.0f | %s"),
                *State.AgentId.ToString(), State.bMissionObjective ? TEXT(" [PRIMARY]") : TEXT(""),
                *CompactRole(State.Role), *PolicyState,
                *AlertState, AlertConfidence, Sensor && Sensor->bDirectLineOfSight ? TEXT("DIRECT") : TEXT("NONE"),
                State.Health, State.MaxHealth,
                State.bAlive ? TEXT("ALIVE") : TEXT("NEUTRALIZED"))));
            Label->SetTextRenderColor(bD1 ? AlertColor(Sensor ? Sensor->AlertState : ETacticalRedAlertState::Unaware) :
                (State.bMissionObjective ? ObjectiveColor : RedColor));
            RosterRows.Add(FString::Printf(TEXT("%s %-16s %-9s | %-12s %.2f | HP %3.0f/%3.0f | %s"),
                State.bMissionObjective ? TEXT("[R*]") : TEXT("[R ]"),
                *State.AgentId.ToString(), *CompactRole(State.Role), *AlertState, AlertConfidence,
                State.Health, State.MaxHealth,
                State.bAlive ? TEXT("ALIVE") : TEXT("NEUTRALIZED")));
        }
    }
    RosterRows.Sort();
}

UTextRenderComponent* UTacticalMARLVisualizationSubsystem::FindOrCreateLabel(
    AActor* Actor, const FColor& Color, const float Height)
{
    if (!IsValid(Actor) || !Actor->GetRootComponent()) return nullptr;
    if (TWeakObjectPtr<UTextRenderComponent>* Existing = Labels.Find(Actor))
    {
        if (UTextRenderComponent* Label = Existing->Get()) return Label;
    }

    UTextRenderComponent* Label = NewObject<UTextRenderComponent>(Actor);
    Actor->AddInstanceComponent(Label);
    Label->SetupAttachment(Actor->GetRootComponent());
    Label->SetRelativeLocation(FVector(0.0f, 0.0f, Height));
    Label->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    Label->SetVerticalAlignment(EVerticalTextAligment::EVRTA_TextBottom);
    Label->SetWorldSize(22.0f);
    Label->SetTextRenderColor(Color);
    Label->SetTranslucentSortPriority(100);
    Label->SetBoundsScale(5.0f);
    Label->bAlwaysRenderAsText = true;
    Label->ComponentTags.Add(TEXT("MARL.S0Label"));
    Label->RegisterComponent();
    Labels.Add(Actor, Label);
    return Label;
}

void UTacticalMARLVisualizationSubsystem::RemoveStaleLabels(
    const TSet<TWeakObjectPtr<AActor>>& ActiveBlueActors)
{
    for (auto It = Labels.CreateIterator(); It; ++It)
    {
        AActor* Actor = It.Key().Get();
        if (Actor && Actor->ActorHasTag(TEXT("MARL.Red"))) continue;
        if (!Actor || !ActiveBlueActors.Contains(Actor))
        {
            if (UTextRenderComponent* Label = It.Value().Get()) Label->DestroyComponent();
            It.RemoveCurrent();
        }
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateLabelFacing()
{
    const APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (!PlayerController || !PlayerController->PlayerCameraManager) return;
    const FVector CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
    for (const TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<UTextRenderComponent>>& Pair : Labels)
    {
        UTextRenderComponent* Label = Pair.Value.Get();
        if (!Label) continue;
        Label->SetWorldRotation((CameraLocation - Label->GetComponentLocation()).Rotation());
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateHUD() const
{
    if (!GEngine || ViewportOverlay.IsValid()) return;
    FTacticalMARLEpisodeState Episode;
    if (const UTacticalMARLEpisodeSubsystem* Subsystem = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>())
    {
        Episode = Subsystem->GetEpisodeState();
    }
    FTacticalMARLMissionState Mission;
    if (const UTacticalMARLMissionSubsystem* Subsystem = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        Mission = Subsystem->GetMissionState();
    }
    const int32 DisplaySeed = Episode.Phase == ETacticalMARLEpisodePhase::NotStarted ? DefaultS0Seed : Episode.Seed;
    const FString EpisodePhase = Episode.Phase == ETacticalMARLEpisodePhase::Running ? TEXT("RUNNING") :
        Episode.Phase == ETacticalMARLEpisodePhase::Terminated ? TEXT("TERMINATED") :
        Episode.Phase == ETacticalMARLEpisodePhase::Truncated ? TEXT("TRUNCATED") : TEXT("READY");
    const bool bS1Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"));
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const bool bD1 = Threat && Threat->IsThreatSensingEnabled();
    const FTacticalSharedAlertState Shared = Threat ? Threat->GetSharedAlertState() : FTacticalSharedAlertState();
    const FString SharedState = UTacticalThreatSubsystem::AlertStateToString(Shared.AlertState).ToUpper();

    GEngine->AddOnScreenDebugMessage(
        0x4D41524C01ull, 0.30f, FColor::White,
        bD1
            ? TEXT("TACTICAL MARL | S2 LOCAL PERCEPTION / SHARED ALERT | D1 SENSORS ONLY")
            : bS1Demo
            ? TEXT("TACTICAL MARL | S1 HEALTH / DISABLE ACCEPTANCE | D0 PASSIVE RED")
            : TEXT("TACTICAL MARL | S0 BASELINE | DIFFICULTY D0 (PASSIVE RED)"),
        false, FVector2D(1.25f, 1.25f));
    GEngine->AddOnScreenDebugMessage(
        0x4D41524C02ull, 0.30f, FColor(255, 220, 80),
        FString::Printf(TEXT("EPISODE %d | SEED %d | %s | STEP %d"),
            Episode.EpisodeId, DisplaySeed, *EpisodePhase, Episode.Step), false, FVector2D(1.10f, 1.10f));
    GEngine->AddOnScreenDebugMessage(
        0x4D41524C03ull, 0.30f, FColor(120, 255, 140),
        FString::Printf(TEXT("MISSION PHASE: %s | TRACK %.2f | RED ALERT: %s %.2f"),
            *MissionPhaseToString(static_cast<uint8>(Mission.Phase)).ToUpper(), Mission.TrackQuality,
            *SharedState, Shared.Confidence),
        false, FVector2D(1.05f, 1.05f));
    GEngine->AddOnScreenDebugMessage(
        0x4D41524C04ull, 0.30f, Counts.bRosterValid ? FColor::Cyan : FColor::Red,
        FString::Printf(TEXT("BLUE ALIVE %d/%d | RED ALIVE %d/%d | ROSTER %s"),
            Counts.BlueAlive, ExpectedBlueAgents, Counts.RedAlive, ExpectedRedAgents,
            Counts.bRosterValid ? TEXT("VALID 6+5") : TEXT("WAITING/INVALID")),
        false, FVector2D(1.05f, 1.05f));
    GEngine->AddOnScreenDebugMessage(
        0x4D41524C05ull, 0.30f, FColor(180, 180, 180),
        TEXT("CVars: tacticalmarl.Difficulty / S2Debug / S0HUD / S0Labels / S0Zones / S0OverviewCamera"),
        false, FVector2D(0.85f, 0.85f));
}

void UTacticalMARLVisualizationSubsystem::EnsureViewportOverlay()
{
    if (ViewportOverlay.IsValid() || !GEngine || !GEngine->GameViewport) return;

    TSharedRef<SOverlay> Overlay = SNew(SOverlay)
        + SOverlay::Slot()
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        .Padding(FMargin(24.0f, 70.0f, 0.0f, 0.0f))
        [
            SNew(SBorder)
            .Visibility_Lambda([]()
            {
                return CVarTacticalS0HUD.GetValueOnGameThread() != 0
                    ? EVisibility::Visible
                    : EVisibility::Collapsed;
            })
            .Padding(FMargin(14.0f, 10.0f))
            .BorderBackgroundColor(FLinearColor(0.01f, 0.035f, 0.08f, 0.88f))
            [
                SNew(STextBlock)
                .Text_Lambda([this]() { return FText::FromString(BuildHUDPanelText()); })
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 17))
                .ColorAndOpacity(FLinearColor(0.75f, 0.95f, 1.0f, 1.0f))
                .ShadowOffset(FVector2D(1.5f, 1.5f))
                .ShadowColorAndOpacity(FLinearColor::Black)
            ]
        ]
        + SOverlay::Slot()
        .HAlign(HAlign_Right)
        .VAlign(VAlign_Top)
        .Padding(FMargin(0.0f, 70.0f, 24.0f, 0.0f))
        [
            SNew(SBorder)
            .Visibility_Lambda([]()
            {
                return CVarTacticalS0HUD.GetValueOnGameThread() != 0
                    ? EVisibility::Visible
                    : EVisibility::Collapsed;
            })
            .Padding(FMargin(14.0f, 10.0f))
            .BorderBackgroundColor(FLinearColor(0.035f, 0.02f, 0.02f, 0.88f))
            [
                SNew(STextBlock)
                .Text_Lambda([this]() { return FText::FromString(BuildRosterPanelText()); })
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 13))
                .ColorAndOpacity(FLinearColor::White)
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .ShadowColorAndOpacity(FLinearColor::Black)
            ]
        ];

    ViewportOverlay = Overlay;
    GEngine->GameViewport->AddViewportWidgetContent(Overlay, 10000);
}

void UTacticalMARLVisualizationSubsystem::RemoveViewportOverlay()
{
    if (ViewportOverlay.IsValid() && GEngine && GEngine->GameViewport)
    {
        GEngine->GameViewport->RemoveViewportWidgetContent(ViewportOverlay.ToSharedRef());
    }
    ViewportOverlay.Reset();
}

FString UTacticalMARLVisualizationSubsystem::BuildHUDPanelText() const
{
    FTacticalMARLEpisodeState Episode;
    if (const UTacticalMARLEpisodeSubsystem* Subsystem = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>())
    {
        Episode = Subsystem->GetEpisodeState();
    }
    FTacticalMARLMissionState Mission;
    if (const UTacticalMARLMissionSubsystem* Subsystem = GetWorld()->GetSubsystem<UTacticalMARLMissionSubsystem>())
    {
        Mission = Subsystem->GetMissionState();
    }
    const int32 DisplaySeed = Episode.Phase == ETacticalMARLEpisodePhase::NotStarted ? DefaultS0Seed : Episode.Seed;
    const FString EpisodePhase = Episode.Phase == ETacticalMARLEpisodePhase::Running ? TEXT("RUNNING") :
        Episode.Phase == ETacticalMARLEpisodePhase::Terminated ? TEXT("TERMINATED") :
        Episode.Phase == ETacticalMARLEpisodePhase::Truncated ? TEXT("TRUNCATED") : TEXT("READY");
    const bool bS1Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"));
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const bool bD1 = Threat && Threat->IsThreatSensingEnabled();
    const FTacticalSharedAlertState Shared = Threat ? Threat->GetSharedAlertState() : FTacticalSharedAlertState();
    const FString SharedState = UTacticalThreatSubsystem::AlertStateToString(Shared.AlertState).ToUpper();
    const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
    const bool bD2 = AirDefense && AirDefense->IsAirDefenseEnabled();
    const FTacticalAirDefenseState AirState = AirDefense ? AirDefense->GetAirDefenseState() : FTacticalAirDefenseState();
    return FString::Printf(
        TEXT("TACTICAL MARL  |  %s\n")
        TEXT("DIFFICULTY %s  |  RED POLICY: %s\n")
        TEXT("Episode %d  |  Seed %d  |  %s  |  Step %d\n")
        TEXT("MISSION PHASE: %s  |  Track %.2f\n")
        TEXT("RED ALERT: %s %.2f  |  Source %s  |  Target %s\n")
        TEXT("S2 STAGE: %s  |  Delay %.2fs  |  Messages %d\n")
        TEXT("AIR DEFENSE: %s  |  Target %s  |  Ammo %d/%d  |  Heat %.0f/%.0f\n")
        TEXT("Warning %.2fs  |  TTI %.2fs  |  Last %s  |  P(hit) %.2f\n")
        TEXT("BLUE ALIVE %d/6  |  RED ALIVE %d/5\n")
        TEXT("ROSTER: %s\n")
        TEXT("Toggle: tacticalmarl.Difficulty / S2Debug / S3Debug / S0HUD / S0Labels / S0Zones / S0OverviewCamera"),
        bD2 ? TEXT("S3 ANTI-UAV DEFENSE / DETERMINISTIC ATTACK") :
            bD1 ? TEXT("S2 LOCAL PERCEPTION / SHARED ALERT") :
            (bS1Demo ? TEXT("S1 HEALTH / DISABLE ACCEPTANCE") : TEXT("S0 BASELINE")),
        bD2 ? TEXT("D2") : (bD1 ? TEXT("D1") : TEXT("D0")),
        bD2 ? TEXT("LOCAL SENSORS + ANTI-UAV") : (bD1 ? TEXT("LOCAL SENSORS / NO ATTACK") : TEXT("PASSIVE")),
        Episode.EpisodeId, DisplaySeed, *EpisodePhase, Episode.Step,
        *MissionPhaseToString(static_cast<uint8>(Mission.Phase)).ToUpper(), Mission.TrackQuality,
        *SharedState, Shared.Confidence, *Shared.SourceAgentId.ToString(), *Shared.TargetAgentId.ToString(),
        Threat ? *Threat->GetAcceptanceStage() : TEXT("none"), Shared.LastPropagationDelay, Shared.DeliveredMessageCount,
        *UTacticalAirDefenseSubsystem::StateToString(AirState.State).ToUpper(), *AirState.TargetAgentId.ToString(),
        AirState.AmmoRemaining, AirDefense ? AirDefense->AmmoCapacity : 0, AirState.Heat,
        AirDefense ? AirDefense->MaximumHeat : 0.0f,
        AirState.WarningRemainingSeconds, AirState.EstimatedTimeToImpact,
        *AirState.LastShotResult.ToUpper(), AirState.LastHitProbability,
        Counts.BlueAlive, Counts.RedAlive, Counts.bRosterValid ? TEXT("VALID 6+5") : TEXT("WAITING/INVALID"));
}

FString UTacticalMARLVisualizationSubsystem::BuildRosterPanelText() const
{
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    const FTacticalSharedAlertState Shared = Threat ? Threat->GetSharedAlertState() : FTacticalSharedAlertState();
    const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
    const FTacticalAirDefenseState AirState = AirDefense ? AirDefense->GetAirDefenseState() : FTacticalAirDefenseState();
    FString Result = TEXT("UNIT ROSTER  |  JSON AGENT IDs\n");
    Result += TEXT("Side  Agent ID          Type/Role   | Current task  | Health/Status\n");
    Result += TEXT("--------------------------------------------------------------------------\n");
    for (const FString& Row : RosterRows)
    {
        Result += Row;
        Result += TEXT("\n");
    }
    Result += FString::Printf(TEXT("\nR* = PRIMARY OBJECTIVE  |  RED SHARED ALERT: %s %.2f  |  %s\nAIR DEFENSE: %s | Target %s | Ammo %d | Last %s"),
        *UTacticalThreatSubsystem::AlertStateToString(Shared.AlertState).ToUpper(), Shared.Confidence,
        AirDefense && AirDefense->IsAirDefenseEnabled() ? TEXT("D2 ANTI-UAV ACTIVE") :
            (Threat && Threat->IsThreatSensingEnabled() ? TEXT("D1 SENSORS / NO ATTACK") : TEXT("D0 PASSIVE")),
        *UTacticalAirDefenseSubsystem::StateToString(AirState.State).ToUpper(),
        *AirState.TargetAgentId.ToString(), AirState.AmmoRemaining, *AirState.LastShotResult.ToUpper());
    return Result;
}

void UTacticalMARLVisualizationSubsystem::DrawSceneMarkers() const
{
    for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
    {
        const FVector Location = It->GetActorLocation();
        const bool bDisabled = It->HealthComponent && It->HealthComponent->IsDisabled();
        const FColor Color = bDisabled ? FColor::Red : BlueColor;
        DrawDebugSphere(GetWorld(), Location, 180.0f, 16, Color, false, 0.25f, 0, 12.0f);
        DrawDebugLine(GetWorld(), Location, Location + FVector(0, 0, 500), Color, false, 0.25f, 0, 9.0f);
    }
    for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
    {
        const FVector Location = It->GetActorLocation();
        const bool bDisabled = It->HealthComponent && It->HealthComponent->IsDisabled();
        const FColor Color = bDisabled ? FColor::Red : BlueColor;
        DrawDebugSphere(GetWorld(), Location, 150.0f, 16, Color, false, 0.25f, 0, 12.0f);
        DrawDebugLine(GetWorld(), Location, Location + FVector(0, 0, 420), Color, false, 0.25f, 0, 9.0f);
    }
    for (const TPair<TWeakObjectPtr<AActor>, TWeakObjectPtr<UTextRenderComponent>>& Pair : Labels)
    {
        AActor* Actor = Pair.Key.Get();
        if (!Actor || !Actor->ActorHasTag(TEXT("MARL.Red"))) continue;
        const FVector Location = Actor->GetActorLocation();
        DrawDebugSphere(GetWorld(), Location, 120.0f, 16, RedColor, false, 0.25f, 0, 11.0f);
        DrawDebugLine(GetWorld(), Location, Location + FVector(0, 0, 420), RedColor, false, 0.25f, 0, 8.0f);
    }
    if (BlueBaseLocation.IsSet())
    {
        DrawDebugCircle(GetWorld(), BlueBaseLocation.GetValue() + FVector(0, 0, 20), 1100.0f, 48,
            BlueColor, false, 0.25f, 0, 20.0f, FVector(1, 0, 0), FVector(0, 1, 0), false);
        DrawDebugString(GetWorld(), BlueBaseLocation.GetValue() + FVector(0, 1500, 450),
            TEXT("BLUE BASE | 3 UAV + 3 UGV"), nullptr, BlueColor, 0.25f, false, 1.6f);
    }
    if (RedDefenseLocation.IsSet())
    {
        const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
        const bool bD1 = Threat && Threat->IsThreatSensingEnabled();
        const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
        const bool bD2 = AirDefense && AirDefense->IsAirDefenseEnabled();
        DrawDebugCircle(GetWorld(), RedDefenseLocation.GetValue() + FVector(0, 0, 20), 1500.0f, 48,
            RedColor, false, 0.25f, 0, 20.0f, FVector(1, 0, 0), FVector(0, 1, 0), false);
        DrawDebugString(GetWorld(), RedDefenseLocation.GetValue() + FVector(0, -1700, 500),
            bD2 ? TEXT("RED DEFENSE ZONE | D2 ANTI-UAV ACTIVE") :
                (bD1 ? TEXT("RED DEFENSE ZONE | D1 LOCAL SENSORS / NO ATTACK") : TEXT("RED DEFENSE ZONE | D0 PASSIVE")),
            nullptr, RedColor, 0.25f, false, 1.6f);
    }
    if (PrimaryObjectiveLocation.IsSet())
    {
        const FVector Location = PrimaryObjectiveLocation.GetValue();
        DrawDebugSphere(GetWorld(), Location, 360.0f, 24, ObjectiveColor, false, 0.25f, 0, 18.0f);
        DrawDebugLine(GetWorld(), Location + FVector(0, 0, 50), Location + FVector(0, 0, 1300),
            ObjectiveColor, false, 0.25f, 0, 22.0f);
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo")) && bS1DisableDamageApplied)
    {
        for (const TWeakObjectPtr<AActor>& Target : {S1UAVTarget, S1UGVTarget})
        {
            if (!Target.IsValid()) continue;
            DrawDebugCircle(GetWorld(), Target->GetActorLocation() + FVector(0, 0, 30), 310.0f, 24,
                FColor::Red, false, 0.25f, 0, 20.0f, FVector(1, 0, 0), FVector(0, 1, 0), false);
            DrawDebugString(GetWorld(), Target->GetActorLocation() + FVector(0, 0, 520),
                TEXT("S1 TEST DAMAGE | DISABLED / RETAINED"), nullptr, FColor::Red, 0.25f, false, 1.4f);
        }
    }
    const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    if (Threat && Threat->IsThreatSensingEnabled() && CVarTacticalS2Debug.GetValueOnGameThread() != 0)
    {
        for (const FTacticalThreatSensorState& Sensor : Threat->GetSensorStates())
        {
            AActor* SensorActor = Sensor.SensorActor.Get();
            if (!SensorActor) continue;
            const FVector Origin = SensorActor->GetActorLocation() + FVector(0, 0, 110);
            const FVector Forward = SensorActor->GetActorForwardVector().GetSafeNormal();
            const float DebugLength = FMath::Min(Sensor.SensorRange, 3000.0f);
            const FVector Left = Forward.RotateAngleAxis(-Sensor.FieldOfViewDegrees * 0.5f, FVector::UpVector);
            const FVector Right = Forward.RotateAngleAxis(Sensor.FieldOfViewDegrees * 0.5f, FVector::UpVector);
            const FColor Color = AlertColor(Sensor.AlertState);
            DrawDebugLine(GetWorld(), Origin, Origin + Left * DebugLength, Color, false, 0.25f, 0, 10.0f);
            DrawDebugLine(GetWorld(), Origin, Origin + Right * DebugLength, Color, false, 0.25f, 0, 10.0f);
            DrawDebugLine(GetWorld(), Origin, Origin + Forward * DebugLength, Color, false, 0.25f, 0, 5.0f);
            if (Sensor.bDirectLineOfSight && !Sensor.TargetAgentId.IsNone())
            {
                DrawDebugLine(GetWorld(), Origin, Sensor.LastKnownLocation, FColor::Green, false, 0.25f, 0, 18.0f);
                DrawDebugString(GetWorld(), Sensor.LastKnownLocation + FVector(0, 0, 260),
                    FString::Printf(TEXT("DIRECT LOS | %s | %.2f"), *Sensor.TargetAgentId.ToString(), Sensor.Confidence),
                    nullptr, FColor::Green, 0.25f, false, 1.2f);
            }
            else if (Sensor.bLastTraceBlocked)
            {
                DrawDebugLine(GetWorld(), Origin, Sensor.LastTraceEnd, FColor::Orange, false, 0.25f, 0, 14.0f);
                DrawDebugPoint(GetWorld(), Sensor.LastTraceEnd, 35.0f, FColor::Orange, false, 0.25f);
                DrawDebugString(GetWorld(), Sensor.LastTraceEnd + FVector(0, 0, 100),
                    TEXT("LOS BLOCKED | NO DETECTION"), nullptr, FColor::Orange, 0.25f, false, 1.1f);
            }
        }
        const FTacticalSharedAlertState Shared = Threat->GetSharedAlertState();
        if (!Shared.TargetAgentId.IsNone())
        {
            const FColor SharedColor = AlertColor(Shared.AlertState);
            DrawDebugSphere(GetWorld(), Shared.LastKnownLocation, 260.0f, 20, SharedColor, false, 0.25f, 0, 16.0f);
            DrawDebugString(GetWorld(), Shared.LastKnownLocation + FVector(0, 0, 520),
                FString::Printf(TEXT("SHARED %s | LAST KNOWN | %.2f"),
                    *UTacticalThreatSubsystem::AlertStateToString(Shared.AlertState).ToUpper(), Shared.Confidence),
                nullptr, SharedColor, 0.25f, false, 1.4f);
        }
    }
    const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
    if (AirDefense && AirDefense->IsAirDefenseEnabled() && CVarTacticalS3Debug.GetValueOnGameThread() != 0)
    {
        const FTacticalAirDefenseState AirState = AirDefense->GetAirDefenseState();
        AActor* Launcher = AirDefense->GetLauncherActor();
        AActor* Target = AirDefense->GetCurrentTargetActor();
        if (Launcher)
        {
            const FVector Origin = Launcher->GetActorLocation() + FVector(0, 0, 180);
            if (AirState.State == ETacticalAirDefenseState::Scanning)
            {
                const float Angle = FMath::Fmod(GetWorld()->GetTimeSeconds() * 75.0f, 140.0f) - 70.0f;
                const FVector ScanDirection = Launcher->GetActorForwardVector().RotateAngleAxis(Angle, FVector::UpVector);
                DrawDebugLine(GetWorld(), Origin, Origin + ScanDirection * 2800.0f,
                    FColor::Cyan, false, 0.25f, 0, 18.0f);
                DrawDebugString(GetWorld(), Origin + FVector(0, 0, 260),
                    TEXT("D2 ANTI-UAV | SECTOR SCANNING"), nullptr, FColor::Cyan, 0.25f, false, 1.25f);
            }
            if (Target && !AirState.TargetAgentId.IsNone())
            {
                const FColor LockColor = AirState.State == ETacticalAirDefenseState::Warning ? FColor::Red :
                    (AirState.State == ETacticalAirDefenseState::Locking ? FColor::Yellow : FColor(255, 140, 0));
                DrawDebugLine(GetWorld(), Origin, AirState.LastPerceivedLocation,
                    LockColor, false, 0.25f, 0, AirState.State == ETacticalAirDefenseState::Warning ? 28.0f : 16.0f);
                if (AirState.State == ETacticalAirDefenseState::Warning)
                {
                    DrawDebugSphere(GetWorld(), AirState.LastPerceivedLocation, 330.0f, 24,
                        FColor::Red, false, 0.25f, 0, 22.0f);
                    DrawDebugString(GetWorld(), AirState.LastPerceivedLocation + FVector(0, 0, 420),
                        FString::Printf(TEXT("MISSILE LOCK WARNING | LAUNCH %.1fs"), AirState.WarningRemainingSeconds),
                        nullptr, FColor::Red, 0.25f, false, 1.6f);
                }
                else if (AirState.State == ETacticalAirDefenseState::LostLock)
                {
                    DrawDebugString(GetWorld(), AirState.LastPerceivedLocation + FVector(0, 0, 420),
                        TEXT("LOCK BROKEN | LOS BLOCKED | NO FIRE"), nullptr, FColor::Green, 0.25f, false, 1.5f);
                }
            }
            if (AirState.State == ETacticalAirDefenseState::Launching)
            {
                const FVector Projectile = AirDefense->GetProjectileLocation();
                DrawDebugLine(GetWorld(), Origin, Projectile, FColor::Red, false, 0.25f, 0, 22.0f);
                DrawDebugPoint(GetWorld(), Projectile, 42.0f, FColor::Yellow, false, 0.25f);
                if (Target)
                {
                    DrawDebugString(GetWorld(), Target->GetActorLocation() + FVector(0, 0, 500),
                        FString::Printf(TEXT("MISSILE INCOMING | TTI %.2fs"), AirState.EstimatedTimeToImpact),
                        nullptr, FColor::Red, 0.25f, false, 1.7f);
                }
            }
        }
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateOverviewCamera()
{
    const bool bShouldUseCamera = CVarTacticalS0OverviewCamera.GetValueOnGameThread() != 0 &&
        GetWorld()->GetMapName().Contains(TEXT("L_MARL_UrbanDepot"));
    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (!bShouldUseCamera || !PlayerController || !Counts.bRosterValid ||
        !BlueBaseLocation.IsSet() || !RedDefenseLocation.IsSet())
    {
        if (!bShouldUseCamera) RestorePreviousViewTarget();
        return;
    }

    if (!OverviewCamera.IsValid())
    {
        FActorSpawnParameters Params;
        Params.Name = TEXT("TacticalMARL_S0_OverviewCamera");
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Params.ObjectFlags |= RF_Transient;
        OverviewCamera = GetWorld()->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Params);
        if (ACameraActor* Camera = OverviewCamera.Get()) Camera->GetCameraComponent()->SetFieldOfView(66.0f);
    }
    ACameraActor* Camera = OverviewCamera.Get();
    if (!Camera) return;
    if (PlayerController->GetViewTarget() != Camera)
    {
        PreviousViewTarget = PlayerController->GetViewTarget();
        PlayerController->SetViewTarget(Camera);
    }

    const FVector Blue = BlueBaseLocation.GetValue();
    const FVector Red = RedDefenseLocation.GetValue();
    const bool bS1Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"));
    const bool bS2Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2AutoDemo"));
    const bool bS3Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3AutoDemo"));
    const FVector Center = (bS2Demo || bS3Demo)
        ? Red + FVector(900, 0, 350)
        : bS1Demo
            ? Blue + FVector(0, 0, 180)
            : (Blue + Red) * 0.5f + FVector(0, 0, 250);
    const float Span = (bS2Demo || bS3Demo) ? 4200.0f :
        (bS1Demo ? 3800.0f : FMath::Max(8000.0f, FVector::Dist2D(Blue, Red)));
    const FVector CameraLocation = Center + FVector(-Span * 0.72f, -Span * 0.72f, Span * 1.18f);
    Camera->SetActorLocationAndRotation(CameraLocation, (Center - CameraLocation).Rotation());
}

void UTacticalMARLVisualizationSubsystem::RestorePreviousViewTarget()
{
    APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (PlayerController && OverviewCamera.IsValid() && PlayerController->GetViewTarget() == OverviewCamera.Get() && PreviousViewTarget.IsValid())
    {
        PlayerController->SetViewTarget(PreviousViewTarget.Get());
    }
    PreviousViewTarget.Reset();
}

void UTacticalMARLVisualizationSubsystem::TryStartAcceptanceDemo()
{
    const bool bS0Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS0AutoDemo"));
    const bool bS1Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"));
    const bool bS2Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2AutoDemo"));
    const bool bS3Demo = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3AutoDemo"));
    if (bAutoDemoStarted || (!bS0Demo && !bS1Demo && !bS2Demo && !bS3Demo) ||
        !Counts.bRosterValid || GetWorld()->GetTimeSeconds() < 35.0f)
    {
        return;
    }
    if (UTacticalMARLEpisodeSubsystem* Episode = GetWorld()->GetSubsystem<UTacticalMARLEpisodeSubsystem>())
    {
        if (Episode->ResetEpisode(DefaultS0Seed))
        {
            bAutoDemoStarted = true;
            AutoDemoStartedAt = GetWorld()->GetTimeSeconds();
            UE_LOG(LogTacticalS0Visuals, Display, TEXT("%s acceptance demo started with seed 42"),
                bS3Demo ? TEXT("S3") : (bS2Demo ? TEXT("S2") : (bS1Demo ? TEXT("S1") : TEXT("S0"))));
        }
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateS1AcceptanceDemo()
{
    if (!bAutoDemoStarted || !FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1AutoDemo"))) return;
    const double Elapsed = GetWorld()->GetTimeSeconds() - AutoDemoStartedAt;
    if (!S1UAVTarget.IsValid())
    {
        for (TActorIterator<ATacticalUAVPawn> It(GetWorld()); It; ++It)
        {
            if (It->AgentId == TEXT("BLUE_UAV_01")) { S1UAVTarget = *It; break; }
        }
    }
    if (!S1UGVTarget.IsValid())
    {
        for (TActorIterator<ATacticalUGVPawn> It(GetWorld()); It; ++It)
        {
            if (It->AgentId == TEXT("BLUE_UGV_01")) { S1UGVTarget = *It; break; }
        }
    }
    if (!bS1PartialDamageApplied && Elapsed >= 1.0)
    {
        for (const TWeakObjectPtr<AActor>& Target : {S1UAVTarget, S1UGVTarget})
        {
            if (UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Target.Get()))
            {
                Health->ApplyAgentDamage(35.0f, nullptr);
            }
        }
        bS1PartialDamageApplied = true;
        UE_LOG(LogTacticalS0Visuals, Display, TEXT("S1 demo applied 35 damage to BLUE_UAV_01 and BLUE_UGV_01"));
    }
    if (!bS1DisableDamageApplied && Elapsed >= 2.5)
    {
        for (const TWeakObjectPtr<AActor>& Target : {S1UAVTarget, S1UGVTarget})
        {
            if (UTacticalAgentHealthComponent* Health = UTacticalAgentHealthComponent::FindHealthComponent(Target.Get()))
            {
                // Deliberately exceeds MaxHealth so an external reset between
                // the two visual stages cannot leave the acceptance target alive.
                Health->ApplyAgentDamage(200.0f, nullptr);
            }
        }
        bS1DisableDamageApplied = true;
        UE_LOG(LogTacticalS0Visuals, Display, TEXT("S1 demo disabled BLUE_UAV_01 and BLUE_UGV_01; actors retained"));
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateS2AcceptanceDemo()
{
    if (!bAutoDemoStarted || !FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2AutoDemo"))) return;
    UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
    if (!Threat) return;
    const double Elapsed = GetWorld()->GetTimeSeconds() - AutoDemoStartedAt;
    FString Error;
    if (!bS2OccludedStageApplied && Elapsed >= 1.0)
    {
        bS2OccludedStageApplied = Threat->SetAcceptanceStage(TEXT("occluded"), Error);
        if (!bS2OccludedStageApplied) UE_LOG(LogTacticalS0Visuals, Error, TEXT("S2 occluded stage failed: %s"), *Error);
    }
    if (!bS2VisibleStageApplied && Elapsed >= 3.2)
    {
        bS2VisibleStageApplied = Threat->SetAcceptanceStage(TEXT("visible"), Error);
        if (!bS2VisibleStageApplied) UE_LOG(LogTacticalS0Visuals, Error, TEXT("S2 visible stage failed: %s"), *Error);
    }
    if (!bS2LostStageApplied && Elapsed >= 7.4)
    {
        bS2LostStageApplied = Threat->SetAcceptanceStage(TEXT("lost_contact"), Error);
        if (!bS2LostStageApplied) UE_LOG(LogTacticalS0Visuals, Error, TEXT("S2 lost-contact stage failed: %s"), *Error);
    }
}

void UTacticalMARLVisualizationSubsystem::UpdateS3AcceptanceDemo()
{
    if (!bAutoDemoStarted || !FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3AutoDemo"))) return;
    UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
    if (!AirDefense) return;
    const double Elapsed = GetWorld()->GetTimeSeconds() - AutoDemoStartedAt;
    FString Error;
    if (!bS3VisibleStageApplied && Elapsed >= 1.0)
    {
        bS3VisibleStageApplied = AirDefense->SetAcceptanceStage(TEXT("visible_hit"), Error);
        if (!bS3VisibleStageApplied) UE_LOG(LogTacticalS0Visuals, Error, TEXT("S3 visible-hit stage failed: %s"), *Error);
    }
    const FTacticalAirDefenseState State = AirDefense->GetAirDefenseState();
    // After one resolved shot and cooldown, allow a second lock to form, then
    // insert the acceptance wall during its warning window to prove no launch.
    if (!bS3LostStageApplied && State.ShotCount >= 1 && State.State == ETacticalAirDefenseState::Warning)
    {
        bS3LostStageApplied = AirDefense->SetAcceptanceStage(TEXT("lost_lock"), Error);
        if (!bS3LostStageApplied) UE_LOG(LogTacticalS0Visuals, Error, TEXT("S3 lost-lock stage failed: %s"), *Error);
    }
}

void UTacticalMARLVisualizationSubsystem::TryCaptureAcceptanceScreenshot()
{
    const bool bS3Capture = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS3Capture"));
    if (bS3Capture && bAutoDemoStarted)
    {
        const UTacticalAirDefenseSubsystem* AirDefense = GetWorld()->GetSubsystem<UTacticalAirDefenseSubsystem>();
        if (!AirDefense) return;
        const FTacticalAirDefenseState State = AirDefense->GetAirDefenseState();
        FString Filename;
        bool* Flag = nullptr;
        if (!bS3WarningScreenshotRequested && State.ShotCount == 0 && State.State == ETacticalAirDefenseState::Warning)
        {
            Filename = TEXT("S3_LockWarning_seed42.png");
            Flag = &bS3WarningScreenshotRequested;
        }
        else if (!bS3ImpactScreenshotRequested && State.ShotCount >= 1 &&
            State.State == ETacticalAirDefenseState::Cooldown && State.StateElapsedSeconds >= 0.25f)
        {
            Filename = TEXT("S3_Impact_seed42.png");
            Flag = &bS3ImpactScreenshotRequested;
        }
        else if (!bS3LostScreenshotRequested && State.ShotCount >= 1 &&
            State.State == ETacticalAirDefenseState::LostLock && State.StateElapsedSeconds >= 0.25f)
        {
            Filename = TEXT("S3_LostLock_seed42.png");
            Flag = &bS3LostScreenshotRequested;
        }
        if (Flag)
        {
            const FString Directory = FPaths::ProjectSavedDir() / TEXT("TacticalMARL/Screenshots");
            FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);
            const FString FullPath = Directory / Filename;
            FScreenshotRequest::RequestScreenshot(FullPath, true, false);
            *Flag = true;
            UE_LOG(LogTacticalS0Visuals, Display, TEXT("Requested S3 acceptance screenshot: %s"), *FullPath);
        }
        return;
    }
    const bool bS2Capture = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS2Capture"));
    if (bS2Capture && bAutoDemoStarted)
    {
        const double Elapsed = GetWorld()->GetTimeSeconds() - AutoDemoStartedAt;
        FString Filename;
        bool* Flag = nullptr;
        if (!bS2OccludedScreenshotRequested && Elapsed >= 2.2 && Elapsed < 3.1)
        {
            Filename = TEXT("S2_Occluded_seed42.png");
            Flag = &bS2OccludedScreenshotRequested;
        }
        else if (!bS2TrackingScreenshotRequested && Elapsed >= 6.4 && Elapsed < 7.3)
        {
            Filename = TEXT("S2_Tracking_seed42.png");
            Flag = &bS2TrackingScreenshotRequested;
        }
        else if (!bS2LostScreenshotRequested && Elapsed >= 9.5)
        {
            const UTacticalThreatSubsystem* Threat = GetWorld()->GetSubsystem<UTacticalThreatSubsystem>();
            if (Threat && Threat->GetSharedAlertState().AlertState == ETacticalRedAlertState::LostContact)
            {
                Filename = TEXT("S2_LostContact_seed42.png");
                Flag = &bS2LostScreenshotRequested;
            }
        }
        if (Flag)
        {
            const FString Directory = FPaths::ProjectSavedDir() / TEXT("TacticalMARL/Screenshots");
            FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);
            const FString FullPath = Directory / Filename;
            FScreenshotRequest::RequestScreenshot(FullPath, true, false);
            *Flag = true;
            UE_LOG(LogTacticalS0Visuals, Display, TEXT("Requested S2 acceptance screenshot: %s"), *FullPath);
        }
        return;
    }
    const bool bS1Capture = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS1Capture"));
    const bool bS0Capture = FParse::Param(FCommandLine::Get(), TEXT("TacticalMARLS0Capture"));
    const double RequiredDelay = bS1Capture ? 4.5 : 3.0;
    if (bScreenshotRequested || (!bS0Capture && !bS1Capture) ||
        !bAutoDemoStarted || GetWorld()->GetTimeSeconds() - AutoDemoStartedAt < RequiredDelay)
    {
        return;
    }
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("TacticalMARL/Screenshots");
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);
    const FString Filename = Directory /
        (bS1Capture ? TEXT("S1_HealthAndDisable_seed42.png") : TEXT("S0_UrbanDepot_seed42.png"));
    FScreenshotRequest::RequestScreenshot(Filename, true, false);
    bScreenshotRequested = true;
    UE_LOG(LogTacticalS0Visuals, Display, TEXT("Requested %s acceptance screenshot: %s"),
        bS1Capture ? TEXT("S1") : TEXT("S0"), *Filename);
}

FString UTacticalMARLVisualizationSubsystem::MissionPhaseToString(const uint8 PhaseValue)
{
    switch (static_cast<ETacticalMARLMissionPhase>(PhaseValue))
    {
    case ETacticalMARLMissionPhase::CandidateFound: return TEXT("candidate_found");
    case ETacticalMARLMissionPhase::TargetConfirmed: return TEXT("target_confirmed");
    case ETacticalMARLMissionPhase::TrackEstablished: return TEXT("track_established");
    case ETacticalMARLMissionPhase::StrikeReady: return TEXT("strike_ready");
    case ETacticalMARLMissionPhase::Engagement: return TEXT("engagement");
    case ETacticalMARLMissionPhase::EffectAssessment: return TEXT("effect_assessment");
    case ETacticalMARLMissionPhase::Success: return TEXT("success");
    case ETacticalMARLMissionPhase::Failed: return TEXT("failed");
    default: return TEXT("search");
    }
}
