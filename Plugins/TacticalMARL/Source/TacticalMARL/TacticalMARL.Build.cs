using UnrealBuildTool;

public class TacticalMARL : ModuleRules
{
    public TacticalMARL(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "Json",
            "JsonUtilities",
            "NavigationSystem",
            "Sockets",
            "Networking",
            "Slate",
            "SlateCore"
        });
    }
}
