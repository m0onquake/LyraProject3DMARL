#include "Weapons/TacticalWeaponInstances.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(TacticalWeaponInstances)

UTacticalSniperWeaponInstance::UTacticalSniperWeaponInstance(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    SpreadExponent = 5.0f;
    MaxDamageRange = 65000.0f;
    BulletTraceSweepRadius = 1.0f;
    BulletsPerCartridge = 1;
    bAllowFirstShotAccuracy = true;
    SpreadAngleMultiplier_Aiming = 0.20f;
    SpreadAngleMultiplier_StandingStill = 0.25f;
    SpreadRecoveryCooldownDelay = 0.15f;

    HeatToSpreadCurve.EditorCurveData.Reset();
    HeatToSpreadCurve.EditorCurveData.AddKey(0.0f, 0.05f);
    HeatToSpreadCurve.EditorCurveData.AddKey(1.0f, 0.30f);

    HeatToHeatPerShotCurve.EditorCurveData.Reset();
    HeatToHeatPerShotCurve.EditorCurveData.AddKey(0.0f, 0.40f);

    HeatToCoolDownPerSecondCurve.EditorCurveData.Reset();
    HeatToCoolDownPerSecondCurve.EditorCurveData.AddKey(0.0f, 1.50f);
}

UTacticalAntiUAVWeaponInstance::UTacticalAntiUAVWeaponInstance(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    SpreadExponent = 2.5f;
    MaxDamageRange = 45000.0f;
    BulletTraceSweepRadius = 45.0f;
    BulletsPerCartridge = 3;
    bAllowFirstShotAccuracy = true;
    SpreadAngleMultiplier_Aiming = 0.55f;
    SpreadAngleMultiplier_StandingStill = 0.65f;
    SpreadRecoveryCooldownDelay = 0.05f;

    HeatToSpreadCurve.EditorCurveData.Reset();
    HeatToSpreadCurve.EditorCurveData.AddKey(0.0f, 0.35f);
    HeatToSpreadCurve.EditorCurveData.AddKey(1.0f, 1.25f);

    HeatToHeatPerShotCurve.EditorCurveData.Reset();
    HeatToHeatPerShotCurve.EditorCurveData.AddKey(0.0f, 0.25f);

    HeatToCoolDownPerSecondCurve.EditorCurveData.Reset();
    HeatToCoolDownPerSecondCurve.EditorCurveData.AddKey(0.0f, 2.0f);
}
