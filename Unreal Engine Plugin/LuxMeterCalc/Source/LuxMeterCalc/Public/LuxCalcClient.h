#pragma once

#include "CoreMinimal.h"
#include "LuxMeasurementTypes.h"

class FLuxCalcClient
{
public:
	using FHealthCallback = TFunction<void(bool /*bOk*/, const FString& /*Version*/)>;
	using FMeasureCallback = TFunction<void(bool /*bOk*/, const FLuxMeasureResponse& /*Response*/)>;
	using FMeasureBatchCallback = TFunction<void(bool /*bOk*/, const FLuxMeasureBatchResponse& /*Response*/)>;

	static void Health(FHealthCallback OnComplete);
	static void Measure(const FLuxMeasureRequest& Request, FMeasureCallback OnComplete);
	static void MeasureBatch(const FLuxMeasureBatchRequest& Request, FMeasureBatchCallback OnComplete);
};
