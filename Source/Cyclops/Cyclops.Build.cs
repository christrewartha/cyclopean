using UnrealBuildTool;

public class Cyclops : ModuleRules
{
	public Cyclops(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
		});

		// WristPerfPanel: frame timings, RHI draw stats, motion controllers.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RHI",
			"RenderCore",
			"HeadMountedDisplay",
			// Leaderboard client.
			"HTTP",
			"Json",
		});
	}
}
