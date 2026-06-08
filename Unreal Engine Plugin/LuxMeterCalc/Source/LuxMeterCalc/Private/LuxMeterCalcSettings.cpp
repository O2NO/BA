#include "LuxMeterCalcSettings.h"

ULuxMeterCalcSettings::ULuxMeterCalcSettings()
	: Host(TEXT("127.0.0.1"))
	, Port(8765)
	, RequestTimeoutSeconds(10.f)
{
}

FString ULuxMeterCalcSettings::GetBaseUrl() const
{
	return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}
