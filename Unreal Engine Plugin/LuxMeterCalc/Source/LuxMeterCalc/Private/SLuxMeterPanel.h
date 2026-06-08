#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ALuxIESLightActor;
class ALuxLightmeterActor;
class ALuxReflectorActor;
class AActor;
class ITableRow;
class SBox;
class STableViewBase;
class STextBlock;
class UObject;

namespace ESelectInfo { enum Type : int; }

struct FExposurePreset
{
	FText Label;
	float Value = 0.f;
};

class SLuxMeterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLuxMeterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SLuxMeterPanel();

private:
	// IES library
	void RefreshIESList();
	void ApplySearchFilter();
	void OnIESSearchChanged(const FText& NewText);
	TSharedRef<ITableRow> OnGenerateIESRow(TSharedPtr<FAssetData> Item, const TSharedRef<STableViewBase>& OwnerTable);

	// Buttons
	FReply HandleSpawnLight();
	FReply HandleSpawnMeter();
	FReply HandleSpawnReflector();
	FReply HandleRefreshScene();
	FReply HandlePing();
	FReply HandleMeasure();
	FReply HandleImportIES();
	FReply HandleApplyIESToSelected();
	FReply HandleAutoAim();

	// Display getters
	FText GetLightCountText() const;
	FText GetMeterCountText() const;
	FText GetReflectorCountText() const;
	FText GetEndpointText() const;
	EVisibility GetSelectedFixtureVisibility() const;
	FText GetSelectedFixtureNameText() const;
	FText GetSelectedFixtureIESText() const;

	// Selected-reflector binding
	EVisibility GetSelectedReflectorVisibility() const;
	FText GetSelectedReflectorNameText() const;
	float GetSelectedReflectorWidth() const;
	float GetSelectedReflectorHeight() const;
	float GetSelectedReflectorAlbedo() const;
	void OnSelectedReflectorWidthChanged(float NewValue);
	void OnSelectedReflectorWidthCommitted(float NewValue, ETextCommit::Type CommitType);
	void OnSelectedReflectorHeightChanged(float NewValue);
	void OnSelectedReflectorHeightCommitted(float NewValue, ETextCommit::Type CommitType);
	void OnSelectedReflectorAlbedoChanged(float NewValue);
	void OnSelectedReflectorAlbedoCommitted(float NewValue, ETextCommit::Type CommitType);

	// Selected-fixture intensity binding
	float GetSelectedIntensity() const;
	void OnSelectedIntensityChanged(float NewValue);
	void OnSelectedIntensityCommitted(float NewValue, ETextCommit::Type CommitType);

	// Pan/Tilt binding (works on any selected fixture or meter)
	EVisibility GetOrientationVisibility() const;
	float GetSelectedPan() const;
	float GetSelectedTilt() const;
	void OnPanChanged(float NewYawDeg);
	void OnPanCommitted(float NewYawDeg, ETextCommit::Type CommitType);
	void OnTiltChanged(float NewPitchDeg);
	void OnTiltCommitted(float NewPitchDeg, ETextCommit::Type CommitType);
	void ApplyPanTiltToSelected(float NewYawDeg, float NewPitchDeg, bool bTransactional);

	// Editor selection tracking
	void OnEditorSelectionChanged(UObject* Selection);
	void RefreshSelectedFixtures();

	// Exposure helpers
	float GetShutterTimeSeconds() const;
	float GetRequiredFNumber(float Lux) const;
	TSharedRef<SWidget> BuildExposureRow(
		const FText& Label,
		const TArray<TSharedPtr<FExposurePreset>>& Presets,
		TSharedPtr<FExposurePreset>& OutInitialSelection,
		float* OutTargetValue);

	void NotifyError(const FText& Message);
	void NotifySuccess(const FText& Message);

	static FString ResolveSourceIESPath(const FAssetData& Asset);

	// IES library state
	TArray<TSharedPtr<FAssetData>> IESItems;       // filtered (drives the list view)
	TArray<TSharedPtr<FAssetData>> AllIESItems;    // unfiltered master
	FString IESSearchText;
	TSharedPtr<SListView<TSharedPtr<FAssetData>>> IESListView;
	TSharedPtr<FAssetData> SelectedAsset;

	// Selected fixture / meter / reflector tracking
	TArray<TWeakObjectPtr<ALuxIESLightActor>> SelectedFixtures;
	TArray<TWeakObjectPtr<ALuxLightmeterActor>> SelectedMeters;
	TArray<TWeakObjectPtr<ALuxReflectorActor>> SelectedReflectors;
	FDelegateHandle SelectionChangedHandle;
	FDelegateHandle ObjectSelectedHandle;

	// Exposure state (panel-owned)
	float FPS;
	float ISO;
	float ShutterAngle;

	TArray<TSharedPtr<FExposurePreset>> FPSPresets;
	TArray<TSharedPtr<FExposurePreset>> ISOPresets;
	TArray<TSharedPtr<FExposurePreset>> ShutterPresets;
	TSharedPtr<FExposurePreset> SelectedFPSPreset;
	TSharedPtr<FExposurePreset> SelectedISOPreset;
	TSharedPtr<FExposurePreset> SelectedShutterPreset;

	// UI
	TSharedPtr<STextBlock> ResultText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> PerLightText;
};
