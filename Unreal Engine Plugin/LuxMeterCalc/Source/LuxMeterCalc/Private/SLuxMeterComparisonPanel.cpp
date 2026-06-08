#include "SLuxMeterComparisonPanel.h"

#include "LuxIESLightActor.h"
#include "LuxLightmeterActor.h"
#include "LuxReflectorActor.h"
#include "LuxCalcClient.h"
#include "LuxMeasurementTypes.h"
#include "MotiveCsvParser.h"
#include "OptitrackToUE.h"

#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SLuxMeterComparisonPanel"

namespace
{
	ALuxLightmeterActor* FindFirstMeter(UWorld* World)
	{
		if (!World) return nullptr;
		for (TActorIterator<ALuxLightmeterActor> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	int32 MeterSamples_PreReserveHint(int32 NumRows, int32 NumFixtures)
	{
		return (256 + NumFixtures * 96) * NumRows;
	}

	// ====================================================================
	// IES fitting — shared types and four method implementations.
	// All methods output a Cells[NV][NH_DISTINCT] grid (76 γ × 4 C).
	// The 5th column (360°) is added later when writing the LM-63 file.
	// ====================================================================

	constexpr int32 IESFIT_NV = 76;           // γ = 0..75° at 1° steps
	constexpr int32 IESFIT_NH_DISTINCT = 4;   // C ∈ {0°, 90°, 180°, 270°}

	// All the per-sample data any of the four methods could want.
	struct FFitSample
	{
		// Measurement
		float Lux;
		float CosThetaMeter;
		float DistanceM;
		float Alpha;              // cosθ_meter / d² — forward multiplier
		float CandelaEstimate;    // E · d² / cosθ_meter — per-sample inversion
		// IES local-frame direction (unit) for kernel methods
		float Dx, Dy, Dz;
		float GammaDeg;           // [0, 75]
		float CDeg;               // [0, 360)
		// Bilinear neighbour indices/weights (only used by forward-fit)
		int32 V0, V1;
		int32 H0, H1;
		float Wv, Wh;
	};

	// ---- Helpers shared by the bin/diffuse/smooth method ----

	void FillEmptyCellsByDiffusion(TArray<TArray<float>>& Grid, TArray<TArray<bool>>& HasData)
	{
		const int32 NV = Grid.Num();
		if (NV == 0) return;
		const int32 NH = Grid[0].Num();
		if (NH == 0) return;

		constexpr int32 MaxPasses = 64;
		for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
		{
			TArray<TArray<float>> SrcGrid = Grid;
			TArray<TArray<bool>>  SrcHas  = HasData;
			bool bAnyChanged = false;
			bool bAnyStillEmpty = false;

			for (int32 v = 0; v < NV; ++v)
			{
				for (int32 h = 0; h < NH; ++h)
				{
					if (SrcHas[v][h]) continue;

					double Sum = 0.0;
					int32 Count = 0;
					const int32 Vm = FMath::Max(0, v - 1);
					const int32 Vp = FMath::Min(NV - 1, v + 1);
					const int32 Hm = (h - 1 + NH) % NH;
					const int32 Hp = (h + 1) % NH;
					if (Vm != v && SrcHas[Vm][h]) { Sum += SrcGrid[Vm][h]; ++Count; }
					if (Vp != v && SrcHas[Vp][h]) { Sum += SrcGrid[Vp][h]; ++Count; }
					if (SrcHas[v][Hm])            { Sum += SrcGrid[v][Hm]; ++Count; }
					if (SrcHas[v][Hp])            { Sum += SrcGrid[v][Hp]; ++Count; }

					if (Count > 0)
					{
						Grid[v][h] = static_cast<float>(Sum / Count);
						HasData[v][h] = true;
						bAnyChanged = true;
					}
					else
					{
						bAnyStillEmpty = true;
					}
				}
			}
			if (!bAnyChanged || !bAnyStillEmpty) break;
		}

		// Final fallback for any unreachable cells: global mean of filled cells.
		double Sum = 0.0;
		int32 Count = 0;
		for (int32 v = 0; v < NV; ++v)
			for (int32 h = 0; h < NH; ++h)
				if (HasData[v][h]) { Sum += Grid[v][h]; ++Count; }
		const float Fallback = Count > 0 ? static_cast<float>(Sum / Count) : 0.f;
		for (int32 v = 0; v < NV; ++v)
			for (int32 h = 0; h < NH; ++h)
				if (!HasData[v][h]) { Grid[v][h] = Fallback; HasData[v][h] = true; }
	}

	void SmoothGrid(TArray<TArray<float>>& Grid)
	{
		const int32 NV = Grid.Num();
		if (NV == 0) return;
		const int32 NH = Grid[0].Num();
		if (NH == 0) return;

		// γ pass (clamped at endpoints).
		TArray<TArray<float>> Tmp = Grid;
		for (int32 v = 0; v < NV; ++v)
		{
			const int32 Vm = FMath::Max(0, v - 1);
			const int32 Vp = FMath::Min(NV - 1, v + 1);
			for (int32 h = 0; h < NH; ++h)
				Tmp[v][h] = 0.25f * Grid[Vm][h] + 0.5f * Grid[v][h] + 0.25f * Grid[Vp][h];
		}
		// C pass (wrapped).
		for (int32 v = 0; v < NV; ++v)
		{
			for (int32 h = 0; h < NH; ++h)
			{
				const int32 Hm = (h - 1 + NH) % NH;
				const int32 Hp = (h + 1) % NH;
				Grid[v][h] = 0.25f * Tmp[v][Hm] + 0.5f * Tmp[v][h] + 0.25f * Tmp[v][Hp];
			}
		}
	}

	// ---- Method 1: Reverse fit (Phase 5a) — bin / diffuse / smooth ----
	// Per-sample candela inversion → bin to nearest grid cell → average → fill
	// empties by Laplacian diffusion → separable [0.25, 0.5, 0.25] smoothing.
	void FitReverse(const TArray<FFitSample>& Samples, TArray<TArray<float>>& OutCells)
	{
		const int32 NV = IESFIT_NV;
		const int32 NH = IESFIT_NH_DISTINCT;

		TArray<TArray<double>> Sum;
		TArray<TArray<int32>>  Count;
		Sum.SetNum(NV); Count.SetNum(NV);
		for (int32 v = 0; v < NV; ++v)
		{
			Sum[v].SetNumZeroed(NH);
			Count[v].SetNumZeroed(NH);
		}

		for (const FFitSample& S : Samples)
		{
			const int32 V = FMath::Clamp(FMath::RoundToInt(S.GammaDeg), 0, NV - 1);
			const int32 H = (FMath::RoundToInt(S.CDeg / 90.f) % NH + NH) % NH;
			Sum[V][H] += S.CandelaEstimate;
			Count[V][H] += 1;
		}

		OutCells.SetNum(NV);
		TArray<TArray<bool>> HasData;
		HasData.SetNum(NV);
		for (int32 v = 0; v < NV; ++v)
		{
			OutCells[v].SetNumZeroed(NH);
			HasData[v].Init(false, NH);
			for (int32 h = 0; h < NH; ++h)
			{
				if (Count[v][h] > 0)
				{
					OutCells[v][h] = static_cast<float>(Sum[v][h] / Count[v][h]);
					HasData[v][h] = true;
				}
			}
		}

		FillEmptyCellsByDiffusion(OutCells, HasData);
		SmoothGrid(OutCells);
	}

	// ---- Method 2/3: Kernel regression (Phase 5b) ----
	// Per query (γ_q, C_q): weighted average of all samples' per-sample candela
	// estimates, where weight is a Gaussian on great-circle angle (and optionally
	// also cos θ_meter, which down-weights noisy samples).
	void FitKernel(const TArray<FFitSample>& Samples,
		float SigmaDeg, bool bCosThetaWeighted, TArray<TArray<float>>& OutCells)
	{
		const int32 NV = IESFIT_NV;
		const int32 NH = IESFIT_NH_DISTINCT;
		const float SigmaRad = FMath::DegreesToRadians(SigmaDeg);
		const float InvTwoSigmaSq = 1.f / (2.f * SigmaRad * SigmaRad);

		// Fallback if a query has total weight ≈ 0.
		double FallbackWeightSum = 0.0;
		double FallbackCandelaSum = 0.0;
		for (const FFitSample& S : Samples)
		{
			const float W = bCosThetaWeighted ? S.CosThetaMeter : 1.f;
			FallbackWeightSum  += W;
			FallbackCandelaSum += static_cast<double>(W) * S.CandelaEstimate;
		}
		const float FallbackMean = FallbackWeightSum > 1e-12
			? static_cast<float>(FallbackCandelaSum / FallbackWeightSum) : 0.f;

		OutCells.SetNum(NV);
		for (int32 v = 0; v < NV; ++v) OutCells[v].SetNumZeroed(NH);

		const float H_DEG[4] = { 0.f, 90.f, 180.f, 270.f };

		for (int32 v = 0; v < NV; ++v)
		{
			const float GammaRad = FMath::DegreesToRadians(static_cast<float>(v));
			const float SinG = FMath::Sin(GammaRad);
			const float CosG = FMath::Cos(GammaRad);
			for (int32 h = 0; h < NH; ++h)
			{
				const float CRad = FMath::DegreesToRadians(H_DEG[h]);
				const float Qx = SinG * FMath::Cos(CRad);
				const float Qy = SinG * FMath::Sin(CRad);
				const float Qz = CosG;

				double WSum = 0.0;
				double IWSum = 0.0;
				for (const FFitSample& S : Samples)
				{
					const float Cos = FMath::Clamp(Qx * S.Dx + Qy * S.Dy + Qz * S.Dz, -1.f, 1.f);
					const float Angle = FMath::Acos(Cos);
					const float Kernel = FMath::Exp(-Angle * Angle * InvTwoSigmaSq);
					const float W = bCosThetaWeighted ? Kernel * S.CosThetaMeter : Kernel;
					WSum  += W;
					IWSum += static_cast<double>(W) * S.CandelaEstimate;
				}
				OutCells[v][h] = WSum > 1e-12 ? static_cast<float>(IWSum / WSum) : FallbackMean;
			}
		}
	}

	// ---- Method 4: Forward-fit (Phase 5c) — projected coordinate-descent NNLS ----
	// Solves argmin_{c ≥ 0} ‖A c − lux‖² where A_ij = α_i · w_ij and w_ij are the
	// bilinear interpolation weights of sample i onto cell j. Returns the iteration
	// count so the caller can put it in the IES header.
	int32 FitForwardNNLS(const TArray<FFitSample>& Samples, TArray<TArray<float>>& OutCells)
	{
		const int32 NV = IESFIT_NV;
		const int32 NH = IESFIT_NH_DISTINCT;

		struct FToucher { int32 SampleIdx; float A; };
		TArray<TArray<TArray<FToucher>>> CellTouchers;
		CellTouchers.SetNum(NV);
		for (int32 v = 0; v < NV; ++v) CellTouchers[v].SetNum(NH);

		auto AddToucher = [&](int32 V, int32 H, int32 SampleIdx, float W, float Alpha)
		{
			if (W <= 1e-12f) return;
			CellTouchers[V][H].Add({ SampleIdx, W * Alpha });
		};

		for (int32 i = 0; i < Samples.Num(); ++i)
		{
			const FFitSample& S = Samples[i];
			const float W00 = (1.f - S.Wv) * (1.f - S.Wh);
			const float W01 = (1.f - S.Wv) * S.Wh;
			const float W10 = S.Wv * (1.f - S.Wh);
			const float W11 = S.Wv * S.Wh;
			AddToucher(S.V0, S.H0, i, W00, S.Alpha);
			AddToucher(S.V0, S.H1, i, W01, S.Alpha);
			AddToucher(S.V1, S.H0, i, W10, S.Alpha);
			AddToucher(S.V1, S.H1, i, W11, S.Alpha);
		}

		TArray<float> Residuals;
		Residuals.SetNumUninitialized(Samples.Num());
		for (int32 i = 0; i < Samples.Num(); ++i) Residuals[i] = -Samples[i].Lux;

		OutCells.SetNum(NV);
		for (int32 v = 0; v < NV; ++v) OutCells[v].SetNumZeroed(NH);

		constexpr int32 MaxIter = 200;
		float MaxCellSeen = 1.f;
		int32 IterDone = 0;
		for (int32 iter = 0; iter < MaxIter; ++iter)
		{
			IterDone = iter + 1;
			float MaxAbsDelta = 0.f;
			for (int32 v = 0; v < NV; ++v)
			{
				for (int32 h = 0; h < NH; ++h)
				{
					const TArray<FToucher>& Touchers = CellTouchers[v][h];
					if (Touchers.Num() == 0) continue;

					double Numer = 0.0;
					double Denom = 0.0;
					for (const FToucher& T : Touchers)
					{
						Numer += static_cast<double>(T.A) * Residuals[T.SampleIdx];
						Denom += static_cast<double>(T.A) * T.A;
					}
					if (Denom < 1e-12) continue;

					const float OldC = OutCells[v][h];
					float NewC = OldC - static_cast<float>(Numer / Denom);
					if (NewC < 0.f) NewC = 0.f;
					const float Delta = NewC - OldC;
					if (FMath::Abs(Delta) < 1e-12f) continue;

					OutCells[v][h] = NewC;
					if (NewC > MaxCellSeen) MaxCellSeen = NewC;
					if (FMath::Abs(Delta) > MaxAbsDelta) MaxAbsDelta = FMath::Abs(Delta);

					for (const FToucher& T : Touchers)
						Residuals[T.SampleIdx] += T.A * Delta;
				}
			}
			if (MaxAbsDelta < 1e-3f * MaxCellSeen) break;
		}
		return IterDone;
	}

	const TCHAR* FitMethodTestLine(EIESFitMethod M)
	{
		switch (M)
		{
		case EIESFitMethod::ReverseFit:        return TEXT("LuxMeterCalc reverse fit (bin/diffuse/smooth)");
		case EIESFitMethod::KernelSigma5:      return TEXT("LuxMeterCalc kernel-regression fit (sigma=5 deg)");
		case EIESFitMethod::KernelSigma10Cos:  return TEXT("LuxMeterCalc kernel-regression fit (sigma=10 deg, cosTheta-weighted)");
		case EIESFitMethod::ForwardFitNNLS:    return TEXT("LuxMeterCalc forward-fit (projected coordinate descent, NNLS)");
		}
		return TEXT("LuxMeterCalc fit (unknown)");
	}
}

void SLuxMeterComparisonPanel::Construct(const FArguments& InArgs)
{
	// Dropdown options — order is what the user sees, default = first entry.
	FitMethodOptions.Reset();
	FitMethodOptions.Add(MakeShared<FFitMethodOption>(FFitMethodOption{
		LOCTEXT("FitForwardNNLS", "Forward-fit (NNLS, default)"), EIESFitMethod::ForwardFitNNLS }));
	FitMethodOptions.Add(MakeShared<FFitMethodOption>(FFitMethodOption{
		LOCTEXT("FitKernel10",   "Kernel regression (sigma=10 deg, cosTheta-weighted)"), EIESFitMethod::KernelSigma10Cos }));
	FitMethodOptions.Add(MakeShared<FFitMethodOption>(FFitMethodOption{
		LOCTEXT("FitKernel5",    "Kernel regression (sigma=5 deg)"), EIESFitMethod::KernelSigma5 }));
	FitMethodOptions.Add(MakeShared<FFitMethodOption>(FFitMethodOption{
		LOCTEXT("FitReverse",    "Reverse fit (bin / diffuse / smooth)"), EIESFitMethod::ReverseFit }));
	SelectedFitMethod = FitMethodOptions[0];

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.f)
		[
			SNew(SVerticalBox)

			// ---- Tracking CSV row ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("TrackingHeader", "Tracking CSV (Motive default export)"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SAssignNew(TrackPathBox, SEditableTextBox)
					.HintText(LOCTEXT("TrackPathHint", "path/to/track.csv"))
					.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
					{
						TrackPath = Text.ToString();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "Browse..."))
					.OnClicked(this, &SLuxMeterComparisonPanel::HandleBrowseTrack)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(SBox).WidthOverride(110.f) [ SNew(STextBlock).Text(LOCTEXT("TrackFpsLabel", "Framerate (fps)")) ] ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SSpinBox<float>)
					.MinValue(1.f).MaxValue(1000.f)
					.MinSliderValue(1.f).MaxSliderValue(240.f)
					.Delta(1.f)
					.Value_Lambda([this]() { return TrackFps; })
					.OnValueCommitted_Lambda([this](float V, ETextCommit::Type) { TrackFps = FMath::Max(1.f, V); })
				]
			]

			// ---- Lightmeter CSV row ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("MeterHeader", "Lightmeter CSV"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SAssignNew(MeterPathBox, SEditableTextBox)
					.HintText(LOCTEXT("MeterPathHint", "path/to/lightmeter.csv"))
					.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
					{
						MeterPath = Text.ToString();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "Browse..."))
					.OnClicked(this, &SLuxMeterComparisonPanel::HandleBrowseMeter)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(SBox).WidthOverride(110.f) [ SNew(STextBlock).Text(LOCTEXT("MeterFpsLabel", "Framerate (fps)")) ] ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SSpinBox<float>)
					.MinValue(1.f).MaxValue(1000.f)
					.MinSliderValue(1.f).MaxSliderValue(240.f)
					.Delta(1.f)
					.Value_Lambda([this]() { return MeterFps; })
					.OnValueCommitted_Lambda([this](float V, ETextCommit::Type) { MeterFps = FMath::Max(1.f, V); })
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4) [ SNew(SSeparator) ]

			// ---- Origin offset ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).Text(LOCTEXT("OriginOffsetLabel", "World origin offset (cm)"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true).Delta(1.f)
					.Value_Lambda([this]() -> TOptional<float> { return WorldOriginOffset_cm.X; })
					.OnValueCommitted_Lambda([this](float V, ETextCommit::Type) { WorldOriginOffset_cm.X = V; })
					.Label() [ SNew(STextBlock).Text(LOCTEXT("OffX", "X")) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true).Delta(1.f)
					.Value_Lambda([this]() -> TOptional<float> { return WorldOriginOffset_cm.Y; })
					.OnValueCommitted_Lambda([this](float V, ETextCommit::Type) { WorldOriginOffset_cm.Y = V; })
					.Label() [ SNew(STextBlock).Text(LOCTEXT("OffY", "Y")) ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true).Delta(1.f)
					.Value_Lambda([this]() -> TOptional<float> { return WorldOriginOffset_cm.Z; })
					.OnValueCommitted_Lambda([this](float V, ETextCommit::Type) { WorldOriginOffset_cm.Z = V; })
					.Label() [ SNew(STextBlock).Text(LOCTEXT("OffZ", "Z")) ]
				]
			]

			// ---- Track sync frame ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[ SNew(SBox).WidthOverride(160.f) [ SNew(STextBlock).Text(LOCTEXT("TrackSyncLabel", "Track sync frame")) ] ]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(0)
					.MaxValue(1000000)
					.MinSliderValue(0)
					.MaxSliderValue_Lambda([this]() -> int32 { return FMath::Max(1, TrackSamples.Num() - 1); })
					.Value_Lambda([this]() { return TrackSyncFrame; })
					.OnValueCommitted_Lambda([this](int32 V, ETextCommit::Type) { TrackSyncFrame = FMath::Max(0, V); })
					.ToolTipText(LOCTEXT("TrackSyncTooltip", "The track.csv row that lines up with lightmeter row 0."))
				]
			]

			// ---- Improve sync button ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("ImproveSync", "Improve sync"))
				.ToolTipText(LOCTEXT("ImproveSyncTooltip",
					"Tests TrackSyncFrame +/-60 frames around the current value, computes a 20%-trimmed mean absolute error per candidate, and snaps to the lowest. One big batch call to the calculator."))
				.OnClicked(this, &SLuxMeterComparisonPanel::HandleImproveSync)
			]

			// ---- Import / Clear buttons ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("Import", "Import / Reload"))
					.OnClicked(this, &SLuxMeterComparisonPanel::HandleImport)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Clear", "Clear"))
					.OnClicked(this, &SLuxMeterComparisonPanel::HandleClear)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4) [ SNew(SSeparator) ]

			// ---- Sample list ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingSmall"))
				.Text(LOCTEXT("SamplesHeader", "Lightmeter samples (click to scrub the meter)"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(2.f)
				[
					SNew(SBox).HeightOverride(220.f)
					[
						SAssignNew(MeterListView, SListView<TSharedPtr<int32>>)
						.ListItemsSource(&MeterIndices)
						.OnGenerateRow(this, &SLuxMeterComparisonPanel::OnGenerateMeterRow)
						.OnSelectionChanged(this, &SLuxMeterComparisonPanel::OnMeterRowSelectionChanged)
						.SelectionMode(ESelectionMode::Single)
					]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(8, 6))
				.Text(LOCTEXT("CalculateAll", "Calculate all and save"))
				.OnClicked(this, &SLuxMeterComparisonPanel::HandleCalculateAll)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(8, 6))
					.Text(LOCTEXT("CreateIES", "Create IES file"))
					.ToolTipText(LOCTEXT("CreateIESTooltip",
						"Fit a candela distribution from the recording using the selected method. Requires exactly one Lux IES light in the level. Writes fitted_<timestamp>.ies next to lightmeter.csv."))
					.OnClicked(this, &SLuxMeterComparisonPanel::HandleCreateIES)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SComboBox<TSharedPtr<FFitMethodOption>>)
					.OptionsSource(&FitMethodOptions)
					.InitiallySelectedItem(SelectedFitMethod)
					.OnGenerateWidget_Lambda([](TSharedPtr<FFitMethodOption> Item)
					{
						return SNew(STextBlock).Text(Item.IsValid() ? Item->Label : FText::GetEmpty());
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FFitMethodOption> NewSel, ESelectInfo::Type)
					{
						if (NewSel.IsValid()) SelectedFitMethod = NewSel;
					})
					.ToolTipText(LOCTEXT("FitMethodTooltip",
						"Fit method:\n"
						"  Forward-fit (NNLS): solves argmin ||A c - lux||^2 s.t. c >= 0 by projected coordinate descent over a 76x4 cell grid; A holds bilinear weights x alpha_i. Most accurate.\n"
						"  Kernel sigma=10 cos-weighted: Nadaraya-Watson on a unit sphere with a Gaussian kernel on great-circle angle, samples weighted by cos theta_meter.\n"
						"  Kernel sigma=5: same but tighter kernel, no cos-theta weighting.\n"
						"  Reverse fit: per-sample candela estimate -> bin -> Laplacian diffusion fill -> 3-tap smooth."))
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return SelectedFitMethod.IsValid() ? SelectedFitMethod->Label : FText::GetEmpty();
						})
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(StatusText, STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("StatusEmpty", "No data loaded."))
			]
		]
	];
}

// ----------------------------------------------------------------------------
// File pickers
// ----------------------------------------------------------------------------

bool SLuxMeterComparisonPanel::BrowseForCsv(FString& OutAbsolutePath, const FString& DialogTitle) const
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return false;

	const void* ParentWindowHandle = nullptr;
	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (ActiveWindow.IsValid() && ActiveWindow->GetNativeWindow().IsValid())
		{
			ParentWindowHandle = ActiveWindow->GetNativeWindow()->GetOSWindowHandle();
		}
	}

	TArray<FString> Picked;
	// Prefer the plugin's TrackingData folder if the user has already populated it,
	// then fall back to the project Content folder.
	FString DefaultDir;
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LuxMeterCalc")))
	{
		DefaultDir = FPaths::ConvertRelativePathToFull(
			Plugin->GetBaseDir() / TEXT("Content") / TEXT("TrackingData"));
		if (!FPaths::DirectoryExists(DefaultDir))
		{
			DefaultDir.Reset();
		}
	}
	if (DefaultDir.IsEmpty())
	{
		DefaultDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("TrackingData"));
	}

	const bool bOk = Desktop->OpenFileDialog(
		ParentWindowHandle,
		DialogTitle,
		DefaultDir,
		TEXT(""),
		TEXT("CSV files (*.csv)|*.csv|All files (*.*)|*.*"),
		EFileDialogFlags::None,
		Picked);

	if (bOk && Picked.Num() > 0)
	{
		OutAbsolutePath = FPaths::ConvertRelativePathToFull(Picked[0]);
		return true;
	}
	return false;
}

FReply SLuxMeterComparisonPanel::HandleBrowseTrack()
{
	FString Picked;
	if (BrowseForCsv(Picked, LOCTEXT("BrowseTrackTitle", "Pick the Motive track.csv").ToString()))
	{
		TrackPath = Picked;
		if (TrackPathBox.IsValid()) TrackPathBox->SetText(FText::FromString(TrackPath));
	}
	return FReply::Handled();
}

FReply SLuxMeterComparisonPanel::HandleBrowseMeter()
{
	FString Picked;
	if (BrowseForCsv(Picked, LOCTEXT("BrowseMeterTitle", "Pick the lightmeter.csv").ToString()))
	{
		MeterPath = Picked;
		if (MeterPathBox.IsValid()) MeterPathBox->SetText(FText::FromString(MeterPath));
	}
	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// Import / Clear
// ----------------------------------------------------------------------------

FReply SLuxMeterComparisonPanel::HandleImport()
{
	TrackSamples.Reset();
	MeterSamples.Reset();
	MeterIndices.Reset();

	if (TrackPath.IsEmpty() || MeterPath.IsEmpty())
	{
		Notify(false, LOCTEXT("ImportPickFiles", "Pick both CSV files first."));
		if (StatusText.IsValid()) StatusText->SetText(LOCTEXT("StatusEmpty", "No data loaded."));
		if (MeterListView.IsValid()) MeterListView->RequestListRefresh();
		return FReply::Handled();
	}

	FString Error;
	if (!MotiveCsvParser::ParseTrackCsv(TrackPath, TrackFps, TrackSamples, Error))
	{
		Notify(false, FText::Format(LOCTEXT("ImportTrackFail", "Track CSV: {0}"), FText::FromString(Error)));
		TrackSamples.Reset();
		if (StatusText.IsValid()) StatusText->SetText(FText::FromString(FString::Printf(TEXT("Track CSV failed: %s"), *Error)));
		return FReply::Handled();
	}

	if (!MotiveCsvParser::ParseLightmeterCsv(MeterPath, MeterFps, MeterSamples, Error))
	{
		Notify(false, FText::Format(LOCTEXT("ImportMeterFail", "Lightmeter CSV: {0}"), FText::FromString(Error)));
		TrackSamples.Reset();
		MeterSamples.Reset();
		if (StatusText.IsValid()) StatusText->SetText(FText::FromString(FString::Printf(TEXT("Lightmeter CSV failed: %s"), *Error)));
		return FReply::Handled();
	}

	MeterIndices.Reserve(MeterSamples.Num());
	for (int32 i = 0; i < MeterSamples.Num(); ++i)
	{
		MeterIndices.Add(MakeShared<int32>(i));
	}
	if (MeterListView.IsValid()) MeterListView->RequestListRefresh();

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("ImportOk", "Loaded {0} track samples and {1} lightmeter samples."),
			TrackSamples.Num(), MeterSamples.Num()));
	}
	Notify(true, LOCTEXT("ImportSuccess", "CSVs imported."));
	return FReply::Handled();
}

FReply SLuxMeterComparisonPanel::HandleClear()
{
	TrackPath.Reset();
	MeterPath.Reset();
	if (TrackPathBox.IsValid()) TrackPathBox->SetText(FText::GetEmpty());
	if (MeterPathBox.IsValid()) MeterPathBox->SetText(FText::GetEmpty());
	TrackFps = 120.f;
	MeterFps = 60.f;
	TrackSyncFrame = 0;
	WorldOriginOffset_cm = FVector::ZeroVector;
	TrackSamples.Reset();
	MeterSamples.Reset();
	MeterIndices.Reset();
	if (MeterListView.IsValid()) MeterListView->RequestListRefresh();
	if (StatusText.IsValid()) StatusText->SetText(LOCTEXT("StatusEmpty", "No data loaded."));
	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// List view
// ----------------------------------------------------------------------------

TSharedRef<ITableRow> SLuxMeterComparisonPanel::OnGenerateMeterRow(TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const int32 Idx = (Item.IsValid() && MeterSamples.IsValidIndex(*Item)) ? *Item : INDEX_NONE;
	FText Label;
	FSlateColor RowColor = FSlateColor::UseForeground();

	if (Idx != INDEX_NONE)
	{
		const FMeterSample& S = MeterSamples[Idx];
		const FResolveResult R = ResolveTrackForSample(Idx);
		if (R.bFound)
		{
			Label = FText::FromString(FString::Printf(
				TEXT("frame %5d   track %5d   t=%7.3f s   %.2f lx"),
				S.FrameIndex, R.TrackFrame, S.TimeSeconds, S.Lux));
		}
		else
		{
			Label = FText::FromString(FString::Printf(
				TEXT("frame %5d   no data       t=%7.3f s   %.2f lx"),
				S.FrameIndex, S.TimeSeconds, S.Lux));
			RowColor = FSlateColor(FLinearColor(1.f, 0.35f, 0.35f));
		}
	}
	else
	{
		Label = LOCTEXT("RowEmpty", "(invalid)");
	}

	return SNew(STableRow<TSharedPtr<int32>>, OwnerTable)
		.Padding(FMargin(4, 2))
		[
			SNew(STextBlock)
			.Text(Label)
			.ColorAndOpacity(RowColor)
		];
}

void SLuxMeterComparisonPanel::OnMeterRowSelectionChanged(TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct) return;
	if (!Item.IsValid() || !MeterSamples.IsValidIndex(*Item)) return;
	if (TrackSamples.Num() == 0) return;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	ALuxLightmeterActor* Meter = FindFirstMeter(World);
	if (!Meter)
	{
		Notify(false, LOCTEXT("ScrubNoMeter", "Place a Lux Lightmeter actor first."));
		return;
	}

	const FResolveResult R = ResolveTrackForSample(*Item);
	if (!R.bFound)
	{
		Notify(false, FText::Format(
			LOCTEXT("ScrubNoTrackData", "No track data for meter frame {0} (target track frame {1})."),
			MeterSamples[*Item].FrameIndex, R.TrackFrame));
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ScrubToSample", "Scrub to sample"));
	Meter->Modify();
	Meter->SetActorTransform(R.Xform);
}

const FTrackSample* SLuxMeterComparisonPanel::FindTrackSampleByFrame(int32 Frame) const
{
	int32 Lo = 0;
	int32 Hi = TrackSamples.Num() - 1;
	while (Lo <= Hi)
	{
		const int32 Mid = (Lo + Hi) >> 1;
		const int32 MidFrame = TrackSamples[Mid].FrameIndex;
		if (MidFrame == Frame) return &TrackSamples[Mid];
		if (MidFrame < Frame)   Lo = Mid + 1;
		else                    Hi = Mid - 1;
	}
	return nullptr;
}

SLuxMeterComparisonPanel::FResolveResult SLuxMeterComparisonPanel::ResolveTrackForSample(int32 MeterIndex) const
{
	return ResolveTrackForSampleAt(MeterIndex, TrackSyncFrame);
}

SLuxMeterComparisonPanel::FResolveResult SLuxMeterComparisonPanel::ResolveTrackForSampleAt(int32 MeterIndex, int32 SyncFrameOverride) const
{
	FResolveResult Out;
	if (!MeterSamples.IsValidIndex(MeterIndex) || TrackSamples.Num() == 0)
	{
		return Out;
	}

	// Compute the desired track frame from the meter sample's *actual* frame number.
	const float Ratio = TrackFps / FMath::Max(MeterFps, 0.001f);
	const float MeterFrameF = static_cast<float>(MeterSamples[MeterIndex].FrameIndex);
	const float TrackFloatIdx = static_cast<float>(SyncFrameOverride) + MeterFrameF * Ratio;
	Out.TrackFrame = FMath::RoundToInt(TrackFloatIdx);

	const int32 F0 = FMath::FloorToInt(TrackFloatIdx);
	const int32 F1 = F0 + 1;
	const float Alpha = TrackFloatIdx - static_cast<float>(F0);

	// Look up by FrameIndex — gaps in track.csv (dropped frames) cannot be silently
	// interpolated across, so a missing sample is reported as bFound=false.
	const FTrackSample* A = FindTrackSampleByFrame(F0);
	if (!A) return Out;

	if (FMath::IsNearlyZero(Alpha, 1e-5f))
	{
		Out.Pos_OT = A->Pos_OT;
		Out.Rot_OT = A->Rot_OT;
		Out.Xform = FTransform(
			OptitrackToUE::RotationToUE(Out.Rot_OT),
			OptitrackToUE::PositionToUE(Out.Pos_OT) + WorldOriginOffset_cm);
		Out.bFound = true;
		return Out;
	}

	const FTrackSample* B = FindTrackSampleByFrame(F1);
	if (!B) return Out;  // can't interpolate without both bracketing samples

	Out.Pos_OT = FMath::Lerp(A->Pos_OT, B->Pos_OT, Alpha);
	Out.Rot_OT = FQuat::Slerp(A->Rot_OT, B->Rot_OT, Alpha);
	Out.Xform = FTransform(
		OptitrackToUE::RotationToUE(Out.Rot_OT),
		OptitrackToUE::PositionToUE(Out.Pos_OT) + WorldOriginOffset_cm);
	Out.bFound = true;
	return Out;
}

// ----------------------------------------------------------------------------
// Calculate all
// ----------------------------------------------------------------------------

FReply SLuxMeterComparisonPanel::HandleCalculateAll()
{
	if (bCalculating)
	{
		Notify(false, LOCTEXT("CalcInFlight", "A calculation is already running."));
		return FReply::Handled();
	}
	if (TrackSamples.Num() == 0 || MeterSamples.Num() == 0)
	{
		Notify(false, LOCTEXT("CalcNoData", "Import the CSVs first."));
		return FReply::Handled();
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	// Snapshot fixtures
	struct FFixtureSnapshot
	{
		FString Name;
		FString IesName;
		FVector Pos;
		FRotator Rot;
		FVector Forward;
		FVector Up;
		FString IesFilePath;
	};
	TArray<FFixtureSnapshot> Fixtures;
	for (TActorIterator<ALuxIESLightActor> It(World); It; ++It)
	{
		ALuxIESLightActor* L = *It;
		FFixtureSnapshot Snap;
		Snap.Name = L->GetActorLabel();
		Snap.IesName = L->IESProfile ? L->IESProfile->GetName() : TEXT("");
		Snap.Pos = L->GetActorLocation();
		Snap.Rot = L->GetActorRotation();
		Snap.Forward = L->GetActorForwardVector();
		Snap.Up = L->GetActorUpVector();
		Snap.IesFilePath = L->SourceIESPath;
		Fixtures.Add(Snap);
	}
	if (Fixtures.Num() == 0)
	{
		Notify(false, LOCTEXT("CalcNoFixtures", "Place at least one Lux IES light in the level first."));
		return FReply::Handled();
	}

	// Build the batch request
	FLuxMeasureBatchRequest Request;
	Request.Units = TEXT("cm");
	for (const FFixtureSnapshot& F : Fixtures)
	{
		FLuxLightPayload P;
		P.Location = F.Pos;
		P.Forward = F.Forward;
		P.Up = F.Up;
		P.IESFilePath = F.IesFilePath;
		P.IntensityLumens = 0.f;
		P.DisplayName = F.Name;
		Request.Lights.Add(P);
	}
	// Reflectors in the level participate in every meter-pose evaluation.
	for (TActorIterator<ALuxReflectorActor> It(World); It; ++It)
	{
		ALuxReflectorActor* R = *It;
		FLuxReflectorPayload RP;
		RP.Location    = R->GetActorLocation();
		RP.Forward     = R->GetActorForwardVector();
		RP.Up          = R->GetActorUpVector();
		RP.WidthCm     = R->Width;
		RP.HeightCm    = R->Height;
		RP.Albedo      = R->Albedo;
		RP.DisplayName = R->GetActorLabel();
		Request.Reflectors.Add(RP);
	}

	// Per-meter-sample resolved data. bFound rows go into the batch request;
	// !bFound rows are still written to the output CSV (with empty calc cells).
	struct FResolvedMeter
	{
		FTransform Xform;                            // UE-space (used internally)
		FVector Pos_OT  = FVector::ZeroVector;       // raw Optitrack position, source units
		FQuat   Rot_OT  = FQuat::Identity;           // raw Optitrack quaternion
		int32 TrackFrame = -1;
		bool bFound = false;
		int32 BatchIndex = INDEX_NONE;               // index inside Request.Meters, or INDEX_NONE if unresolved
	};
	TArray<FResolvedMeter> ResolvedMeters;
	ResolvedMeters.Reserve(MeterSamples.Num());
	for (int32 i = 0; i < MeterSamples.Num(); ++i)
	{
		const FResolveResult RR = ResolveTrackForSample(i);
		FResolvedMeter RM;
		RM.Xform      = RR.Xform;
		RM.Pos_OT     = RR.Pos_OT;
		RM.Rot_OT     = RR.Rot_OT;
		RM.TrackFrame = RR.TrackFrame;
		RM.bFound     = RR.bFound;

		if (RR.bFound)
		{
			FLuxMeterPayload MP;
			MP.Location = RR.Xform.GetLocation();
			MP.Forward = RR.Xform.TransformVectorNoScale(FVector::ForwardVector);
			RM.BatchIndex = Request.Meters.Num();
			Request.Meters.Add(MP);
		}

		ResolvedMeters.Add(RM);
	}

	if (Request.Meters.Num() == 0)
	{
		Notify(false, LOCTEXT("CalcNoTrackData",
			"None of the lightmeter samples have matching track data. Check Track sync frame and the track CSV."));
		return FReply::Handled();
	}

	bCalculating = true;
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("Calculating", "Calculating {0}/{1} samples (rest missing track) against {2} fixture(s)..."),
			Request.Meters.Num(), MeterSamples.Num(), Fixtures.Num()));
	}

	const FString OutPath = DefaultOutputPath();
	TWeakPtr<SLuxMeterComparisonPanel> WeakSelf = SharedThis(this);
	const TArray<FResolvedMeter> CapturedResolved = MoveTemp(ResolvedMeters);
	const TArray<FMeterSample> CapturedMeters = MeterSamples;
	const int32 ExpectedBatchSize = Request.Meters.Num();
	// Capture the first fixture's pose so the CSV writer can emit per-row
	// distance + γ (IES vertical angle) to that fixture. Single-fixture scenes
	// get unambiguous values; multi-fixture scenes get the values against the
	// first fixture (documented in column names).
	const FVector FirstFixPos    = Fixtures[0].Pos;
	const FVector FirstFixNadir  = Fixtures[0].Forward.GetSafeNormal();

	FLuxCalcClient::MeasureBatch(Request,
		[WeakSelf, OutPath, CapturedResolved, CapturedMeters, ExpectedBatchSize,
		 FirstFixPos, FirstFixNadir]
		(bool bOk, const FLuxMeasureBatchResponse& Response)
		{
			TSharedPtr<SLuxMeterComparisonPanel> Self = WeakSelf.Pin();
			if (!Self.IsValid()) return;
			Self->bCalculating = false;

			if (!bOk)
			{
				Self->Notify(false, FText::Format(
					LOCTEXT("CalcFailed", "Calculator error: {0}"),
					FText::FromString(Response.Error.IsEmpty() ? TEXT("unknown") : Response.Error)));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("CalcFailedStatus", "Calculation failed."));
				return;
			}
			if (Response.IlluminanceLux.Num() != ExpectedBatchSize)
			{
				Self->Notify(false, LOCTEXT("CalcSizeMismatch", "Calculator returned a different row count than expected."));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("CalcFailedStatus", "Calculation failed."));
				return;
			}

			// Build output CSV.
			FString CSV;
			CSV.Reserve(MeterSamples_PreReserveHint(CapturedMeters.Num(), 0));

			// Header. Position and rotation are raw Optitrack values, exactly as
			// written in the source CSV (so position is in mm for default Motive).
			// No axis swap, no unit scale. Rows with missing track data are skipped.
			// dist_to_fix_m and gamma_fix_deg are computed in UE world space against
			// the first fixture in the level (single-fixture scenes get unambiguous
			// values; multi-fixture scenes get them relative to fixture #0).
			CSV += TEXT("meter_frame,measured_lux,calculated_lux,delta_lux,error_pct,error_stops,");
			CSV += TEXT("dist_to_fix_m,gamma_fix_deg,");
			CSV += TEXT("meter_tx,meter_ty,meter_tz,meter_qx,meter_qy,meter_qz,meter_qw\n");

			// MAPE accumulator — skip rows where measured is near-zero (would blow
			// up the percentage and tell us nothing useful).
			constexpr float MinLuxForErr = 0.1f;
			double SumAbsErrPct = 0.0;
			int32 ValidErrCount = 0;
			int32 MissingTrackCount = 0;
			int32 WrittenRowCount = 0;

			for (int32 i = 0; i < CapturedMeters.Num(); ++i)
			{
				const FMeterSample& MS = CapturedMeters[i];
				const FResolvedMeter& RM = CapturedResolved[i];

				if (!RM.bFound)
				{
					++MissingTrackCount;
					continue;
				}

				const float Calc = Response.IlluminanceLux[RM.BatchIndex];
				const float Delta = Calc - MS.Lux;
				const float ErrPct = (Delta / FMath::Max(MS.Lux, 1e-3f)) * 100.f;
				// log2(calc/measured): +1 = predicted 1 stop brighter, -1 = 1 stop dimmer.
				// Clamp inputs at 1e-3 lx to keep the ratio finite for very dark samples.
				const float ErrStops = FMath::Loge(FMath::Max(Calc, 1e-3f) / FMath::Max(MS.Lux, 1e-3f))
				                       / FMath::Loge(2.f);

				if (MS.Lux >= MinLuxForErr)
				{
					SumAbsErrPct += FMath::Abs(ErrPct);
					++ValidErrCount;
				}

				// Distance + γ to the first fixture, in UE world space.
				const FVector DeltaCm = RM.Xform.GetLocation() - FirstFixPos;
				const float DistM = DeltaCm.Size() * 0.01f;
				float GammaDeg = 0.f;
				if (DistM > 1e-5f)
				{
					const FVector Dir = DeltaCm.GetSafeNormal();
					const float CosG = FMath::Clamp(static_cast<float>(FVector::DotProduct(Dir, FirstFixNadir)), -1.f, 1.f);
					GammaDeg = FMath::RadiansToDegrees(FMath::Acos(CosG));
				}

				CSV += FString::Printf(
					TEXT("%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
					MS.FrameIndex, MS.Lux, Calc, Delta, ErrPct, ErrStops,
					DistM, GammaDeg,
					RM.Pos_OT.X, RM.Pos_OT.Y, RM.Pos_OT.Z,
					RM.Rot_OT.X, RM.Rot_OT.Y, RM.Rot_OT.Z, RM.Rot_OT.W);
				++WrittenRowCount;
			}

			if (!FFileHelper::SaveStringToFile(CSV, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Self->Notify(false, FText::Format(LOCTEXT("WriteFailed", "Couldn't write {0}"), FText::FromString(OutPath)));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("WriteFailedStatus", "Save failed."));
				return;
			}

			const float AvgAbsErrPct = ValidErrCount > 0
				? static_cast<float>(SumAbsErrPct / ValidErrCount)
				: 0.f;

			FString DoneMsg;
			if (ValidErrCount > 0)
			{
				DoneMsg = FString::Printf(
					TEXT("Wrote %s (%d row(s)). Avg |error| = %.2f%% over %d/%d samples"),
					*FPaths::GetCleanFilename(OutPath),
					WrittenRowCount,
					AvgAbsErrPct,
					ValidErrCount,
					WrittenRowCount);
			}
			else
			{
				DoneMsg = FString::Printf(
					TEXT("Wrote %s (%d row(s)). (No samples with measured >= %.1f lx — average error not meaningful.)"),
					*FPaths::GetCleanFilename(OutPath),
					WrittenRowCount,
					MinLuxForErr);
			}
			if (MissingTrackCount > 0)
			{
				DoneMsg += FString::Printf(TEXT("  [%d row(s) skipped — no track data]"), MissingTrackCount);
			}

			Self->Notify(true, FText::FromString(DoneMsg));
			if (Self->StatusText.IsValid())
			{
				Self->StatusText->SetText(FText::FromString(FString::Printf(TEXT("Saved %s"), *OutPath)));
			}
		});

	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// Improve sync — local search around TrackSyncFrame
// ----------------------------------------------------------------------------

FReply SLuxMeterComparisonPanel::HandleImproveSync()
{
	if (bCalculating)
	{
		Notify(false, LOCTEXT("ImproveBusy", "A calculation is already running."));
		return FReply::Handled();
	}
	if (TrackSamples.Num() == 0 || MeterSamples.Num() == 0)
	{
		Notify(false, LOCTEXT("ImproveNoData", "Import the CSVs first."));
		return FReply::Handled();
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	// Snapshot fixtures (same as HandleCalculateAll).
	FLuxMeasureBatchRequest Request;
	Request.Units = TEXT("cm");
	for (TActorIterator<ALuxIESLightActor> It(World); It; ++It)
	{
		ALuxIESLightActor* L = *It;
		FLuxLightPayload P;
		P.Location = L->GetActorLocation();
		P.Forward  = L->GetActorForwardVector();
		P.Up       = L->GetActorUpVector();
		P.IESFilePath    = L->SourceIESPath;
		P.IntensityLumens = 0.f;
		P.DisplayName    = L->GetActorLabel();
		Request.Lights.Add(P);
	}
	if (Request.Lights.Num() == 0)
	{
		Notify(false, LOCTEXT("ImproveNoFixtures", "Place at least one Lux IES light in the level first."));
		return FReply::Handled();
	}
	// Include reflectors so the sync-search scoring uses the same forward model.
	for (TActorIterator<ALuxReflectorActor> It(World); It; ++It)
	{
		ALuxReflectorActor* R = *It;
		FLuxReflectorPayload RP;
		RP.Location    = R->GetActorLocation();
		RP.Forward     = R->GetActorForwardVector();
		RP.Up          = R->GetActorUpVector();
		RP.WidthCm     = R->Width;
		RP.HeightCm    = R->Height;
		RP.Albedo      = R->Albedo;
		RP.DisplayName = R->GetActorLabel();
		Request.Reflectors.Add(RP);
	}

	constexpr int32 SearchRadius = 60;
	const int32 NumCandidates = 2 * SearchRadius + 1;
	const int32 OffsetBase = TrackSyncFrame - SearchRadius;

	// Map each batch entry back to its (candidate, meter) pair.
	struct FEntry { int32 CandidateIdx; int32 MeterIdx; };
	TArray<FEntry> Entries;
	Entries.Reserve(NumCandidates * MeterSamples.Num());

	for (int32 c = 0; c < NumCandidates; ++c)
	{
		const int32 Sync = OffsetBase + c;
		for (int32 mi = 0; mi < MeterSamples.Num(); ++mi)
		{
			const FResolveResult RR = ResolveTrackForSampleAt(mi, Sync);
			if (!RR.bFound) continue;

			FLuxMeterPayload MP;
			MP.Location = RR.Xform.GetLocation();
			MP.Forward  = RR.Xform.TransformVectorNoScale(FVector::ForwardVector);
			Request.Meters.Add(MP);
			Entries.Add({c, mi});
		}
	}

	if (Request.Meters.Num() == 0)
	{
		Notify(false, LOCTEXT("ImproveNoCoverage",
			"No track samples resolved within +/-60 frames of the current sync. Try widening or moving TrackSyncFrame first."));
		return FReply::Handled();
	}

	bCalculating = true;
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("ImproveCalculating", "Improving sync: testing {0} candidate offsets ({1} poses)..."),
			NumCandidates, Request.Meters.Num()));
	}

	TWeakPtr<SLuxMeterComparisonPanel> WeakSelf = SharedThis(this);
	const TArray<FMeterSample> CapturedMeters = MeterSamples;
	const TArray<FEntry> CapturedEntries = MoveTemp(Entries);
	const int32 CapturedOffsetBase = OffsetBase;
	const int32 CapturedNumCandidates = NumCandidates;
	const int32 ExpectedBatchSize = Request.Meters.Num();

	FLuxCalcClient::MeasureBatch(Request,
		[WeakSelf, CapturedMeters, CapturedEntries, CapturedOffsetBase, CapturedNumCandidates, ExpectedBatchSize]
		(bool bOk, const FLuxMeasureBatchResponse& Response)
		{
			TSharedPtr<SLuxMeterComparisonPanel> Self = WeakSelf.Pin();
			if (!Self.IsValid()) return;
			Self->bCalculating = false;

			if (!bOk)
			{
				Self->Notify(false, FText::Format(
					LOCTEXT("ImproveCalcFailed", "Calculator error: {0}"),
					FText::FromString(Response.Error.IsEmpty() ? TEXT("unknown") : Response.Error)));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("ImproveFailedStatus", "Improve-sync failed."));
				return;
			}
			if (Response.IlluminanceLux.Num() != ExpectedBatchSize)
			{
				Self->Notify(false, LOCTEXT("ImproveSizeMismatch", "Calculator returned a different row count than expected."));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("ImproveFailedStatus", "Improve-sync failed."));
				return;
			}

			// Bucket per-sample |error_pct| by candidate.
			constexpr float MinLuxForErr = 0.1f;
			TArray<TArray<float>> ErrsByCandidate;
			ErrsByCandidate.SetNum(CapturedNumCandidates);
			for (int32 b = 0; b < CapturedEntries.Num(); ++b)
			{
				const FEntry& E = CapturedEntries[b];
				const float Measured = CapturedMeters[E.MeterIdx].Lux;
				if (Measured < MinLuxForErr) continue;
				const float Calc = Response.IlluminanceLux[b];
				const float SignedErr = (Calc - Measured) / FMath::Max(Measured, 1e-3f) * 100.f;
				ErrsByCandidate[E.CandidateIdx].Add(FMath::Abs(SignedErr));
			}

			// 20%-trimmed mean per candidate (drop the largest 20%).
			int32 BestCandidate = INDEX_NONE;
			float BestMean = TNumericLimits<float>::Max();
			int32 BestKept = 0;
			for (int32 c = 0; c < CapturedNumCandidates; ++c)
			{
				TArray<float>& Errs = ErrsByCandidate[c];
				if (Errs.Num() < 5) continue;  // need at least a few samples to be meaningful

				Errs.Sort();
				// Trim the largest 20% of |error| as outliers, average the rest.
				const int32 KeepCount = FMath::Max(1, FMath::FloorToInt(Errs.Num() * 0.80f));
				double Sum = 0.0;
				for (int32 i = 0; i < KeepCount; ++i) Sum += Errs[i];
				const float Mean = static_cast<float>(Sum / KeepCount);

				if (Mean < BestMean)
				{
					BestMean = Mean;
					BestCandidate = c;
					BestKept = KeepCount;
				}
			}

			if (BestCandidate == INDEX_NONE)
			{
				Self->Notify(false, LOCTEXT("ImproveNoCandidate",
					"Not enough resolvable samples across the search window to score a candidate."));
				if (Self->StatusText.IsValid()) Self->StatusText->SetText(LOCTEXT("ImproveFailedStatus", "Improve-sync failed."));
				return;
			}

			const int32 BestSync = CapturedOffsetBase + BestCandidate;
			const int32 OldSync = Self->TrackSyncFrame;
			Self->TrackSyncFrame = BestSync;
			if (Self->MeterListView.IsValid())
			{
				Self->MeterListView->RequestListRefresh();
			}

			const int32 Shift = BestSync - OldSync;
			Self->Notify(true, FText::FromString(FString::Printf(
				TEXT("TrackSyncFrame: %d -> %d (shift %+d). Avg |error| = %.2f%% over %d sample(s)."),
				OldSync, BestSync, Shift, BestMean, BestKept)));
			if (Self->StatusText.IsValid())
			{
				Self->StatusText->SetText(FText::FromString(FString::Printf(
					TEXT("Sync improved: %d -> %d (avg |error| %.2f%% over %d samples, 20%% outliers trimmed)."),
					OldSync, BestSync, BestMean, BestKept)));
			}
		});

	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// Create IES file — reverse-fit a candela distribution from the recording
// ----------------------------------------------------------------------------

FReply SLuxMeterComparisonPanel::HandleCreateIES()
{
	if (TrackSamples.Num() == 0 || MeterSamples.Num() == 0)
	{
		Notify(false, LOCTEXT("CreateIESNoData", "Import the CSVs first."));
		return FReply::Handled();
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FReply::Handled();
	}

	// Exactly one Lux IES light.
	ALuxIESLightActor* Fixture = nullptr;
	int32 FixtureCount = 0;
	for (TActorIterator<ALuxIESLightActor> It(World); It; ++It)
	{
		Fixture = *It;
		++FixtureCount;
	}
	if (FixtureCount != 1 || Fixture == nullptr)
	{
		Notify(false, LOCTEXT("CreateIESNotOneFixture",
			"Need exactly one Lux IES light in the level (found 0 or more than 1)."));
		return FReply::Handled();
	}

	// Reflectors break the assumption that lux = direct emission only.
	// The LS fit can't separate direct vs. reflected contribution from a
	// single fixture, so the fitted IES will fold reflected light into the
	// emission. Warn the user, but proceed (they may want this for a quick fit).
	int32 ReflectorCount = 0;
	for (TActorIterator<ALuxReflectorActor> It(World); It; ++It) ++ReflectorCount;
	if (ReflectorCount > 0)
	{
		Notify(false, LOCTEXT("CreateIESReflectorWarning",
			"Reflector present in level — fitted IES will fold reflected light into the fixture's emission. Remove reflectors for a clean fit."));
	}

	// ---- Output grid: γ = 0..75° in 1° steps (76 values), C = {0, 90, 180, 270, 360}. ----
	constexpr int32 NV = 76;
	constexpr int32 NH_OUT = 5;          // file columns; index 4 mirrors index 0
	constexpr int32 NH_DISTINCT = 4;     // {0, 90, 180, 270}

	// ---- Light's IES local frame (constant across samples). ----
	const FVector LightPos = Fixture->GetActorLocation();
	const FVector LightFwd = Fixture->GetActorForwardVector();
	const FVector LightUp  = Fixture->GetActorUpVector();
	const FVector Nadir = LightFwd.GetSafeNormal();
	FVector Right = FVector::CrossProduct(LightUp, Nadir);
	if (Right.IsNearlyZero(1e-4f))
	{
		const FVector Fallback = FMath::Abs(Nadir.X) < 0.9f ? FVector::ForwardVector : FVector::RightVector;
		Right = FVector::CrossProduct(Fallback, Nadir);
	}
	Right = Right.GetSafeNormal();
	const FVector UpRef = FVector::CrossProduct(Nadir, Right);

	constexpr float MinLuxForCalc = 0.1f;

	// ---- Build sample list — every fit method uses the same prepared data. ----
	TArray<FFitSample> Samples;
	Samples.Reserve(MeterSamples.Num());

	int32 SkippedNoTrack = 0, SkippedDistance = 0, SkippedCosTheta = 0, SkippedGamma = 0, SkippedLowLux = 0;

	for (int32 i = 0; i < MeterSamples.Num(); ++i)
	{
		const FMeterSample& MS = MeterSamples[i];
		if (MS.Lux < MinLuxForCalc) { ++SkippedLowLux; continue; }

		const FResolveResult RR = ResolveTrackForSample(i);
		if (!RR.bFound) { ++SkippedNoTrack; continue; }

		const FVector MeterPos = RR.Xform.GetLocation();
		const FVector MeterFwd = RR.Xform.TransformVectorNoScale(FVector::ForwardVector).GetSafeNormal();

		const FVector DeltaCm = MeterPos - LightPos;
		const float DistanceM = DeltaCm.Size() * 0.01f;
		if (DistanceM < 0.05f) { ++SkippedDistance; continue; }
		const FVector DirWorld = DeltaCm.GetSafeNormal();

		// Direction in IES local frame.
		const float Lx = static_cast<float>(FVector::DotProduct(DirWorld, Right));
		const float Ly = static_cast<float>(FVector::DotProduct(DirWorld, UpRef));
		const float Lz = static_cast<float>(FVector::DotProduct(DirWorld, Nadir));

		const float GammaDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Lz, -1.f, 1.f)));
		if (GammaDeg > 75.f) { ++SkippedGamma; continue; }

		const float CosThetaMeter = -static_cast<float>(FVector::DotProduct(DirWorld, MeterFwd));
		if (CosThetaMeter <= 0.05f) { ++SkippedCosTheta; continue; }

		float CDeg = 0.f;
		if (FMath::Abs(Lx) > 1e-7f || FMath::Abs(Ly) > 1e-7f)
		{
			CDeg = FMath::RadiansToDegrees(FMath::Atan2(Ly, Lx));
			CDeg = FMath::Fmod(CDeg + 360.f, 360.f);
		}

		FFitSample S;
		S.Lux             = MS.Lux;
		S.CosThetaMeter   = CosThetaMeter;
		S.DistanceM       = DistanceM;
		S.Alpha           = CosThetaMeter / (DistanceM * DistanceM);
		S.CandelaEstimate = MS.Lux * DistanceM * DistanceM / CosThetaMeter;
		S.Dx = Lx; S.Dy = Ly; S.Dz = Lz;
		S.GammaDeg        = GammaDeg;
		S.CDeg            = CDeg;
		S.V0 = FMath::Clamp(FMath::FloorToInt(GammaDeg), 0, NV - 1);
		S.V1 = FMath::Min(S.V0 + 1, NV - 1);
		S.Wv = (S.V1 > S.V0) ? FMath::Clamp(GammaDeg - static_cast<float>(S.V0), 0.f, 1.f) : 0.f;
		const int32 H0 = FMath::Clamp(FMath::FloorToInt(CDeg / 90.f), 0, NH_DISTINCT - 1);
		S.H0 = H0;
		S.H1 = (H0 + 1) % NH_DISTINCT;
		S.Wh = FMath::Clamp((CDeg - 90.f * static_cast<float>(H0)) / 90.f, 0.f, 1.f);

		Samples.Add(S);
	}

	if (Samples.Num() == 0)
	{
		Notify(false, FText::Format(
			LOCTEXT("CreateIESNoUsable", "No usable samples after filtering. (track={0}, dist<5cm={1}, cosθ≤0={2}, γ>75°={3}, lux<{4}={5})"),
			SkippedNoTrack, SkippedDistance, SkippedCosTheta, SkippedGamma, FText::AsNumber(MinLuxForCalc), SkippedLowLux));
		return FReply::Handled();
	}

	// ---- Dispatch to the selected fit method. ----
	const EIESFitMethod ActiveMethod = SelectedFitMethod.IsValid()
		? SelectedFitMethod->Method
		: EIESFitMethod::ForwardFitNNLS;

	TArray<TArray<float>> Cells;
	int32 IterDone = 0;  // forward-fit only; 0 for the others
	switch (ActiveMethod)
	{
	case EIESFitMethod::ReverseFit:
		FitReverse(Samples, Cells);
		break;
	case EIESFitMethod::KernelSigma5:
		FitKernel(Samples, /*Sigma=*/5.f, /*CosWeighted=*/false, Cells);
		break;
	case EIESFitMethod::KernelSigma10Cos:
		FitKernel(Samples, /*Sigma=*/10.f, /*CosWeighted=*/true, Cells);
		break;
	case EIESFitMethod::ForwardFitNNLS:
	default:
		IterDone = FitForwardNNLS(Samples, Cells);
		break;
	}

	// ---- Build output grid with the mirror column. ----
	TArray<TArray<float>> Grid;
	Grid.SetNum(NV);
	for (int32 v = 0; v < NV; ++v)
	{
		Grid[v].SetNumUninitialized(NH_OUT);
		for (int32 h = 0; h < NH_DISTINCT; ++h) Grid[v][h] = Cells[v][h];
		Grid[v][4] = Grid[v][0];  // closure
	}

	// ---- RMS residual via forward evaluation through bilinear interp on Cells.
	// Done the same way for every method so the toast/IES header numbers are
	// directly comparable across methods.
	double SumR2 = 0.0;
	for (const FFitSample& S : Samples)
	{
		const float W00 = (1.f - S.Wv) * (1.f - S.Wh);
		const float W01 = (1.f - S.Wv) * S.Wh;
		const float W10 = S.Wv * (1.f - S.Wh);
		const float W11 = S.Wv * S.Wh;
		const float Cd  = W00 * Cells[S.V0][S.H0]
		                + W01 * Cells[S.V0][S.H1]
		                + W10 * Cells[S.V1][S.H0]
		                + W11 * Cells[S.V1][S.H1];
		const float Predicted = Cd * S.Alpha;
		const float R = Predicted - S.Lux;
		SumR2 += static_cast<double>(R) * R;
	}
	const float RmsLux = static_cast<float>(FMath::Sqrt(SumR2 / Samples.Num()));

	// ---- Rough flux estimate: trapezoidal ∫∫ I·sin(γ) dγ dC over the output grid. ----
	double FluxEstimate = 0.0;
	const double DCRad = HALF_PI;  // 90° per distinct C bin (4 bins span 2π)
	const double DvInner = FMath::DegreesToRadians(1.0);
	const double DvEnd   = FMath::DegreesToRadians(0.5);
	for (int32 v = 0; v < NV; ++v)
	{
		const double DvRad = (v == 0 || v == NV - 1) ? DvEnd : DvInner;
		const double SinG = FMath::Sin(FMath::DegreesToRadians(static_cast<double>(v)));
		double RowSum = 0.0;
		for (int32 h = 0; h < NH_DISTINCT; ++h)
		{
			RowSum += Grid[v][h];
		}
		FluxEstimate += DvRad * SinG * DCRad * RowSum;
	}
	const float LumensForHeader = FluxEstimate > 0.0 ? static_cast<float>(FluxEstimate) : -1.f;

	// ---- Compose LM-63 ----
	FString IesText;
	IesText.Reserve(8192);
	const FDateTime NowUtc = FDateTime::UtcNow();
	const FString DateStr = FString::Printf(TEXT("%04d-%02d-%02d"),
		NowUtc.GetYear(), NowUtc.GetMonth(), NowUtc.GetDay());

	IesText += TEXT("IESNA:LM-63-2002\n");
	IesText += FString::Printf(TEXT("[TEST] %s\n"), FitMethodTestLine(ActiveMethod));
	IesText += TEXT("[MANUFAC] LuxMeterCalc\n");
	IesText += FString::Printf(TEXT("[OTHER] Source: lightmeter.csv + track.csv, fitted %s\n"), *DateStr);
	IesText += FString::Printf(TEXT("[OTHER] Samples used: %d, iterations: %d, RMS residual: %.4f lx\n"),
		Samples.Num(), IterDone, RmsLux);
	IesText += TEXT("TILT=NONE\n");

	// num_lamps lumens_per_lamp candela_mult n_vert n_horiz photo_type units width length height
	IesText += FString::Printf(
		TEXT("1 %.4f 1.0 %d %d 1 2 0 0 0\n"),
		LumensForHeader, NV, NH_OUT);
	// ballast_factor future input_watts
	IesText += TEXT("1.0 1.0 100.0\n");
	// Vertical angles (0..75)
	for (int32 v = 0; v < NV; ++v)
	{
		IesText += FString::Printf(TEXT("%s%d"), v == 0 ? TEXT("") : TEXT(" "), v);
	}
	IesText += TEXT("\n");
	// Horizontal angles
	IesText += TEXT("0 90 180 270 360\n");
	// Candela: for each H column, list n_vert values
	auto WriteRow = [&](int32 hCol)
	{
		for (int32 v = 0; v < NV; ++v)
		{
			IesText += FString::Printf(TEXT("%s%.3f"), v == 0 ? TEXT("") : TEXT(" "), Grid[v][hCol]);
		}
		IesText += TEXT("\n");
	};
	for (int32 hCol = 0; hCol < NH_OUT; ++hCol)
	{
		WriteRow(hCol);
	}

	// Output path: next to the lightmeter CSV if known, else project Content.
	const FString OutFolder = MeterPath.IsEmpty()
		? FPaths::ProjectContentDir() / TEXT("TrackingData")
		: FPaths::GetPath(MeterPath);
	const FString OutPath = OutFolder / FString::Printf(TEXT("fitted_%s.ies"), *TimestampSuffix());

	if (!FFileHelper::SaveStringToFile(IesText, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Notify(false, FText::Format(LOCTEXT("CreateIESWriteFailed", "Couldn't write {0}"), FText::FromString(OutPath)));
		return FReply::Handled();
	}

	const FString FluxStr = LumensForHeader > 0
		? FString::Printf(TEXT("%.0f lm"), LumensForHeader)
		: FString(TEXT("flux undetermined (-1)"));
	Notify(true, FText::FromString(FString::Printf(
		TEXT("Wrote %s. %d samples, %d iters, RMS residual=%.2f lx, ~%s."),
		*FPaths::GetCleanFilename(OutPath), Samples.Num(), IterDone, RmsLux, *FluxStr)));
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Saved %s"), *OutPath)));
	}
	return FReply::Handled();
}

// ----------------------------------------------------------------------------
// Output path / timestamp
// ----------------------------------------------------------------------------

FString SLuxMeterComparisonPanel::TimestampSuffix() const
{
	const FDateTime Now = FDateTime::UtcNow();
	return FString::Printf(TEXT("%04d-%02d-%02dT%02d-%02d-%02dZ"),
		Now.GetYear(), Now.GetMonth(), Now.GetDay(),
		Now.GetHour(), Now.GetMinute(), Now.GetSecond());
}

FString SLuxMeterComparisonPanel::DefaultOutputPath() const
{
	const FString Folder = MeterPath.IsEmpty()
		? FPaths::ProjectContentDir() / TEXT("TrackingData")
		: FPaths::GetPath(MeterPath);
	return Folder / FString::Printf(TEXT("comparison_%s.csv"), *TimestampSuffix());
}

// ----------------------------------------------------------------------------
// Notifications
// ----------------------------------------------------------------------------

void SLuxMeterComparisonPanel::Notify(bool bSuccess, const FText& Message)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = bSuccess ? 2.5f : 4.f;
	const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

#undef LOCTEXT_NAMESPACE
