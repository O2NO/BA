// Copyright Epic Games, Inc. All Rights Reserved.

#include "LuxMeterCalc.h"
#include "LuxMeterStyle.h"
#include "SLuxMeterPanel.h"
#include "SLuxMeterComparisonPanel.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FLuxMeterCalcModule"

namespace LuxMeterCalcTabs
{
	static const FName PanelTabName(TEXT("LuxMeterPanel"));
	static const FName ComparisonTabName(TEXT("LuxMeterComparisonPanel"));
}

void FLuxMeterCalcModule::StartupModule()
{
	FLuxMeterStyle::Initialize();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		LuxMeterCalcTabs::PanelTabName,
		FOnSpawnTab::CreateRaw(this, &FLuxMeterCalcModule::SpawnPanelTab))
		.SetDisplayName(LOCTEXT("PanelTabTitle", "LuxMeter"))
		.SetTooltipText(LOCTEXT("PanelTabTooltip", "Open the LuxMeter measurement panel"))
		.SetIcon(FSlateIcon(FLuxMeterStyle::GetStyleSetName(), "LuxMeter.TabIcon"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		LuxMeterCalcTabs::ComparisonTabName,
		FOnSpawnTab::CreateRaw(this, &FLuxMeterCalcModule::SpawnComparisonTab))
		.SetDisplayName(LOCTEXT("ComparisonTabTitle", "LuxMeter Comparison"))
		.SetTooltipText(LOCTEXT("ComparisonTabTooltip", "Compare a real-world recording (Optitrack tracking + lightmeter CSV) against the calculator"))
		.SetIcon(FSlateIcon(FLuxMeterStyle::GetStyleSetName(), "LuxMeter.TabIcon"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	ToolMenusHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLuxMeterCalcModule::RegisterMenus));
}

void FLuxMeterCalcModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (FGlobalTabmanager::Get()->HasTabSpawner(LuxMeterCalcTabs::PanelTabName))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LuxMeterCalcTabs::PanelTabName);
	}
	if (FGlobalTabmanager::Get()->HasTabSpawner(LuxMeterCalcTabs::ComparisonTabName))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LuxMeterCalcTabs::ComparisonTabName);
	}

	FLuxMeterStyle::Shutdown();
}

void FLuxMeterCalcModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Window menu entry — most reliable discovery surface.
	if (UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window"))
	{
		FToolMenuSection& Section = WindowMenu->FindOrAddSection("LuxMeter", LOCTEXT("LuxMeterSection", "LuxMeter"));
		Section.AddMenuEntry(
			"OpenLuxMeterPanel",
			LOCTEXT("OpenPanelLabel", "LuxMeter"),
			LOCTEXT("OpenPanelTooltip", "Open the LuxMeter measurement panel"),
			FSlateIcon(FLuxMeterStyle::GetStyleSetName(), "LuxMeter.OpenPanel.Small"),
			FUIAction(FExecuteAction::CreateRaw(this, &FLuxMeterCalcModule::OpenPanelClicked)));
		Section.AddMenuEntry(
			"OpenLuxMeterComparison",
			LOCTEXT("OpenComparisonLabel", "LuxMeter Comparison"),
			LOCTEXT("OpenComparisonTooltip", "Open the comparison panel for real-world Optitrack + lightmeter data"),
			FSlateIcon(FLuxMeterStyle::GetStyleSetName(), "LuxMeter.OpenPanel.Small"),
			FUIAction(FExecuteAction::CreateRaw(this, &FLuxMeterCalcModule::OpenComparisonClicked)));
	}

	// Optional toolbar button — best-effort; menu path may differ between UE versions.
	if (UToolMenu* ToolBar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar"))
	{
		FToolMenuSection& Section = ToolBar->FindOrAddSection("LuxMeter");
		FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
			"OpenLuxMeterPanel",
			FUIAction(FExecuteAction::CreateRaw(this, &FLuxMeterCalcModule::OpenPanelClicked)),
			LOCTEXT("OpenPanelLabel", "LuxMeter"),
			LOCTEXT("OpenPanelTooltip", "Open the LuxMeter measurement panel"),
			FSlateIcon(FLuxMeterStyle::GetStyleSetName(), "LuxMeter.OpenPanel", "LuxMeter.OpenPanel.Small"));
		Entry.SetCommandList(nullptr);
		Section.AddEntry(Entry);
	}
}

TSharedRef<SDockTab> FLuxMeterCalcModule::SpawnPanelTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SLuxMeterPanel)
		];
}

TSharedRef<SDockTab> FLuxMeterCalcModule::SpawnComparisonTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SLuxMeterComparisonPanel)
		];
}

void FLuxMeterCalcModule::OpenPanelClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(LuxMeterCalcTabs::PanelTabName);
}

void FLuxMeterCalcModule::OpenComparisonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(LuxMeterCalcTabs::ComparisonTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLuxMeterCalcModule, LuxMeterCalc)
