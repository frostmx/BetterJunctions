using UnrealBuildTool;

public class BetterJunctions : ModuleRules
{
	public BetterJunctions(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });

		// FactoryGame for the autopilot component and the vehicle subsystem, SML for native hooks.
		PublicDependencyModuleNames.AddRange(new[] { "FactoryGame", "SML" });
	}
}
