using UnrealBuildTool;

public class CyclopsTarget : TargetRules
{
	public CyclopsTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Cyclops");
	}
}
