#include "SLuxMeterPanel.h"

#include "LuxIESLightActor.h"
#include "LuxLightmeterActor.h"
#include "LuxReflectorActor.h"
#include "LuxCalcClient.h"
#include "LuxMeasurementTypes.h"
#include "LuxMeterCalcSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Components/SpotLightComponent.h"
#include "EditorViewportClient.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/Texture.h"
#include "EditorFramework/AssetImportData.h"
#include "Framework/Notifications/NotificationManager.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Views/STableRow.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SLuxMeterPanel"

namespace
{
	constexpr float IncidentCalibrationConstant = 250.f; // K for incident-meter equation N^2 = E*t*S/K

	TSharedPtr<FExposurePreset> MakePreset(float Value, FString Label)
	{
		TSharedPtr<FExposurePreset> P = MakeShared<FExposurePreset>();
		P->Value = Value;
		P->Label = FText::FromString(MoveTemp(Label));
		return P;
	}

	TSharedPtr<FExposurePreset> FindPreset(const TArray<TSharedPtr<FExposurePreset>>& Arr, float Value)
	{
		for (const TSharedPtr<FExposurePreset>& P : Arr)
		{
			if (FMath::IsNearlyEqual(P->Value, Value, 0.001f))
			{
				return P;
			}
		}
		return Arr.Num() > 0 ? Arr[0] : TSharedPtr<FExposurePreset>();
	}
}

void SLuxMeterPanel::Construct(const FArguments& InArgs)
{
	// --- Initialize exposure state ---
	FPS = 24.0f;
	ISO = 800.0f;
	ShutterAngle = 172.8f;

	FPSPresets.Reset();
	FPSPresets.Add(MakePreset(23.976f, TEXT("23.976 fps")));
	FPSPresets.Add(MakePreset(24.0f,   TEXT("24 fps")));
	FPSPresets.Add(MakePreset(25.0f,   TEXT("25 fps")));
	FPSPresets.Add(MakePreset(29.97f,  TEXT("29.97 fps")));
	FPSPresets.Add(MakePreset(30.0f,   TEXT("30 fps")));
	FPSPresets.Add(MakePreset(48.0f,   TEXT("48 fps")));
	FPSPresets.Add(MakePreset(50.0f,   TEXT("50 fps")));
	FPSPresets.Add(MakePreset(60.0f,   TEXT("60 fps")));

	ISOPresets.Reset();
	ISOPresets.Add(MakePreset(100.f,  TEXT("ISO 100")));
	ISOPresets.Add(MakePreset(200.f,  TEXT("ISO 200")));
	ISOPresets.Add(MakePreset(400.f,  TEXT("ISO 400")));
	ISOPresets.Add(MakePreset(800.f,  TEXT("ISO 800")));
	ISOPresets.Add(MakePreset(1600.f, TEXT("ISO 1600")));
	ISOPresets.Add(MakePreset(3200.f, TEXT("ISO 3200")));
	ISOPresets.Add(MakePreset(6400.f, TEXT("ISO 6400")));

	ShutterPresets.Reset();
	ShutterPresets.Add(MakePreset(45.0f,   TEXT("45°")));
	ShutterPresets.Add(MakePreset(90.0f,   TEXT("90°")));
	ShutterPresets.Add(MakePreset(135.0f,  TEXT("135°")));
	ShutterPresets.Add(MakePreset(144.0f,  TEXT("144°")));
	ShutterPresets.Add(MakePreset(172.8f,  TEXT("172.8°")));
	ShutterPresets.Add(MakePreset(180.0f,  TEXT("180°")));
	ShutterPresets.Add(MakePreset(270.0f,  TEXT("270°")));
	ShutterPresets.Add(MakePreset(360.0f,  TEXT("360°")));

	SelectedFPSPreset     = FindPreset(FPSPresets, FPS);
	SelectedISOPreset     = FindPreset(ISOPresets, ISO);
	SelectedShutterPreset = FindPreset(ShutterPresets, ShutterAngle);

	// --- Subscribe to editor selection changes ---
	SelectionChangedHandle = USelection::SelectionChangedEvent.AddRaw(this, &SLuxMeterPanel::OnEditorSelectionChanged);
	ObjectSelectedHandle   = USelection::SelectObjectEvent.AddRaw(this, &SLuxMeterPanel::OnEditorSelectionChanged);
	RefreshSelectedFixtures();

	RefreshIESList();

	// --- UI ---
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(8.f)
		[
			SNew(SVerticalBox)

			// ---- IES Library section ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("IESLibraryHeader", "IES Library"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("IESSearchHint", "Search IES profiles..."))
				.OnTextChanged(this, &SLuxMeterPanel::OnIESSearchChanged)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(2.f)
				[
					SNew(SBox).HeightOverride(180.f)
					[
						SAssignNew(IESListView, SListView<TSharedPtr<FAssetData>>)
						.ListItemsSource(&IESItems)
						.OnGenerateRow(this, &SLuxMeterPanel::OnGenerateIESRow)
						.OnSelectionChanged_Lambda(
							[this](TSharedPtr<FAssetData> Item, ESelectInfo::Type)
							{
								SelectedAsset = Item;
							})
						.SelectionMode(ESelectionMode::Single)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("SpawnLight", "Spawn light"))
					.ToolTipText(LOCTEXT("SpawnLightTooltip", "Spawn a new light fixture using the selected IES profile"))
					.OnClicked(this, &SLuxMeterPanel::HandleSpawnLight)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ApplyToSelected", "Apply to selected"))
					.ToolTipText(LOCTEXT("ApplyToSelectedTooltip", "Replace the IES profile of the currently selected fixture(s) with the chosen library entry. Transform is preserved."))
					.OnClicked(this, &SLuxMeterPanel::HandleApplyIESToSelected)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportIES", "Import..."))
					.ToolTipText(LOCTEXT("ImportIESTooltip", "Pick one or more .ies files and import them into /Game/IES"))
					.OnClicked(this, &SLuxMeterPanel::HandleImportIES)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshIES", "Reload"))
					.OnClicked_Lambda(
						[this]()
						{
							RefreshIESList();
							return FReply::Handled();
						})
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4) [ SNew(SSeparator) ]

			// ---- Selected Fixture section (visible only when an IES light is selected) ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.Visibility(this, &SLuxMeterPanel::GetSelectedFixtureVisibility)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("HeadingSmall"))
						.Text(LOCTEXT("SelectedFixtureHeader", "Selected Fixture"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
					[
						SNew(STextBlock).Text(this, &SLuxMeterPanel::GetSelectedFixtureNameText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock).Text(this, &SLuxMeterPanel::GetSelectedFixtureIESText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[
							SNew(STextBlock).Text(LOCTEXT("ViewportIntensityLabel", "Viewport intensity (lm):"))
						]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.f)
							.MaxValue(200000.f)
							.MinSliderValue(0.f)
							.MaxSliderValue(200000.f)
							.SliderExponent(5.f)
							.Delta(1.f)
							.Value(this, &SLuxMeterPanel::GetSelectedIntensity)
							.OnValueChanged(this, &SLuxMeterPanel::OnSelectedIntensityChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnSelectedIntensityCommitted)
							.ToolTipText(LOCTEXT("ViewportIntensityTooltip", "Drives only the spotlight's rendered brightness in the editor. Has NO effect on the calculated illuminance. Slider is logarithmic — fine control at low lumens, broad reach at the top."))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8) [ SNew(SSeparator) ]
				]
			]

			// ---- Selected Reflector section (visible only when a reflector is selected) ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.Visibility(this, &SLuxMeterPanel::GetSelectedReflectorVisibility)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("HeadingSmall"))
						.Text(LOCTEXT("SelectedReflectorHeader", "Selected Reflector"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock).Text(this, &SLuxMeterPanel::GetSelectedReflectorNameText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(SBox).WidthOverride(110.f) [ SNew(STextBlock).Text(LOCTEXT("ReflectorWidth", "Width (cm)")) ] ]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(1.f).MaxValue(500.f)
							.MinSliderValue(1.f).MaxSliderValue(500.f)
							.Delta(1.f)
							.Value(this, &SLuxMeterPanel::GetSelectedReflectorWidth)
							.OnValueChanged(this, &SLuxMeterPanel::OnSelectedReflectorWidthChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnSelectedReflectorWidthCommitted)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(SBox).WidthOverride(110.f) [ SNew(STextBlock).Text(LOCTEXT("ReflectorHeight", "Height (cm)")) ] ]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(1.f).MaxValue(500.f)
							.MinSliderValue(1.f).MaxSliderValue(500.f)
							.Delta(1.f)
							.Value(this, &SLuxMeterPanel::GetSelectedReflectorHeight)
							.OnValueChanged(this, &SLuxMeterPanel::OnSelectedReflectorHeightChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnSelectedReflectorHeightCommitted)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[ SNew(SBox).WidthOverride(110.f) [ SNew(STextBlock).Text(LOCTEXT("ReflectorAlbedo", "Albedo (0-1)")) ] ]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.f).MaxValue(1.f)
							.MinSliderValue(0.f).MaxSliderValue(1.f)
							.Delta(0.01f)
							.Value(this, &SLuxMeterPanel::GetSelectedReflectorAlbedo)
							.OnValueChanged(this, &SLuxMeterPanel::OnSelectedReflectorAlbedoChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnSelectedReflectorAlbedoCommitted)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8) [ SNew(SSeparator) ]
				]
			]

			// ---- Orientation section (visible when fixture or meter is selected) ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.Visibility(this, &SLuxMeterPanel::GetOrientationVisibility)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("HeadingSmall"))
						.Text(LOCTEXT("OrientationHeader", "Orientation"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[
							SNew(SBox).WidthOverride(60.f)
							[ SNew(STextBlock).Text(LOCTEXT("PanLabel", "Pan")) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(-360.f)
							.MaxValue(360.f)
							.MinSliderValue(-180.f)
							.MaxSliderValue(180.f)
							.Delta(1.f)
							.Value(this, &SLuxMeterPanel::GetSelectedPan)
							.OnValueChanged(this, &SLuxMeterPanel::OnPanChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnPanCommitted)
							.ToolTipText(LOCTEXT("PanTooltip", "Yaw rotation in degrees (around world Z)."))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
						[
							SNew(SBox).WidthOverride(60.f)
							[ SNew(STextBlock).Text(LOCTEXT("TiltLabel", "Tilt")) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SSpinBox<float>)
							.MinValue(-89.9f)
							.MaxValue(89.9f)
							.Delta(1.f)
							.Value(this, &SLuxMeterPanel::GetSelectedTilt)
							.OnValueChanged(this, &SLuxMeterPanel::OnTiltChanged)
							.OnValueCommitted(this, &SLuxMeterPanel::OnTiltCommitted)
							.ToolTipText(LOCTEXT("TiltTooltip", "Pitch rotation in degrees (above/below horizontal)."))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("AutoAim", "Auto-aim"))
						.ToolTipText(LOCTEXT("AutoAimTooltip", "Selected fixtures snap to face the lightmeter. A selected meter snaps to face the nearest fixture (smallest angle from its current forward)."))
						.OnClicked(this, &SLuxMeterPanel::HandleAutoAim)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8) [ SNew(SSeparator) ]
				]
			]

			// ---- Scene section ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("SceneHeader", "Scene"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
			[
				SNew(STextBlock).Text(this, &SLuxMeterPanel::GetLightCountText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).Text(this, &SLuxMeterPanel::GetMeterCountText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).Text(this, &SLuxMeterPanel::GetReflectorCountText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("SpawnMeter", "Spawn lightmeter"))
					.OnClicked(this, &SLuxMeterPanel::HandleSpawnMeter)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("SpawnReflector", "Spawn reflector"))
					.ToolTipText(LOCTEXT("SpawnReflectorTooltip",
						"Spawn a flat Lambertian reflector (matte plate). Its +X face is the active reflective side."))
					.OnClicked(this, &SLuxMeterPanel::HandleSpawnReflector)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshScene", "Refresh"))
					.OnClicked(this, &SLuxMeterPanel::HandleRefreshScene)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4) [ SNew(SSeparator) ]

			// ---- Exposure section ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("ExposureHeader", "Exposure"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SBox).WidthOverride(110.f)
					[ SNew(STextBlock).Text(LOCTEXT("FPSLabel", "FPS")) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SComboBox<TSharedPtr<FExposurePreset>>)
					.OptionsSource(&FPSPresets)
					.InitiallySelectedItem(SelectedFPSPreset)
					.OnGenerateWidget_Lambda(
						[](TSharedPtr<FExposurePreset> Item)
						{
							return SNew(STextBlock).Text(Item.IsValid() ? Item->Label : FText::GetEmpty());
						})
					.OnSelectionChanged_Lambda(
						[this](TSharedPtr<FExposurePreset> NewSel, ESelectInfo::Type)
						{
							if (NewSel.IsValid())
							{
								FPS = NewSel->Value;
								SelectedFPSPreset = NewSel;
							}
						})
					[
						SNew(STextBlock)
						.Text_Lambda(
							[this]()
							{
								return SelectedFPSPreset.IsValid() ? SelectedFPSPreset->Label : FText::GetEmpty();
							})
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SBox).WidthOverride(110.f)
					[ SNew(STextBlock).Text(LOCTEXT("ISOLabel", "ISO")) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SComboBox<TSharedPtr<FExposurePreset>>)
					.OptionsSource(&ISOPresets)
					.InitiallySelectedItem(SelectedISOPreset)
					.OnGenerateWidget_Lambda(
						[](TSharedPtr<FExposurePreset> Item)
						{
							return SNew(STextBlock).Text(Item.IsValid() ? Item->Label : FText::GetEmpty());
						})
					.OnSelectionChanged_Lambda(
						[this](TSharedPtr<FExposurePreset> NewSel, ESelectInfo::Type)
						{
							if (NewSel.IsValid())
							{
								ISO = NewSel->Value;
								SelectedISOPreset = NewSel;
							}
						})
					[
						SNew(STextBlock)
						.Text_Lambda(
							[this]()
							{
								return SelectedISOPreset.IsValid() ? SelectedISOPreset->Label : FText::GetEmpty();
							})
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SBox).WidthOverride(110.f)
					[ SNew(STextBlock).Text(LOCTEXT("ShutterLabel", "Shutter angle")) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SComboBox<TSharedPtr<FExposurePreset>>)
					.OptionsSource(&ShutterPresets)
					.InitiallySelectedItem(SelectedShutterPreset)
					.OnGenerateWidget_Lambda(
						[](TSharedPtr<FExposurePreset> Item)
						{
							return SNew(STextBlock).Text(Item.IsValid() ? Item->Label : FText::GetEmpty());
						})
					.OnSelectionChanged_Lambda(
						[this](TSharedPtr<FExposurePreset> NewSel, ESelectInfo::Type)
						{
							if (NewSel.IsValid())
							{
								ShutterAngle = NewSel->Value;
								SelectedShutterPreset = NewSel;
							}
						})
					[
						SNew(STextBlock)
						.Text_Lambda(
							[this]()
							{
								return SelectedShutterPreset.IsValid() ? SelectedShutterPreset->Label : FText::GetEmpty();
							})
					]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4) [ SNew(SSeparator) ]

			// ---- Calculator section ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("CalcHeader", "Calculator"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(this, &SLuxMeterPanel::GetEndpointText)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Ping", "Ping"))
					.OnClicked(this, &SLuxMeterPanel::HandlePing)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(StatusText, STextBlock)
				.Text(LOCTEXT("StatusUnknown", "Status: unknown"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(8, 6))
				.Text(LOCTEXT("Measure", "Measure"))
				.OnClicked(this, &SLuxMeterPanel::HandleMeasure)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SAssignNew(ResultText, STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingMedium"))
				.Text(LOCTEXT("ResultPlaceholder", "Result: —"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SAssignNew(PerLightText, STextBlock)
				.AutoWrapText(true)
			]
		]
	];
}

SLuxMeterPanel::~SLuxMeterPanel()
{
	if (SelectionChangedHandle.IsValid())
	{
		USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
	}
	if (ObjectSelectedHandle.IsValid())
	{
		USelection::SelectObjectEvent.Remove(ObjectSelectedHandle);
	}
}

// ----------------------------------------------------------------------------
// IES library
// ----------------------------------------------------------------------------

void SLuxMeterPanel::RefreshIESList()
{
	AllIESItems.Reset();

	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UTextureLightProfile::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		AllIESItems.Add(MakeShared<FAssetData>(Asset));
	}

	AllIESItems.Sort([](const TSharedPtr<FAssetData>& A, const TSharedPtr<FAssetData>& B)
	{
		return A->AssetName.LexicalLess(B->AssetName);
	});

	ApplySearchFilter();
}

void SLuxMeterPanel::ApplySearchFilter()
{
	IESItems.Reset();

	TArray<FString> Tokens;
	IESSearchText.ToLower().ParseIntoArrayWS(Tokens);

	if (Tokens.Num() == 0)
	{
		IESItems = AllIESItems;
	}
	else
	{
		for (const TSharedPtr<FAssetData>& Item : AllIESItems)
		{
			const FString Name = Item->AssetName.ToString().ToLower();
			bool bAllMatch = true;
			for (const FString& Token : Tokens)
			{
				if (!Name.Contains(Token))
				{
					bAllMatch = false;
					break;
				}
			}
			if (bAllMatch)
			{
				IESItems.Add(Item);
			}
		}
	}

	if (IESListView.IsValid())
	{
		IESListView->RequestListRefresh();
	}
}

void SLuxMeterPanel::OnIESSearchChanged(const FText& NewText)
{
	IESSearchText = NewText.ToString();
	ApplySearchFilter();
}

TSharedRef<ITableRow> SLuxMeterPanel::OnGenerateIESRow(TSharedPtr<FAssetData> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString PackagePath = Item->PackagePath.ToString();
	const FText Label = FText::FromString(FString::Printf(TEXT("%s   (%s)"), *Item->AssetName.ToString(), *PackagePath));

	return SNew(STableRow<TSharedPtr<FAssetData>>, OwnerTable)
		.Padding(FMargin(4, 2))
		[
			SNew(STextBlock).Text(Label)
		];
}

// ----------------------------------------------------------------------------
// Buttons
// ----------------------------------------------------------------------------

FReply SLuxMeterPanel::HandleImportIES()
{
	const FString DestinationPath = TEXT("/Game/IES");

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	const TArray<UObject*> Imported = AssetToolsModule.Get().ImportAssetsWithDialog(DestinationPath);

	int32 IESCount = 0;
	for (UObject* Obj : Imported)
	{
		if (Cast<UTextureLightProfile>(Obj)) ++IESCount;
	}

	if (Imported.Num() == 0)
	{
		return FReply::Handled();
	}

	if (IESCount == 0)
	{
		NotifyError(LOCTEXT("ImportNoIES", "No IES profiles found in the selection. Pick .ies files."));
		return FReply::Handled();
	}

	RefreshIESList();

	NotifySuccess(FText::Format(
		LOCTEXT("ImportedIES", "Imported {0} IES profile(s) into {1}"),
		IESCount,
		FText::FromString(DestinationPath)));

	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleSpawnLight()
{
	if (!SelectedAsset.IsValid())
	{
		NotifyError(LOCTEXT("NoIESSelected", "Select an IES profile first."));
		return FReply::Handled();
	}

	UTextureLightProfile* Profile = Cast<UTextureLightProfile>(SelectedAsset->GetAsset());
	if (!Profile)
	{
		NotifyError(LOCTEXT("LoadFailed", "Failed to load the selected IES asset."));
		return FReply::Handled();
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	FVector SpawnLocation(0, 0, 200);
	FRotator SpawnRotation(-45.f, 0.f, 0.f);
	if (GCurrentLevelEditingViewportClient)
	{
		const FVector CamLoc = GCurrentLevelEditingViewportClient->GetViewLocation();
		const FRotator CamRot = GCurrentLevelEditingViewportClient->GetViewRotation();
		SpawnLocation = CamLoc + CamRot.Vector() * 200.f;
		SpawnRotation = CamRot;
	}

	const FScopedTransaction Transaction(LOCTEXT("SpawnLuxLight", "Spawn IES Light"));

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional;
	ALuxIESLightActor* Actor = World->SpawnActor<ALuxIESLightActor>(ALuxIESLightActor::StaticClass(), SpawnLocation, SpawnRotation, Params);
	if (!Actor)
	{
		NotifyError(LOCTEXT("SpawnFailed", "Failed to spawn light actor."));
		return FReply::Handled();
	}

	Actor->Modify();
	Actor->IESProfile = Profile;
	Actor->SourceIESPath = ResolveSourceIESPath(*SelectedAsset);
	Actor->ApplyIESToLight();
	Actor->SetActorLabel(FString::Printf(TEXT("LuxIES_%s"), *SelectedAsset->AssetName.ToString()));

	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Actor, true, true);
	}

	NotifySuccess(FText::Format(LOCTEXT("SpawnedLight", "Spawned {0}"), FText::FromString(Actor->GetActorLabel())));
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleApplyIESToSelected()
{
	if (!SelectedAsset.IsValid())
	{
		NotifyError(LOCTEXT("ApplyNoIES", "Select an IES profile from the library first."));
		return FReply::Handled();
	}
	if (SelectedFixtures.Num() == 0)
	{
		NotifyError(LOCTEXT("ApplyNoFixture", "Select one or more LuxIES fixtures in the level first."));
		return FReply::Handled();
	}

	UTextureLightProfile* Profile = Cast<UTextureLightProfile>(SelectedAsset->GetAsset());
	if (!Profile)
	{
		NotifyError(LOCTEXT("LoadFailed", "Failed to load the selected IES asset."));
		return FReply::Handled();
	}
	const FString SourcePath = ResolveSourceIESPath(*SelectedAsset);

	const FScopedTransaction Transaction(LOCTEXT("ApplyIES", "Apply IES to selected fixtures"));

	int32 Applied = 0;
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			Fixture->Modify();
			Fixture->IESProfile = Profile;
			Fixture->SourceIESPath = SourcePath;
			Fixture->ApplyIESToLight();
			Fixture->SetActorLabel(FString::Printf(TEXT("LuxIES_%s"), *SelectedAsset->AssetName.ToString()));
			++Applied;
		}
	}

	if (Applied == 0)
	{
		NotifyError(LOCTEXT("ApplyNoneApplied", "Selected fixtures are no longer valid."));
	}
	else
	{
		NotifySuccess(FText::Format(
			LOCTEXT("AppliedIES", "Applied {0} to {1} fixture(s)"),
			FText::FromName(SelectedAsset->AssetName),
			Applied));
	}
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleAutoAim()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	auto AimAt = [](AActor* Actor, const FVector& Target)
	{
		const FVector Dir = (Target - Actor->GetActorLocation()).GetSafeNormal();
		if (Dir.IsNearlyZero()) return;
		FRotator Aimed = Dir.Rotation();
		Aimed.Roll = Actor->GetActorRotation().Roll; // preserve manual roll
		Actor->Modify();
		Actor->SetActorRotation(Aimed);
	};

	const bool bHasFixtureSelection = SelectedFixtures.Num() > 0;
	const bool bHasMeterSelection   = SelectedMeters.Num() > 0;
	if (!bHasFixtureSelection && !bHasMeterSelection)
	{
		NotifyError(LOCTEXT("AutoAimNoSelection", "Select a LuxIES light or lightmeter first."));
		return FReply::Handled();
	}

	const FScopedTransaction Transaction(LOCTEXT("AutoAim", "Auto-aim"));
	int32 Aimed = 0;

	// Fixtures aim at the (first) lightmeter in the level.
	if (bHasFixtureSelection)
	{
		ALuxLightmeterActor* Meter = nullptr;
		for (TActorIterator<ALuxLightmeterActor> It(World); It; ++It) { Meter = *It; break; }
		if (!Meter)
		{
			NotifyError(LOCTEXT("AutoAimNoMeter", "Place a lightmeter before aiming fixtures."));
			return FReply::Handled();
		}
		const FVector MeterLoc = Meter->GetActorLocation();
		for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)
		{
			if (ALuxIESLightActor* F = W.Get())
			{
				AimAt(F, MeterLoc);
				++Aimed;
			}
		}
	}

	// Meters aim at the fixture whose direction is closest to the meter's current forward.
	if (bHasMeterSelection)
	{
		TArray<ALuxIESLightActor*> AllFixtures;
		for (TActorIterator<ALuxIESLightActor> It(World); It; ++It) AllFixtures.Add(*It);
		if (AllFixtures.Num() == 0)
		{
			NotifyError(LOCTEXT("AutoAimNoFixture", "No LuxIES fixtures in the level to aim at."));
			return FReply::Handled();
		}

		for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)
		{
			ALuxLightmeterActor* M = W.Get();
			if (!M) continue;

			const FVector MLoc = M->GetActorLocation();
			const FVector MFwd = M->GetActorForwardVector();
			ALuxIESLightActor* Best = nullptr;
			float BestCos = -2.f; // larger cos = smaller angle
			for (ALuxIESLightActor* F : AllFixtures)
			{
				const FVector ToF = (F->GetActorLocation() - MLoc).GetSafeNormal();
				if (ToF.IsNearlyZero()) continue;
				const float C = FVector::DotProduct(MFwd, ToF);
				if (C > BestCos) { BestCos = C; Best = F; }
			}
			if (Best)
			{
				AimAt(M, Best->GetActorLocation());
				++Aimed;
			}
		}
	}

	if (Aimed > 0)
	{
		NotifySuccess(FText::Format(LOCTEXT("AutoAimedN", "Auto-aimed {0} actor(s)"), Aimed));
	}
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleSpawnReflector()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	FVector SpawnLocation(0, 0, 100);
	FRotator SpawnRotation(0.f, 0.f, 0.f);
	if (GCurrentLevelEditingViewportClient)
	{
		const FVector CamLoc = GCurrentLevelEditingViewportClient->GetViewLocation();
		const FRotator CamRot = GCurrentLevelEditingViewportClient->GetViewRotation();
		SpawnLocation = CamLoc + CamRot.Vector() * 200.f;
		// Face the reflector back toward the camera so its +X (active face) is
		// visible immediately after spawn.
		SpawnRotation = (-CamRot.Vector()).Rotation();
	}

	const FScopedTransaction Transaction(LOCTEXT("SpawnReflector", "Spawn Reflector"));

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional;
	ALuxReflectorActor* Actor = World->SpawnActor<ALuxReflectorActor>(
		ALuxReflectorActor::StaticClass(), SpawnLocation, SpawnRotation, Params);
	if (!Actor)
	{
		return FReply::Handled();
	}
	Actor->Modify();
	Actor->SetActorLabel(TEXT("LuxReflector"));

	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Actor, true, true);
	}

	NotifySuccess(LOCTEXT("SpawnedReflector", "Spawned reflector"));
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleSpawnMeter()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	FVector SpawnLocation(0, 0, 100);
	FRotator SpawnRotation(90.f, 0.f, 0.f);
	if (GCurrentLevelEditingViewportClient)
	{
		const FVector CamLoc = GCurrentLevelEditingViewportClient->GetViewLocation();
		const FRotator CamRot = GCurrentLevelEditingViewportClient->GetViewRotation();
		SpawnLocation = CamLoc + CamRot.Vector() * 200.f;
	}

	const FScopedTransaction Transaction(LOCTEXT("SpawnLightmeter", "Spawn Lightmeter"));

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transactional;
	ALuxLightmeterActor* Actor = World->SpawnActor<ALuxLightmeterActor>(ALuxLightmeterActor::StaticClass(), SpawnLocation, SpawnRotation, Params);
	if (!Actor)
	{
		return FReply::Handled();
	}
	Actor->Modify();
	Actor->SetActorLabel(TEXT("LuxLightmeter"));

	if (GEditor)
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Actor, true, true);
	}

	NotifySuccess(LOCTEXT("SpawnedMeter", "Spawned lightmeter"));
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleRefreshScene()
{
	return FReply::Handled();
}

FReply SLuxMeterPanel::HandlePing()
{
	if (StatusText.IsValid()) StatusText->SetText(LOCTEXT("StatusPinging", "Status: pinging..."));

	TWeakPtr<SLuxMeterPanel> WeakSelf = SharedThis(this);
	FLuxCalcClient::Health(
		[WeakSelf](bool bOk, const FString& Version)
		{
			TSharedPtr<SLuxMeterPanel> Self = WeakSelf.Pin();
			if (!Self.IsValid() || !Self->StatusText.IsValid()) return;

			if (bOk)
			{
				Self->StatusText->SetText(FText::Format(
					LOCTEXT("StatusOk", "Status: OK ({0})"),
					FText::FromString(Version.IsEmpty() ? TEXT("no version") : Version)));
			}
			else
			{
				Self->StatusText->SetText(LOCTEXT("StatusFail", "Status: server unreachable"));
			}
		});

	return FReply::Handled();
}

FReply SLuxMeterPanel::HandleMeasure()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	ALuxLightmeterActor* Meter = nullptr;
	for (TActorIterator<ALuxLightmeterActor> It(World); It; ++It)
	{
		Meter = *It;
		break;
	}
	if (!Meter)
	{
		NotifyError(LOCTEXT("NoMeter", "Place a lightmeter first."));
		return FReply::Handled();
	}

	const FVector MeterLocation = Meter->GetActorLocation();
	const FVector MeterForward  = Meter->GetActorForwardVector();

	FLuxMeasureRequest Request;
	Request.Units = TEXT("cm");
	Request.Meter.Location = MeterLocation;
	Request.Meter.Forward  = MeterForward;

	int32 LightsInScene = 0;

	// Reflectors are added unconditionally (no hemisphere-pruning) — a reflector
	// behind the meter still affects the lux total via bounce. Collected first
	// so the light loop below can decide whether the meter-front prune applies.
	for (TActorIterator<ALuxReflectorActor> It(World); It; ++It)
	{
		ALuxReflectorActor* R = *It;
		FLuxReflectorPayload P;
		P.Location    = R->GetActorLocation();
		P.Forward     = R->GetActorForwardVector();
		P.Up          = R->GetActorUpVector();
		P.WidthCm     = R->Width;
		P.HeightCm    = R->Height;
		P.Albedo      = R->Albedo;
		P.DisplayName = R->GetActorLabel();
		Request.Reflectors.Add(P);
	}

	// With a reflector in the scene, lights behind the meter still matter
	// because they illuminate the reflector. Skip the hemisphere prune in
	// that case and let the Python side zero out the direct contribution.
	const bool bHasReflector = Request.Reflectors.Num() > 0;

	for (TActorIterator<ALuxIESLightActor> It(World); It; ++It)
	{
		ALuxIESLightActor* Light = *It;
		++LightsInScene;

		if (!bHasReflector)
		{
			// Restrict to the meter's front hemisphere (180° in front of the sensor).
			// Lights with cos(angle) <= 0 between meter forward and meter->light
			// direction contribute nothing to the direct path, so we drop them.
			const FVector ToLight = Light->GetActorLocation() - MeterLocation;
			if (FVector::DotProduct(MeterForward, ToLight) <= 0.f)
			{
				continue;
			}
		}

		FLuxLightPayload Payload;
		Payload.Location = Light->GetActorLocation();
		Payload.Forward = Light->GetActorForwardVector();
		Payload.Up = Light->GetActorUpVector();
		Payload.IESFilePath = Light->SourceIESPath;
		Payload.IntensityLumens = 0.f; // calc always uses IES candela as-is; ViewportIntensityLumens is previs-only
		Payload.DisplayName = Light->GetActorLabel();
		Request.Lights.Add(Payload);
	}

	if (LightsInScene == 0)
	{
		NotifyError(LOCTEXT("NoLights", "Place at least one Lux IES light first."));
		return FReply::Handled();
	}
	// Only abort on "all lights behind the meter" when there is no reflector to
	// route them via bounce. With a reflector, the request keeps every light.
	if (Request.Lights.Num() == 0 && !bHasReflector)
	{
		NotifyError(LOCTEXT("NoLightsInFront", "All lights are behind the meter — point the meter toward your fixtures."));
		return FReply::Handled();
	}

	if (ResultText.IsValid()) ResultText->SetText(LOCTEXT("Measuring", "Result: measuring..."));
	if (PerLightText.IsValid()) PerLightText->SetText(FText::GetEmpty());

	TWeakPtr<SLuxMeterPanel> WeakSelf = SharedThis(this);
	TWeakObjectPtr<ALuxLightmeterActor> WeakMeter = Meter;
	TArray<TWeakObjectPtr<ALuxReflectorActor>> WeakReflectors;
	for (TActorIterator<ALuxReflectorActor> It(World); It; ++It) WeakReflectors.Add(*It);
	TArray<FString> LightNames;
	for (const FLuxLightPayload& L : Request.Lights) LightNames.Add(L.DisplayName);

	FLuxCalcClient::Measure(Request,
		[WeakSelf, WeakMeter, WeakReflectors, LightNames](bool bOk, const FLuxMeasureResponse& Response)
		{
			TSharedPtr<SLuxMeterPanel> Self = WeakSelf.Pin();
			if (!Self.IsValid()) return;

			if (!bOk)
			{
				if (Self->ResultText.IsValid())
				{
					Self->ResultText->SetText(FText::Format(
						LOCTEXT("ResultError", "Error: {0}"),
						FText::FromString(Response.Error.IsEmpty() ? TEXT("unknown") : Response.Error)));
				}
				return;
			}

			if (Self->ResultText.IsValid())
			{
				FString Line = FString::Printf(TEXT("Result: %.1f lx"), Response.IlluminanceLux);
				const float N = Self->GetRequiredFNumber(Response.IlluminanceLux);
				if (N > 0.f)
				{
					Line += FString::Printf(TEXT("  |  f/%.1f"), N);
				}
				Self->ResultText->SetText(FText::FromString(Line));
			}

			if (Self->PerLightText.IsValid())
			{
				FString Lines;
				if (Response.Debug.Num() > 0)
				{
					for (const FLuxLightDebug& D : Response.Debug)
					{
						Lines += FString::Printf(
							TEXT("  %s | dist=%.2fm | gamma=%.1f deg | cd=%.1f | cos=%.2f -> %.2f lx\n"),
							*D.DisplayName,
							D.DistanceMeters,
							D.GammaDeg,
							D.Candela,
							D.CosineToMeter,
							D.ContributionLux);
					}
				}
				else if (Response.PerLightLux.Num() == LightNames.Num())
				{
					for (int32 i = 0; i < LightNames.Num(); ++i)
					{
						Lines += FString::Printf(TEXT("  %s: %.2f lx\n"), *LightNames[i], Response.PerLightLux[i]);
					}
				}
				Self->PerLightText->SetText(FText::FromString(Lines));
			}

			if (ALuxLightmeterActor* Meter = WeakMeter.Get())
			{
				const float N = Self->GetRequiredFNumber(Response.IlluminanceLux);
				Meter->SetReadout(Response.IlluminanceLux, N);
			}

			// Stash the reflected lux on each reflector for Details-panel inspection.
			// Find the "(reflector)" row in Debug; if present, write its value.
			float ReflectedLux = 0.f;
			for (const FLuxLightDebug& D : Response.Debug)
			{
				if (D.DisplayName == TEXT("(reflector)"))
				{
					ReflectedLux = D.ContributionLux;
					break;
				}
			}
			for (const TWeakObjectPtr<ALuxReflectorActor>& W : WeakReflectors)
			{
				if (ALuxReflectorActor* R = W.Get())
				{
					R->LastReflectedLux = ReflectedLux;
				}
			}
		});

	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// Selected fixture binding
// ----------------------------------------------------------------------------

void SLuxMeterPanel::OnEditorSelectionChanged(UObject* /*Selection*/)
{
	RefreshSelectedFixtures();
}

void SLuxMeterPanel::RefreshSelectedFixtures()
{
	SelectedFixtures.Reset();
	SelectedMeters.Reset();
	SelectedReflectors.Reset();
	if (!GEditor) return;

	if (USelection* Selection = GEditor->GetSelectedActors())
	{
		TArray<UObject*> Selected;
		Selection->GetSelectedObjects(Selected);
		for (UObject* Obj : Selected)
		{
			if (ALuxIESLightActor* Light = Cast<ALuxIESLightActor>(Obj))
			{
				SelectedFixtures.Add(Light);
			}
			else if (ALuxLightmeterActor* Meter = Cast<ALuxLightmeterActor>(Obj))
			{
				SelectedMeters.Add(Meter);
			}
			else if (ALuxReflectorActor* Reflector = Cast<ALuxReflectorActor>(Obj))
			{
				SelectedReflectors.Add(Reflector);
			}
		}
	}
}

EVisibility SLuxMeterPanel::GetSelectedFixtureVisibility() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (Weak.IsValid()) return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

FText SLuxMeterPanel::GetSelectedFixtureNameText() const
{
	int32 ValidCount = 0;
	ALuxIESLightActor* First = nullptr;
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			if (!First) First = Fixture;
			++ValidCount;
		}
	}
	if (ValidCount == 0) return FText::GetEmpty();
	if (ValidCount == 1) return FText::Format(LOCTEXT("FixtureName", "Name: {0}"), FText::FromString(First->GetActorLabel()));
	return FText::Format(LOCTEXT("FixtureMultiName", "Name: ({0} fixtures selected)"), ValidCount);
}

FText SLuxMeterPanel::GetSelectedFixtureIESText() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			const FString Name = Fixture->IESProfile ? Fixture->IESProfile->GetName() : TEXT("(none)");
			return FText::Format(LOCTEXT("FixtureIES", "IES:  {0}"), FText::FromString(Name));
		}
	}
	return FText::GetEmpty();
}

float SLuxMeterPanel::GetSelectedIntensity() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			return Fixture->ViewportIntensityLumens;
		}
	}
	return 0.f;
}

void SLuxMeterPanel::OnSelectedIntensityChanged(float NewValue)
{
	// Live preview during slider drag — no transaction (would create undo spam).
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			Fixture->ViewportIntensityLumens = NewValue;
			Fixture->ApplyIESToLight();
		}
	}
}

void SLuxMeterPanel::OnSelectedIntensityCommitted(float NewValue, ETextCommit::Type /*CommitType*/)
{
	const FScopedTransaction Transaction(LOCTEXT("ChangeViewportIntensity", "Change viewport intensity"));
	for (const TWeakObjectPtr<ALuxIESLightActor>& Weak : SelectedFixtures)
	{
		if (ALuxIESLightActor* Fixture = Weak.Get())
		{
			Fixture->Modify();
			Fixture->ViewportIntensityLumens = NewValue;
			Fixture->ApplyIESToLight();
		}
	}
}

// ----------------------------------------------------------------------------
// Selected-reflector binding
// ----------------------------------------------------------------------------

EVisibility SLuxMeterPanel::GetSelectedReflectorVisibility() const
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (W.IsValid()) return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

FText SLuxMeterPanel::GetSelectedReflectorNameText() const
{
	int32 ValidCount = 0;
	ALuxReflectorActor* First = nullptr;
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			if (!First) First = R;
			++ValidCount;
		}
	}
	if (ValidCount == 0) return FText::GetEmpty();
	if (ValidCount == 1) return FText::Format(LOCTEXT("ReflectorName", "Name: {0}"), FText::FromString(First->GetActorLabel()));
	return FText::Format(LOCTEXT("ReflectorMultiName", "Name: ({0} reflectors selected)"), ValidCount);
}

float SLuxMeterPanel::GetSelectedReflectorWidth() const
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get()) return R->Width;
	}
	return 0.f;
}

float SLuxMeterPanel::GetSelectedReflectorHeight() const
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get()) return R->Height;
	}
	return 0.f;
}

float SLuxMeterPanel::GetSelectedReflectorAlbedo() const
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get()) return R->Albedo;
	}
	return 0.f;
}

void SLuxMeterPanel::OnSelectedReflectorWidthChanged(float NewValue)
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Width = NewValue;
			R->ApplyShape();
		}
	}
}

void SLuxMeterPanel::OnSelectedReflectorWidthCommitted(float NewValue, ETextCommit::Type /*CommitType*/)
{
	const FScopedTransaction Transaction(LOCTEXT("ChangeReflectorWidth", "Change reflector width"));
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Modify();
			R->Width = NewValue;
			R->ApplyShape();
		}
	}
}

void SLuxMeterPanel::OnSelectedReflectorHeightChanged(float NewValue)
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Height = NewValue;
			R->ApplyShape();
		}
	}
}

void SLuxMeterPanel::OnSelectedReflectorHeightCommitted(float NewValue, ETextCommit::Type /*CommitType*/)
{
	const FScopedTransaction Transaction(LOCTEXT("ChangeReflectorHeight", "Change reflector height"));
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Modify();
			R->Height = NewValue;
			R->ApplyShape();
		}
	}
}

void SLuxMeterPanel::OnSelectedReflectorAlbedoChanged(float NewValue)
{
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Albedo = NewValue;
		}
	}
}

void SLuxMeterPanel::OnSelectedReflectorAlbedoCommitted(float NewValue, ETextCommit::Type /*CommitType*/)
{
	const FScopedTransaction Transaction(LOCTEXT("ChangeReflectorAlbedo", "Change reflector albedo"));
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (ALuxReflectorActor* R = W.Get())
		{
			R->Modify();
			R->Albedo = NewValue;
		}
	}
}

FText SLuxMeterPanel::GetReflectorCountText() const
{
	int32 Count = 0;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ALuxReflectorActor> It(World); It; ++It) ++Count;
	}
	return FText::Format(LOCTEXT("ReflectorCount", "Reflectors:   {0}"), Count);
}

// ----------------------------------------------------------------------------
// Pan / Tilt
// ----------------------------------------------------------------------------

EVisibility SLuxMeterPanel::GetOrientationVisibility() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)
	{
		if (W.IsValid()) return EVisibility::Visible;
	}
	for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)
	{
		if (W.IsValid()) return EVisibility::Visible;
	}
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (W.IsValid()) return EVisibility::Visible;
	}
	return EVisibility::Collapsed;
}

float SLuxMeterPanel::GetSelectedPan() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Yaw;
	}
	for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Yaw;
	}
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Yaw;
	}
	return 0.f;
}

float SLuxMeterPanel::GetSelectedTilt() const
{
	for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Pitch;
	}
	for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Pitch;
	}
	for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors)
	{
		if (AActor* A = W.Get()) return A->GetActorRotation().Pitch;
	}
	return 0.f;
}

void SLuxMeterPanel::ApplyPanTiltToSelected(float NewYawDeg, float NewPitchDeg, bool bTransactional)
{
	auto ApplyOne = [&](AActor* A)
	{
		if (!A) return;
		if (bTransactional) A->Modify();
		FRotator R = A->GetActorRotation();
		R.Yaw = NewYawDeg;
		R.Pitch = NewPitchDeg;
		// Roll is intentionally left as-is so a manually-rolled actor isn't reset.
		A->SetActorRotation(R);
	};

	if (bTransactional)
	{
		const FScopedTransaction Transaction(LOCTEXT("ChangePanTilt", "Change pan/tilt"));
		for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)   ApplyOne(W.Get());
		for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)   ApplyOne(W.Get());
		for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors) ApplyOne(W.Get());
	}
	else
	{
		for (const TWeakObjectPtr<ALuxIESLightActor>& W : SelectedFixtures)   ApplyOne(W.Get());
		for (const TWeakObjectPtr<ALuxLightmeterActor>& W : SelectedMeters)   ApplyOne(W.Get());
		for (const TWeakObjectPtr<ALuxReflectorActor>& W : SelectedReflectors) ApplyOne(W.Get());
	}
}

void SLuxMeterPanel::OnPanChanged(float NewYawDeg)
{
	ApplyPanTiltToSelected(NewYawDeg, GetSelectedTilt(), /*bTransactional=*/false);
}

void SLuxMeterPanel::OnPanCommitted(float NewYawDeg, ETextCommit::Type /*CommitType*/)
{
	ApplyPanTiltToSelected(NewYawDeg, GetSelectedTilt(), /*bTransactional=*/true);
}

void SLuxMeterPanel::OnTiltChanged(float NewPitchDeg)
{
	ApplyPanTiltToSelected(GetSelectedPan(), NewPitchDeg, /*bTransactional=*/false);
}

void SLuxMeterPanel::OnTiltCommitted(float NewPitchDeg, ETextCommit::Type /*CommitType*/)
{
	ApplyPanTiltToSelected(GetSelectedPan(), NewPitchDeg, /*bTransactional=*/true);
}

// ----------------------------------------------------------------------------
// Misc
// ----------------------------------------------------------------------------

FText SLuxMeterPanel::GetLightCountText() const
{
	int32 Count = 0;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ALuxIESLightActor> It(World); It; ++It) ++Count;
	}
	return FText::Format(LOCTEXT("LightCount", "Lights placed: {0}"), Count);
}

FText SLuxMeterPanel::GetMeterCountText() const
{
	int32 Count = 0;
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<ALuxLightmeterActor> It(World); It; ++It) ++Count;
	}
	return FText::Format(LOCTEXT("MeterCount", "Lightmeters: {0}"), Count);
}

FText SLuxMeterPanel::GetEndpointText() const
{
	const ULuxMeterCalcSettings* Settings = GetDefault<ULuxMeterCalcSettings>();
	return FText::Format(LOCTEXT("Endpoint", "Endpoint: {0}"), FText::FromString(Settings->GetBaseUrl()));
}

float SLuxMeterPanel::GetShutterTimeSeconds() const
{
	const float SafeFPS = FMath::Max(FPS, 0.001f);
	const float SafeAngle = FMath::Clamp(ShutterAngle, 1.f, 360.f);
	return (SafeAngle / 360.f) / SafeFPS;
}

float SLuxMeterPanel::GetRequiredFNumber(float Lux) const
{
	if (Lux <= 0.f) return 0.f;
	const float t = GetShutterTimeSeconds();
	const float SafeISO = FMath::Max(ISO, 1.f);
	return FMath::Sqrt(Lux * t * SafeISO / IncidentCalibrationConstant);
}

void SLuxMeterPanel::NotifyError(const FText& Message)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 4.f;
	FSlateNotificationManager::Get().AddNotification(Info)
		->SetCompletionState(SNotificationItem::CS_Fail);
}

void SLuxMeterPanel::NotifySuccess(const FText& Message)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 2.5f;
	FSlateNotificationManager::Get().AddNotification(Info)
		->SetCompletionState(SNotificationItem::CS_Success);
}

FString SLuxMeterPanel::ResolveSourceIESPath(const FAssetData& Asset)
{
	UTextureLightProfile* Profile = Cast<UTextureLightProfile>(Asset.GetAsset());
	if (!Profile)
	{
		return FString();
	}

#if WITH_EDITORONLY_DATA
	if (Profile->AssetImportData)
	{
		TArray<FString> Filenames;
		Profile->AssetImportData->ExtractFilenames(Filenames);
		if (Filenames.Num() > 0)
		{
			return FPaths::ConvertRelativePathToFull(Filenames[0]);
		}
	}
#endif

	return FString();
}

#undef LOCTEXT_NAMESPACE
