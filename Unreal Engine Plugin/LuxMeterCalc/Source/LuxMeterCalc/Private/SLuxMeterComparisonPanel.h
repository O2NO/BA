#pragma once

#include "CoreMinimal.h"
#include "ComparisonTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class STableViewBase;
class STextBlock;
class SEditableTextBox;

enum class EIESFitMethod : uint8
{
	ForwardFitNNLS    = 0,   // current — projected coordinate-descent NNLS on the bilinear forward model
	KernelSigma10Cos  = 1,   // Phase 5b improved: Gaussian kernel σ=10°, samples weighted by cosθ_meter
	KernelSigma5      = 2,   // Phase 5b original: Gaussian kernel σ=5°, no extra weighting
	ReverseFit        = 3,   // Phase 5a: per-sample inversion → bin → diffuse fill → smooth
};

struct FFitMethodOption
{
	FText Label;
	EIESFitMethod Method = EIESFitMethod::ForwardFitNNLS;
};

class SLuxMeterComparisonPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLuxMeterComparisonPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// File picking
	FReply HandleBrowseTrack();
	FReply HandleBrowseMeter();
	bool BrowseForCsv(FString& OutAbsolutePath, const FString& DialogTitle) const;

	// Pipeline buttons
	FReply HandleImport();
	FReply HandleClear();
	FReply HandleCalculateAll();
	FReply HandleImproveSync();
	FReply HandleCreateIES();

	// List view
	TSharedRef<ITableRow> OnGenerateMeterRow(TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnMeterRowSelectionChanged(TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo);

	struct FResolveResult
	{
		bool bFound = false;           // false when no track sample exists for the target frame
		FTransform Xform;              // UE-space transform (used for scrubbing the meter actor)
		int32 TrackFrame = -1;         // target track frame (for display) regardless of bFound
		// Raw Optitrack-frame pose, possibly interpolated. Position is in whatever
		// unit the source CSV uses (mm for default Motive); quaternion is native.
		// Used for CSV output so the user gets the unmodified tracker readings.
		FVector Pos_OT = FVector::ZeroVector;
		FQuat   Rot_OT = FQuat::Identity;
	};

	// Resolves a meter sample's track transform in UE space. Looks up by actual
	// track frame number, so dropped/missing frames in track.csv are detected and
	// surfaced as bFound=false (no interpolation across gaps).
	FResolveResult ResolveTrackForSample(int32 MeterIndex) const;

	// Same as above but with an explicit sync-frame override (used by the
	// Improve-sync search without mutating panel state).
	FResolveResult ResolveTrackForSampleAt(int32 MeterIndex, int32 SyncFrameOverride) const;

	// Binary search TrackSamples (sorted by FrameIndex) for an exact frame match.
	const FTrackSample* FindTrackSampleByFrame(int32 Frame) const;

	// Helpers
	void Notify(bool bSuccess, const FText& Message);
	FString DefaultOutputPath() const;
	FString TimestampSuffix() const;

	// State
	FString TrackPath;
	FString MeterPath;
	float TrackFps = 120.f;
	float MeterFps = 60.f;
	int32 TrackSyncFrame = 0;
	FVector WorldOriginOffset_cm = FVector::ZeroVector;

	TArray<FTrackSample> TrackSamples;
	TArray<FMeterSample> MeterSamples;

	// SListView wants TSharedPtr<T>; the int just holds the meter index.
	TArray<TSharedPtr<int32>> MeterIndices;
	TSharedPtr<SListView<TSharedPtr<int32>>> MeterListView;

	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SEditableTextBox> TrackPathBox;
	TSharedPtr<SEditableTextBox> MeterPathBox;

	// Fit-method dropdown next to Create IES file.
	TArray<TSharedPtr<FFitMethodOption>> FitMethodOptions;
	TSharedPtr<FFitMethodOption> SelectedFitMethod;

	bool bCalculating = false;
};
