#include "LuxMeterStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

TSharedPtr<FSlateStyleSet> FLuxMeterStyle::StyleInstance = nullptr;

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

void FLuxMeterStyle::Initialize()
{
	if (StyleInstance.IsValid())
	{
		return;
	}
	StyleInstance = Create();
	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FLuxMeterStyle::Shutdown()
{
	if (!StyleInstance.IsValid())
	{
		return;
	}
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	StyleInstance.Reset();
}

FName FLuxMeterStyle::GetStyleSetName()
{
	static const FName StyleSetName(TEXT("LuxMeterStyle"));
	return StyleSetName;
}

const ISlateStyle& FLuxMeterStyle::Get()
{
	return *StyleInstance;
}

TSharedRef<FSlateStyleSet> FLuxMeterStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LuxMeterCalc"));
	if (Plugin.IsValid())
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	const FVector2D Icon20(20.f, 20.f);
	const FVector2D Icon40(40.f, 40.f);
	const FVector2D Icon128(128.f, 128.f);

	Style->Set("LuxMeter.OpenPanel", new IMAGE_BRUSH(TEXT("Icon128"), Icon40));
	Style->Set("LuxMeter.OpenPanel.Small", new IMAGE_BRUSH(TEXT("Icon128"), Icon20));
	Style->Set("LuxMeter.TabIcon", new IMAGE_BRUSH(TEXT("Icon128"), Icon20));

	return Style;
}

#undef IMAGE_BRUSH
