// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSpawnTabArgs;
class SDockTab;

class FLuxMeterCalcModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnComparisonTab(const FSpawnTabArgs& Args);
	void OpenPanelClicked();
	void OpenComparisonClicked();

	FDelegateHandle ToolMenusHandle;
};
