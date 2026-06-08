// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LuxMeterCalc : ModuleRules
{
	public LuxMeterCalc(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Projects",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"UnrealEd",
				"LevelEditor",
				"ToolMenus",
				"WorkspaceMenuStructure",
				"PropertyEditor",
				"AssetTools",
				"AssetRegistry",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
				"DesktopPlatform",
			}
		);
	}
}
