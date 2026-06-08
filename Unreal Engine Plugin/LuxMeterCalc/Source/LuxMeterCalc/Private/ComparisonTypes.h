#pragma once

#include "CoreMinimal.h"

// Internal-only structs for the comparison panel. Plain C++ structs (not USTRUCTs)
// because they never cross the C++/Blueprint boundary or get serialised.

struct FTrackSample
{
	int32   FrameIndex   = 0;        // row index in track.csv (0-based, after header)
	float   TimeSeconds  = 0.f;      // from "Time (s)" column if present, else FrameIndex/TrackFps
	FVector Pos_OT       = FVector::ZeroVector;  // raw position, units as written in the source CSV
	FQuat   Rot_OT       = FQuat::Identity;      // raw quaternion, source frame
};

struct FMeterSample
{
	int32   FrameIndex  = 0;
	float   TimeSeconds = 0.f;
	float   Lux         = 0.f;
};
