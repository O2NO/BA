#include "MotiveCsvParser.h"

#include "Misc/FileHelper.h"
#include "Serialization/Csv/CsvParser.h"

namespace
{
	bool TryParseFloat(const TCHAR* Cell, float& Out)
	{
		if (!Cell || !*Cell) return false;
		const FString S = FString(Cell).TrimStartAndEnd();
		if (S.IsEmpty()) return false;
		return LexTryParseString(Out, *S);
	}

	FString CellTrim(const TCHAR* Cell)
	{
		if (!Cell) return FString();
		return FString(Cell).TrimStartAndEnd();
	}

	int32 FindFirstNonEmptyRow(const TArray<TArray<const TCHAR*>>& Rows, int32 StartIdx = 0)
	{
		for (int32 i = StartIdx; i < Rows.Num(); ++i)
		{
			for (const TCHAR* Cell : Rows[i])
			{
				if (!CellTrim(Cell).IsEmpty()) return i;
			}
		}
		return INDEX_NONE;
	}

	// Find the row whose first cell is "Frame" — that's the per-axis row of the
	// Motive header block. Returns INDEX_NONE if not found.
	int32 FindAxisRow(const TArray<TArray<const TCHAR*>>& Rows)
	{
		for (int32 i = 0; i < Rows.Num(); ++i)
		{
			if (Rows[i].Num() == 0) continue;
			if (CellTrim(Rows[i][0]).Equals(TEXT("Frame"), ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	// Walk upward from AxisRow looking for the row whose cells include "Rotation"
	// or "Position" — the row that classifies each column. Returns INDEX_NONE
	// if no such row appears.
	int32 FindRotPosRow(const TArray<TArray<const TCHAR*>>& Rows, int32 AxisRow)
	{
		for (int32 i = AxisRow - 1; i >= 0; --i)
		{
			for (const TCHAR* Cell : Rows[i])
			{
				const FString S = CellTrim(Cell);
				if (S.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase) ||
					S.Equals(TEXT("Position"), ESearchCase::IgnoreCase))
				{
					return i;
				}
			}
		}
		return INDEX_NONE;
	}

	int32 FindRowStartingWith(const TArray<TArray<const TCHAR*>>& Rows, int32 LimitExclusive, const FString& Cell1Value)
	{
		for (int32 i = 0; i < LimitExclusive; ++i)
		{
			if (Rows[i].Num() < 2) continue;
			if (CellTrim(Rows[i][1]).Equals(Cell1Value, ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

}

bool MotiveCsvParser::ParseTrackCsv(
	const FString& AbsolutePath,
	float DefaultFps,
	TArray<FTrackSample>& OutSamples,
	FString& OutError)
{
	OutSamples.Reset();
	OutError.Reset();

	FString FileText;
	if (!FFileHelper::LoadFileToString(FileText, *AbsolutePath))
	{
		OutError = FString::Printf(TEXT("Cannot open '%s'"), *AbsolutePath);
		return false;
	}

	const FCsvParser Parser(MoveTemp(FileText));
	const TArray<TArray<const TCHAR*>>& Rows = Parser.GetRows();
	if (Rows.Num() == 0)
	{
		OutError = TEXT("Track CSV is empty.");
		return false;
	}

	// Position values are intentionally stored as written — no mm -> m scale,
	// no axis remap. Downstream consumers (CSV writer, scrubbing) treat them
	// as raw tracker values.

	const int32 AxisRow = FindAxisRow(Rows);
	if (AxisRow == INDEX_NONE)
	{
		OutError = TEXT("Couldn't find a row whose first cell is 'Frame' — is this a Motive default CSV export?");
		return false;
	}

	const int32 RotPosRow = FindRotPosRow(Rows, AxisRow);
	if (RotPosRow == INDEX_NONE)
	{
		OutError = TEXT("Found 'Frame' row but no 'Rotation/Position' classifier row above it.");
		return false;
	}

	const int32 TypeRow = FindRowStartingWith(Rows, AxisRow, TEXT("Type"));
	if (TypeRow == INDEX_NONE)
	{
		OutError = TEXT("Couldn't find the Type row (cell with 'Type' label).");
		return false;
	}
	const int32 NameRow = FindRowStartingWith(Rows, AxisRow, TEXT("Name"));

	const TArray<const TCHAR*>& AxisCells = Rows[AxisRow];
	const TArray<const TCHAR*>& RotPosCells = Rows[RotPosRow];
	const TArray<const TCHAR*>& TypeCells = Rows[TypeRow];

	// Find the first column whose Type cell is exactly "Rigid Body" (NOT
	// "Rigid Body Marker"). That's where the rigid body's 7 axis columns start.
	int32 RBStart = INDEX_NONE;
	for (int32 c = 0; c < TypeCells.Num(); ++c)
	{
		if (CellTrim(TypeCells[c]).Equals(TEXT("Rigid Body"), ESearchCase::IgnoreCase))
		{
			RBStart = c;
			break;
		}
	}
	if (RBStart == INDEX_NONE)
	{
		OutError = TEXT("Track CSV doesn't contain any 'Rigid Body' columns.");
		return false;
	}

	auto MatchAxisCol = [&](const TCHAR* RotOrPos, const TCHAR* AxisLetter) -> int32
	{
		for (int32 c = RBStart; c < TypeCells.Num() && c < RotPosCells.Num() && c < AxisCells.Num(); ++c)
		{
			if (!CellTrim(TypeCells[c]).Equals(TEXT("Rigid Body"), ESearchCase::IgnoreCase)) break;
			if (CellTrim(RotPosCells[c]).Equals(RotOrPos, ESearchCase::IgnoreCase) &&
				CellTrim(AxisCells[c]).Equals(AxisLetter, ESearchCase::IgnoreCase))
			{
				return c;
			}
		}
		return INDEX_NONE;
	};

	const int32 ColRX = MatchAxisCol(TEXT("Rotation"), TEXT("X"));
	const int32 ColRY = MatchAxisCol(TEXT("Rotation"), TEXT("Y"));
	const int32 ColRZ = MatchAxisCol(TEXT("Rotation"), TEXT("Z"));
	const int32 ColRW = MatchAxisCol(TEXT("Rotation"), TEXT("W"));
	const int32 ColTX = MatchAxisCol(TEXT("Position"), TEXT("X"));
	const int32 ColTY = MatchAxisCol(TEXT("Position"), TEXT("Y"));
	const int32 ColTZ = MatchAxisCol(TEXT("Position"), TEXT("Z"));

	if (ColRX == INDEX_NONE || ColRY == INDEX_NONE || ColRZ == INDEX_NONE || ColRW == INDEX_NONE ||
		ColTX == INDEX_NONE || ColTY == INDEX_NONE || ColTZ == INDEX_NONE)
	{
		FString RBName;
		if (NameRow != INDEX_NONE && Rows[NameRow].IsValidIndex(RBStart))
		{
			RBName = CellTrim(Rows[NameRow][RBStart]);
		}
		OutError = FString::Printf(
			TEXT("Couldn't locate Rotation/Position X/Y/Z/W columns for the first rigid body (%s)."),
			RBName.IsEmpty() ? TEXT("unnamed") : *RBName);
		return false;
	}

	// Frame index column is conventionally cell 0; time is cell 1.
	const int32 ColFrame = 0;
	int32 ColTime = INDEX_NONE;
	for (int32 c = 0; c < AxisCells.Num(); ++c)
	{
		const FString S = CellTrim(AxisCells[c]);
		if (S.StartsWith(TEXT("Time"), ESearchCase::IgnoreCase))
		{
			ColTime = c;
			break;
		}
	}

	const float SafeFps = FMath::Max(DefaultFps, 1.f);

	for (int32 r = AxisRow + 1; r < Rows.Num(); ++r)
	{
		const TArray<const TCHAR*>& Row = Rows[r];
		// Skip rows that don't have enough columns to hold this rigid body.
		const int32 MaxNeededCol = FMath::Max3(ColRW, ColTZ, ColTime != INDEX_NONE ? ColTime : ColFrame);
		if (Row.Num() <= MaxNeededCol) continue;

		float qx, qy, qz, qw, tx, ty, tz;
		if (!TryParseFloat(Row[ColRX], qx) ||
			!TryParseFloat(Row[ColRY], qy) ||
			!TryParseFloat(Row[ColRZ], qz) ||
			!TryParseFloat(Row[ColRW], qw) ||
			!TryParseFloat(Row[ColTX], tx) ||
			!TryParseFloat(Row[ColTY], ty) ||
			!TryParseFloat(Row[ColTZ], tz))
		{
			// Dropped frame or gap — skip.
			continue;
		}

		FTrackSample S;
		S.FrameIndex = OutSamples.Num();
		S.Pos_OT     = FVector(tx, ty, tz);
		S.Rot_OT     = FQuat(qx, qy, qz, qw);

		if (ColTime != INDEX_NONE)
		{
			float TimeS;
			if (TryParseFloat(Row[ColTime], TimeS))
			{
				S.TimeSeconds = TimeS;
			}
			else
			{
				S.TimeSeconds = static_cast<float>(S.FrameIndex) / SafeFps;
			}
		}
		else
		{
			S.TimeSeconds = static_cast<float>(S.FrameIndex) / SafeFps;
		}

		// Prefer the Frame column for the index when it parses.
		float FrameF;
		if (TryParseFloat(Row[ColFrame], FrameF))
		{
			S.FrameIndex = FMath::RoundToInt(FrameF);
		}

		OutSamples.Add(S);
	}

	if (OutSamples.Num() == 0)
	{
		OutError = TEXT("Header parsed, but no data rows produced valid track samples.");
		return false;
	}

	return true;
}

bool MotiveCsvParser::ParseLightmeterCsv(
	const FString& AbsolutePath,
	float DefaultFps,
	TArray<FMeterSample>& OutSamples,
	FString& OutError)
{
	OutSamples.Reset();
	OutError.Reset();

	FString FileText;
	if (!FFileHelper::LoadFileToString(FileText, *AbsolutePath))
	{
		OutError = FString::Printf(TEXT("Cannot open '%s'"), *AbsolutePath);
		return false;
	}

	const FCsvParser Parser(MoveTemp(FileText));
	const TArray<TArray<const TCHAR*>>& Rows = Parser.GetRows();

	const int32 FirstRow = FindFirstNonEmptyRow(Rows);
	if (FirstRow == INDEX_NONE)
	{
		OutError = TEXT("Lightmeter CSV is empty.");
		return false;
	}

	// If the first non-empty row contains a non-numeric cell, treat it as a header.
	int32 HeaderIdx = INDEX_NONE;
	{
		float Tmp;
		bool bAllNumeric = true;
		for (const TCHAR* Cell : Rows[FirstRow])
		{
			if (Cell && *Cell && !TryParseFloat(Cell, Tmp))
			{
				bAllNumeric = false;
				break;
			}
		}
		if (!bAllNumeric)
		{
			HeaderIdx = FirstRow;
		}
	}

	int32 ColLux   = INDEX_NONE;
	int32 ColFrame = INDEX_NONE;

	auto FindColumnContains = [](const TArray<const TCHAR*>& Header, const FString& Sub) -> int32
	{
		for (int32 i = 0; i < Header.Num(); ++i)
		{
			if (Header[i] && CellTrim(Header[i]).Contains(Sub, ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
		return INDEX_NONE;
	};

	if (HeaderIdx != INDEX_NONE)
	{
		const TArray<const TCHAR*>& Header = Rows[HeaderIdx];
		ColLux = FindColumnContains(Header, TEXT("lux"));
		if (ColLux == INDEX_NONE) ColLux = FindColumnContains(Header, TEXT("illuminance"));
		if (ColLux == INDEX_NONE) ColLux = FindColumnContains(Header, TEXT("value"));
		ColFrame = FindColumnContains(Header, TEXT("frame"));
	}

	const int32 DataStart = (HeaderIdx == INDEX_NONE) ? FirstRow : HeaderIdx + 1;

	if (ColLux == INDEX_NONE)
	{
		// Inferred path: pick the LAST numeric column, treat FIRST numeric as the frame index.
		for (int32 r = DataStart; r < Rows.Num(); ++r)
		{
			const TArray<const TCHAR*>& Row = Rows[r];
			int32 LastNumeric = INDEX_NONE;
			int32 FirstNumeric = INDEX_NONE;
			float Tmp;
			for (int32 c = 0; c < Row.Num(); ++c)
			{
				if (Row[c] && TryParseFloat(Row[c], Tmp))
				{
					if (FirstNumeric == INDEX_NONE) FirstNumeric = c;
					LastNumeric = c;
				}
			}
			if (LastNumeric != INDEX_NONE)
			{
				ColLux = LastNumeric;
				if (ColFrame == INDEX_NONE && FirstNumeric != LastNumeric)
				{
					ColFrame = FirstNumeric;
				}
				break;
			}
		}
	}

	if (ColLux == INDEX_NONE)
	{
		OutError = TEXT("Couldn't find a lux/value column in the lightmeter CSV.");
		return false;
	}

	const float SafeFps = FMath::Max(DefaultFps, 1.f);

	for (int32 r = DataStart; r < Rows.Num(); ++r)
	{
		const TArray<const TCHAR*>& Row = Rows[r];
		if (Row.Num() <= ColLux)
		{
			continue;
		}
		float Lux;
		if (!TryParseFloat(Row[ColLux], Lux))
		{
			continue;
		}

		FMeterSample S;
		S.Lux = Lux;

		// Frame index: prefer the explicit frame column. Without one, fall back to
		// the row position. We deliberately ignore any seconds/milliseconds columns;
		// time is always derived from frame / Fps so the alignment math stays
		// integer-clean across the whole pipeline.
		float FrameF;
		if (ColFrame != INDEX_NONE && ColFrame < Row.Num() && TryParseFloat(Row[ColFrame], FrameF))
		{
			S.FrameIndex = FMath::RoundToInt(FrameF);
		}
		else
		{
			S.FrameIndex = OutSamples.Num();
		}

		S.TimeSeconds = static_cast<float>(S.FrameIndex) / SafeFps;
		OutSamples.Add(S);
	}

	if (OutSamples.Num() == 0)
	{
		OutError = TEXT("Lightmeter CSV had a recognisable header but no parseable lux rows.");
		return false;
	}

	return true;
}
