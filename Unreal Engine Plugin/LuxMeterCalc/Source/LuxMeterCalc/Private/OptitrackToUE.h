#pragma once

#include "CoreMinimal.h"

namespace OptitrackToUE
{
	// Direct basis swap from Motive (Y-up, right-handed) to UE (Z-up, left-handed).
	//
	//   UE.X =  Motive.Z       (forward)
	//   UE.Y = -Motive.X       (right; negated for handedness)
	//   UE.Z =  Motive.Y       (up)
	//
	// Position also goes from mm -> cm (× 0.1) so the meter actor lands at the
	// real-world distance in UE world space.
	//
	// The comparison CSV writes the raw Pos_OT / Rot_OT values separately, so the
	// saved file still reflects the unmodified tracker readings.
	FORCEINLINE FVector PositionToUE(const FVector& OT_mm)
	{
		return FVector(OT_mm.Z, -OT_mm.X, OT_mm.Y) * 0.1f;
	}

	FORCEINLINE FQuat RotationToUE(const FQuat& OT)
	{
		// (x, y, z, w)_UE = (Motive.z, -Motive.x, Motive.y, -Motive.w)
		return FQuat(OT.Z, -OT.X, OT.Y, -OT.W).GetNormalized();
	}
}
