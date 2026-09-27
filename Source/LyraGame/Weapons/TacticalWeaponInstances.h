#pragma once

#include "Weapons/LyraRangedWeaponInstance.h"
#include "TacticalWeaponInstances.generated.h"

/** Long-range, low-spread rifle profile used by the red marksman role. */
UCLASS(BlueprintType)
class LYRAGAME_API UTacticalSniperWeaponInstance : public ULyraRangedWeaponInstance
{
    GENERATED_BODY()

public:
    UTacticalSniperWeaponInstance(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Multi-trace rifle profile intended to make small aerial targets easier to engage. */
UCLASS(BlueprintType)
class LYRAGAME_API UTacticalAntiUAVWeaponInstance : public ULyraRangedWeaponInstance
{
    GENERATED_BODY()

public:
    UTacticalAntiUAVWeaponInstance(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
