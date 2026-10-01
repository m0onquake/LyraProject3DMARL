#include "AI/TacticalRedBotSpawnPoint.h"
#include "AI/TacticalRedBotStateComponent.h"
#include "Character/LyraHealthComponent.h"

#include "AIController.h"
#include "BrainComponent.h"
#include "Character/LyraPawnExtensionComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Cosmetics/LyraCharacterPartTypes.h"
#include "Cosmetics/LyraPawnComponent_CharacterParts.h"
#include "Development/LyraDeveloperSettings.h"
#include "Engine/World.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameModes/LyraBotCreationComponent.h"
#include "GameModes/LyraExperienceManagerComponent.h"
#include "GameModes/LyraGameMode.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Player/LyraPlayerState.h"
#include "Teams/LyraTeamSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(TacticalRedBotSpawnPoint)

DEFINE_LOG_CATEGORY_STATIC(LogTacticalRedBot, Log, All);

ATacticalRedBotSpawnPoint::ATacticalRedBotSpawnPoint()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;

    // A root component is required for editor-spawned instances to retain their
    // configured world transform. Without it, every spawn point resolves to
    // the origin and all bot pawns collide during initialization.
    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);
    CombatState = CreateDefaultSubobject<UTacticalRedBotStateComponent>(TEXT("MARLCombatState"));

    // ShooterCore is a Game Feature plugin and is not mounted when this native
    // class default object is created. Keep the reference soft until the
    // experience has finished loading the plugin.
    BotControllerClass = TSoftClassPtr<AAIController>(FSoftObjectPath(
        TEXT("/ShooterCore/Bot/B_AI_Controller_LyraShooter.B_AI_Controller_LyraShooter_C")));
    CharacterPartClass = TSoftClassPtr<AActor>(FSoftObjectPath(
        TEXT("/Game/Characters/Cosmetics/B_Manny.B_Manny_C")));
}

void ATacticalRedBotSpawnPoint::BeginPlay()
{
    Super::BeginPlay();

    if (!HasAuthority())
    {
        return;
    }

    if (AGameStateBase* GameState = GetWorld()->GetGameState())
    {
        if (ULyraExperienceManagerComponent* Experience = GameState->FindComponentByClass<ULyraExperienceManagerComponent>())
        {
            Experience->CallOrRegister_OnExperienceLoaded_HighPriority(
                FOnLyraExperienceLoaded::FDelegate::CreateUObject(
                    this, &ThisClass::HandleExperienceLoaded));
        }
    }
}

void ATacticalRedBotSpawnPoint::HandleExperienceLoaded(const ULyraExperienceDefinition* Experience)
{
    // The stock Elimination experience adds B_ShooterBotSpawner. Suppress its
    // low-priority callback before editor or URL overrides can restore a bot
    // count, so this map contains only the local player and the deterministic
    // TacticalMARL force while no external Python episode is connected.
    if (AGameStateBase* GameState = GetWorld()->GetGameState())
    {
        if (ULyraBotCreationComponent* BotCreation = GameState->FindComponentByClass<ULyraBotCreationComponent>())
        {
            BotCreation->SuppressAutomaticBotCreation();
        }
    }

    GetWorldTimerManager().SetTimer(SpawnTimer, this, &ThisClass::SpawnConfiguredBot, 0.20f, false);
}

void ATacticalRedBotSpawnPoint::SpawnConfiguredBot()
{
    UClass* LoadedControllerClass = BotControllerClass.LoadSynchronous();
    if (!LoadedControllerClass)
    {
        UE_LOG(LogTacticalRedBot, Error, TEXT("%s has no BotControllerClass"), *AgentId.ToString());
        return;
    }

    ALyraGameMode* GameMode = GetWorld()->GetAuthGameMode<ALyraGameMode>();
    if (!GameMode)
    {
        UE_LOG(LogTacticalRedBot, Error, TEXT("%s requires ALyraGameMode"), *AgentId.ToString());
        return;
    }

    FActorSpawnParameters SpawnInfo;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnInfo.ObjectFlags |= RF_Transient;
    SpawnedController = GetWorld()->SpawnActor<AAIController>(
        LoadedControllerClass, FVector::ZeroVector, FRotator::ZeroRotator, SpawnInfo);

    if (!SpawnedController)
    {
        UE_LOG(LogTacticalRedBot, Error, TEXT("Failed to spawn controller for %s"), *AgentId.ToString());
        return;
    }

    GameMode->GenericPlayerInitialization(SpawnedController);
    if (ALyraPlayerState* PlayerState = SpawnedController->GetPlayerState<ALyraPlayerState>())
    {
        PlayerState->SetPlayerName(AgentId.ToString());
        PlayerState->SetGenericTeamId(FGenericTeamId(static_cast<uint8>(RedTeamId)));
    }

    GameMode->RestartPlayerAtTransform(SpawnedController, GetActorTransform());
    if (APawn* Pawn = SpawnedController->GetPawn())
    {
        ConfigureSpawnedPawn(Pawn);
        SetBotAwaitingEpisode(true);

        const FRotator PawnRotation = Pawn->GetActorRotation();
        UE_LOG(
            LogTacticalRedBot,
            Log,
            TEXT("Pawn ready %s location=%s rotation=(P=%.1f Y=%.1f R=%.1f) hidden=%s"),
            *AgentId.ToString(),
            *Pawn->GetActorLocation().ToCompactString(),
            PawnRotation.Pitch,
            PawnRotation.Yaw,
            PawnRotation.Roll,
            Pawn->IsHidden() ? TEXT("true") : TEXT("false"));
    }

    LoadoutRetryCount = 0;
    GetWorldTimerManager().SetTimer(LoadoutTimer, this, &ThisClass::ApplyConfiguredLoadout, LoadoutDelay, false);
    UE_LOG(LogTacticalRedBot, Log, TEXT("Spawned %s role=%s team=%d"), *AgentId.ToString(), *GetRoleTag().ToString(), RedTeamId);
}

void ATacticalRedBotSpawnPoint::SetBotAwaitingEpisode(const bool bAwaitingEpisode)
{
    if (!IsValid(SpawnedController)) return;

    SpawnedController->StopMovement();
    if (UBrainComponent* Brain = SpawnedController->GetBrainComponent())
    {
        if (bAwaitingEpisode)
        {
            Brain->PauseLogic(TEXT("Waiting for TacticalMARL episode reset"));
        }
        else
        {
            Brain->ResumeLogic(TEXT("TacticalMARL episode started"));
        }
    }

    UE_LOG(
        LogTacticalRedBot,
        Log,
        TEXT("%s %s at fixed spawn %s"),
        *AgentId.ToString(),
        bAwaitingEpisode ? TEXT("waiting for episode") : TEXT("released for episode"),
        *GetActorLocation().ToCompactString());
}

bool ATacticalRedBotSpawnPoint::ResetBotForEpisode()
{
    if (!HasAuthority()) return false;
    if (!IsValid(SpawnedController))
    {
        SpawnedController = nullptr;
        SpawnConfiguredBot();
        const bool bReady = IsValid(SpawnedController) && IsValid(SpawnedController->GetPawn());
        if (bReady) SetBotAwaitingEpisode(bPassiveInDifficultyD0);
        return bReady;
    }
    APawn* Pawn = SpawnedController ? SpawnedController->GetPawn() : nullptr;
    ULyraHealthComponent* Health = ULyraHealthComponent::FindHealthComponent(Pawn);
    if (Pawn && Health && !Health->IsDeadOrDying())
    {
        Pawn->SetActorTransform(GetActorTransform(), false, nullptr, ETeleportType::TeleportPhysics);
        CombatState->RestoreFullHealth();
        ConfigureSpawnedPawn(Pawn);
        SetBotAwaitingEpisode(bPassiveInDifficultyD0);
        return true;
    }

    if (IsValid(Pawn))
    {
        SpawnedController->UnPossess();
        Pawn->Destroy();
    }
    SpawnedController->Destroy();
    SpawnedController = nullptr;
    SpawnConfiguredBot();
    const bool bReady = IsValid(SpawnedController) && IsValid(SpawnedController->GetPawn());
    if (bReady) SetBotAwaitingEpisode(bPassiveInDifficultyD0);
    return bReady;
}

void ATacticalRedBotSpawnPoint::ConfigureSpawnedPawn(APawn* Pawn)
{
    if (!Pawn) return;
    // Manually spawned Lyra pawns begin hidden while the stock cosmetic path
    // initializes. Tactical visuals and equipment must already be visible in
    // Warmup, before any external Python episode exists.
    Pawn->SetActorHiddenInGame(false);
    Pawn->Tags.AddUnique(TEXT("MARL.Agent"));
    Pawn->Tags.AddUnique(TEXT("MARL.Red"));
    Pawn->Tags.AddUnique(TEXT("ReconTarget"));
    Pawn->Tags.AddUnique(GetRoleTag());
    const bool bObjective = bMissionObjective || BotRole == ETacticalRedBotRole::AntiUAV;
    if (bObjective) Pawn->Tags.AddUnique(TEXT("MARL.MissionTarget"));
    if (ULyraPawnExtensionComponent* PawnExtension = Pawn->FindComponentByClass<ULyraPawnExtensionComponent>())
    {
        PawnExtension->CheckDefaultInitialization();
    }
    CombatState->Configure(AgentId, GetRoleTag(), bObjective);
    CombatState->BindToPawn(Pawn);
    if (BotRole == ETacticalRedBotRole::AntiUAV) EnsureLauncherVehicleVisual(Pawn);
}

void ATacticalRedBotSpawnPoint::ApplyConfiguredLoadout()
{
    APawn* Pawn = SpawnedController ? SpawnedController->GetPawn() : nullptr;
    ULyraInventoryManagerComponent* Inventory = Pawn ? Pawn->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
    if (!Inventory && SpawnedController)
    {
        Inventory = SpawnedController->FindComponentByClass<ULyraInventoryManagerComponent>();
    }
    ULyraQuickBarComponent* QuickBar = SpawnedController ? SpawnedController->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
    if (!QuickBar && Pawn)
    {
        QuickBar = Pawn->FindComponentByClass<ULyraQuickBarComponent>();
    }
    UClass* ItemClass = WeaponItemDefinition.LoadSynchronous();

    if (!Pawn || !Inventory || !QuickBar || !ItemClass)
    {
        ScheduleLoadoutRetry();
        return;
    }

    const TArray<ULyraInventoryItemInstance*> Slots = QuickBar->GetSlots();
    for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
    {
        if (ULyraInventoryItemInstance* Removed = QuickBar->RemoveItemFromSlot(SlotIndex))
        {
            Inventory->RemoveItemInstance(Removed);
        }
    }

    for (ULyraInventoryItemInstance* ExistingItem : Inventory->GetAllItems())
    {
        Inventory->RemoveItemInstance(ExistingItem);
    }

    ULyraInventoryItemInstance* WeaponItem = Inventory->AddItemDefinition(ItemClass, 1);
    if (!WeaponItem)
    {
        ScheduleLoadoutRetry();
        return;
    }

    QuickBar->AddItemToSlot(0, WeaponItem);
    QuickBar->SetActiveSlotIndex(0);
    Pawn->SetActorHiddenInGame(false);
    if (BotRole == ETacticalRedBotRole::AntiUAV) EnsureLauncherVehicleVisual(Pawn);
    else EnsureVisibleCharacterPart(Pawn);
    UE_LOG(
        LogTacticalRedBot,
        Log,
        TEXT("Applied loadout %s to %s"),
        *ItemClass->GetPathName(),
        *AgentId.ToString());
}

void ATacticalRedBotSpawnPoint::EnsureVisibleCharacterPart(APawn* Pawn)
{
    ULyraPawnComponent_CharacterParts* PawnCustomizer =
        Pawn ? Pawn->FindComponentByClass<ULyraPawnComponent_CharacterParts>() : nullptr;
    if (!PawnCustomizer)
    {
        UE_LOG(LogTacticalRedBot, Warning, TEXT("%s has no pawn character-parts component"), *AgentId.ToString());
        return;
    }

    auto RevealPart = [](AActor* PartActor)
    {
        if (!PartActor)
        {
            return false;
        }

        bool bHasSkeletalMesh = false;
        PartActor->SetActorHiddenInGame(false);
        TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
        PartActor->GetComponents(PrimitiveComponents);
        for (UPrimitiveComponent* Primitive : PrimitiveComponents)
        {
            if (Primitive)
            {
                Primitive->SetVisibility(true, true);
                Primitive->SetHiddenInGame(false, true);
            }
            if (const USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Primitive))
            {
                bHasSkeletalMesh |= SkeletalMesh->GetSkeletalMeshAsset() != nullptr;
            }
        }
        return bHasSkeletalMesh;
    };

    TArray<AActor*> PartActors = PawnCustomizer->GetCharacterPartActors();
    bool bHasVisibleCharacterMesh = false;
    for (AActor* PartActor : PartActors)
    {
        bHasVisibleCharacterMesh |= RevealPart(PartActor);
    }

    if (!bHasVisibleCharacterMesh)
    {
        UClass* LoadedPartClass = CharacterPartClass.LoadSynchronous();
        if (!LoadedPartClass)
        {
            UE_LOG(LogTacticalRedBot, Error, TEXT("Unable to load fallback character part for %s"), *AgentId.ToString());
            return;
        }

        FLyraCharacterPart Part;
        Part.PartClass = LoadedPartClass;
        PawnCustomizer->AddCharacterPart(Part);

        PartActors = PawnCustomizer->GetCharacterPartActors();
        for (AActor* PartActor : PartActors)
        {
            RevealPart(PartActor);
        }
    }

    UE_LOG(
        LogTacticalRedBot,
        Log,
        TEXT("Character visuals ready %s parts=%d fallbackAdded=%s"),
        *AgentId.ToString(),
        PartActors.Num(),
        bHasVisibleCharacterMesh ? TEXT("false") : TEXT("true"));
}

void ATacticalRedBotSpawnPoint::EnsureLauncherVehicleVisual(APawn* Pawn)
{
    if (!Pawn) return;

    TInlineComponentArray<USkeletalMeshComponent*> SkeletalMeshes;
    Pawn->GetComponents(SkeletalMeshes);
    for (USkeletalMeshComponent* Mesh : SkeletalMeshes)
    {
        if (Mesh) { Mesh->SetVisibility(false, true); Mesh->SetHiddenInGame(true, true); }
    }
    if (ULyraPawnComponent_CharacterParts* PawnCustomizer = Pawn->FindComponentByClass<ULyraPawnComponent_CharacterParts>())
    {
        for (AActor* PartActor : PawnCustomizer->GetCharacterPartActors())
        {
            if (PartActor) PartActor->SetActorHiddenInGame(true);
        }
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    UMaterialInterface* BasicMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    auto EnsurePart = [Pawn, BasicMaterial](
        FName Name, UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation,
        const FVector& Scale, const FLinearColor& Color)
    {
        UStaticMeshComponent* Part = nullptr;
        TInlineComponentArray<UStaticMeshComponent*> ExistingParts;
        Pawn->GetComponents(ExistingParts);
        for (UStaticMeshComponent* Existing : ExistingParts)
        {
            if (Existing && Existing->GetFName() == Name) { Part = Existing; break; }
        }
        if (!Part)
        {
            Part = NewObject<UStaticMeshComponent>(Pawn, Name);
            Pawn->AddInstanceComponent(Part);
            Part->SetupAttachment(Pawn->GetRootComponent());
            Part->RegisterComponent();
        }
        Part->SetStaticMesh(Mesh);
        Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Part->SetRelativeLocation(Location);
        Part->SetRelativeRotation(Rotation);
        Part->SetRelativeScale3D(Scale);
        Part->SetVisibility(true, true);
        Part->SetHiddenInGame(false, true);
        Part->ComponentTags.AddUnique(TEXT("MARL.LauncherVisual"));
        if (BasicMaterial)
        {
            UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(BasicMaterial, Pawn);
            Material->SetVectorParameterValue(TEXT("Color"), Color);
            Part->SetMaterial(0, Material);
        }
    };

    const FLinearColor Olive(0.12f, 0.22f, 0.08f);
    const FLinearColor DarkOlive(0.06f, 0.10f, 0.04f);
    EnsurePart(TEXT("LauncherChassis"), Cube, FVector(0, 0, -55), FRotator::ZeroRotator, FVector(2.8f, 1.45f, 0.45f), Olive);
    EnsurePart(TEXT("LauncherCab"), Cube, FVector(-85, 0, -5), FRotator::ZeroRotator, FVector(0.9f, 1.3f, 0.85f), Olive);
    EnsurePart(TEXT("LauncherRack"), Cube, FVector(75, 0, 30), FRotator(-15, 0, 0), FVector(1.45f, 1.0f, 0.22f), DarkOlive);
    EnsurePart(TEXT("LauncherTubeL"), Cylinder, FVector(65, -42, 70), FRotator(75, 0, 0), FVector(0.20f, 0.20f, 1.15f), DarkOlive);
    EnsurePart(TEXT("LauncherTubeR"), Cylinder, FVector(65, 42, 70), FRotator(75, 0, 0), FVector(0.20f, 0.20f, 1.15f), DarkOlive);
}

void ATacticalRedBotSpawnPoint::ShowLauncherHitFeedback()
{
    if (BotRole != ETacticalRedBotRole::AntiUAV || !IsValid(SpawnedController)) return;
    APawn* Pawn = SpawnedController->GetPawn();
    if (!Pawn) return;

    UMaterialInterface* BasicMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    TInlineComponentArray<UStaticMeshComponent*> Parts;
    Pawn->GetComponents(Parts);
    for (UStaticMeshComponent* Part : Parts)
    {
        if (!Part || !Part->ComponentHasTag(TEXT("MARL.LauncherVisual"))) continue;
        Part->SetRelativeScale3D(Part->GetRelativeScale3D() * 1.12f);
        if (BasicMaterial)
        {
            UMaterialInstanceDynamic* HitMaterial = UMaterialInstanceDynamic::Create(BasicMaterial, Pawn);
            HitMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.02f, 0.01f));
            Part->SetMaterial(0, HitMaterial);
        }
    }

    TInlineComponentArray<UTextRenderComponent*> ExistingText;
    Pawn->GetComponents(ExistingText);
    for (UTextRenderComponent* Text : ExistingText)
    {
        if (Text && Text->ComponentHasTag(TEXT("MARL.LauncherHitFeedback"))) Text->DestroyComponent();
    }
    UTextRenderComponent* HitText = NewObject<UTextRenderComponent>(Pawn);
    Pawn->AddInstanceComponent(HitText);
    HitText->SetupAttachment(Pawn->GetRootComponent());
    HitText->SetRelativeLocation(FVector(0, 0, 220));
    HitText->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    HitText->SetText(FText::FromString(TEXT("HIT")));
    HitText->SetTextRenderColor(FColor::Red);
    HitText->SetWorldSize(85.0f);
    HitText->ComponentTags.Add(TEXT("MARL.LauncherHitFeedback"));
    HitText->RegisterComponent();

    GetWorldTimerManager().ClearTimer(DamageFeedbackTimer);
    GetWorldTimerManager().SetTimer(DamageFeedbackTimer, this, &ThisClass::RestoreLauncherVehicleVisual, 0.65f, false);
}

void ATacticalRedBotSpawnPoint::RestoreLauncherVehicleVisual()
{
    APawn* Pawn = IsValid(SpawnedController) ? SpawnedController->GetPawn() : nullptr;
    if (!Pawn) return;
    TInlineComponentArray<UTextRenderComponent*> TextComponents;
    Pawn->GetComponents(TextComponents);
    for (UTextRenderComponent* Text : TextComponents)
    {
        if (Text && Text->ComponentHasTag(TEXT("MARL.LauncherHitFeedback"))) Text->DestroyComponent();
    }
    EnsureLauncherVehicleVisual(Pawn);
}

void ATacticalRedBotSpawnPoint::ScheduleLoadoutRetry()
{
    ++LoadoutRetryCount;
    if (LoadoutRetryCount <= 10)
    {
        GetWorldTimerManager().SetTimer(LoadoutTimer, this, &ThisClass::ApplyConfiguredLoadout, 0.50f, false);
    }
    else
    {
        UE_LOG(LogTacticalRedBot, Error, TEXT("Loadout initialization timed out for %s"), *AgentId.ToString());
    }
}

FName ATacticalRedBotSpawnPoint::GetRoleTag() const
{
    switch (BotRole)
    {
    case ETacticalRedBotRole::Sniper:
        return TEXT("MARL.Role.Sniper");
    case ETacticalRedBotRole::AntiUAV:
        return TEXT("MARL.Role.AntiUAV");
    default:
        return TEXT("MARL.Role.Rifleman");
    }
}
