using UnrealBuildTool;

public class CyclopsEditorTarget : TargetRules
{
	public CyclopsEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Cyclops");
	}
}
