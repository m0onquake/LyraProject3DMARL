#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TacticalPolicySubsystem.generated.h"

class FSocket;

/**
 * Game-thread UDP bridge for external Python policies.
 * It binds only to 127.0.0.1 and routes JSON by agent_id.
 */
UCLASS()
class TACTICALMARL_API UTacticalPolicySubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Policy") bool StartUdpBridge(int32 Port = 7777);
    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Policy") void StopUdpBridge();
    UFUNCTION(BlueprintPure, Category="Tactical MARL|Policy") bool IsUdpBridgeRunning() const { return Socket != nullptr; }

    // Direct in-process interface for Blueprint, UE Python and test harnesses.
    UFUNCTION(BlueprintCallable, Category="Tactical MARL|Policy") bool SubmitCommandJson(const FString& JsonCommand, FString& OutResponse);
    UFUNCTION(BlueprintPure, Category="Tactical MARL|Policy") FString GetObservationsJson() const;

private:
    bool RouteCommand(const FString& JsonCommand, FString& OutError);
    FString BuildResponse(bool bOk, const FString& Error = FString()) const;

    FSocket* Socket = nullptr;
    int32 ListeningPort = 0;
};
