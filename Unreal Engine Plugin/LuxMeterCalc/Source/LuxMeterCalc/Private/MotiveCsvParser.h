#pragma once

#include "CoreMinimal.h"
#include "ComparisonTypes.h"

namespace MotiveCsvParser
{
	// Parse Motive's default CSV export. Returns true on success, fills OutSamples;
	// on failure returns false, leaves OutSamples cleared, and writes a human-readable
	// reason to OutError.
	//
	// Falls back to FrameIndex / DefaultFps when the file has no Time column.
	bool ParseTrackCsv(
		const FString& AbsolutePath,
		float DefaultFps,
		TArray<FTrackSample>& OutSamples,
		FString& OutError);

	// Permissive parser for the lightmeter recording. Looks for any column named
	// lux/value/illuminance, falling back to the last numeric column. Time is taken
	// from a column named time/t when present, else synthesised as FrameIndex/Fps.
	bool ParseLightmeterCsv(
		const FString& AbsolutePath,
		float DefaultFps,
		TArray<FMeterSample>& OutSamples,
		FString& OutError);
}
