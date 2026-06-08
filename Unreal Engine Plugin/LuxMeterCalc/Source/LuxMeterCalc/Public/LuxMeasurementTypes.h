#pragma once

#include "CoreMinimal.h"
#include "LuxMeasurementTypes.generated.h"

USTRUCT(BlueprintType)
struct FLuxLightPayload
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Forward = FVector::ForwardVector;

	UPROPERTY()
	FVector Up = FVector::UpVector;

	UPROPERTY()
	FString IESFilePath;

	UPROPERTY()
	float IntensityLumens = 0.f;

	UPROPERTY()
	FString DisplayName;
};

USTRUCT(BlueprintType)
struct FLuxMeterPayload
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Forward = FVector::ForwardVector;
};

USTRUCT(BlueprintType)
struct FLuxReflectorPayload
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;       // UE world cm

	UPROPERTY()
	FVector Forward = FVector::ForwardVector;     // surface normal

	UPROPERTY()
	FVector Up = FVector::UpVector;               // height axis

	UPROPERTY()
	float WidthCm = 50.f;

	UPROPERTY()
	float HeightCm = 50.f;

	UPROPERTY()
	float Albedo = 0.5f;

	UPROPERTY()
	FString DisplayName;
};

USTRUCT(BlueprintType)
struct FLuxMeasureRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FLuxLightPayload> Lights;

	UPROPERTY()
	FLuxMeterPayload Meter;

	UPROPERTY()
	TArray<FLuxReflectorPayload> Reflectors;

	UPROPERTY()
	FString Units = TEXT("cm");
};

USTRUCT(BlueprintType)
struct FLuxLightDebug
{
	GENERATED_BODY()

	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	float DistanceMeters = 0.f;

	UPROPERTY()
	float GammaDeg = 0.f;

	UPROPERTY()
	float CDeg = 0.f;

	UPROPERTY()
	float Candela = 0.f;

	UPROPERTY()
	float CosineToMeter = 0.f;

	UPROPERTY()
	float ContributionLux = 0.f;
};

USTRUCT(BlueprintType)
struct FLuxMeasureResponse
{
	GENERATED_BODY()

	UPROPERTY()
	float IlluminanceLux = 0.f;

	UPROPERTY()
	TArray<float> PerLightLux;

	UPROPERTY()
	TArray<FLuxLightDebug> Debug;

	UPROPERTY()
	FString Error;
};

USTRUCT(BlueprintType)
struct FLuxMeasureBatchRequest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FLuxLightPayload> Lights;

	UPROPERTY()
	TArray<FLuxMeterPayload> Meters;

	UPROPERTY()
	TArray<FLuxReflectorPayload> Reflectors;

	UPROPERTY()
	FString Units = TEXT("cm");
};

USTRUCT(BlueprintType)
struct FLuxMeasureBatchResponse
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<float> IlluminanceLux;

	UPROPERTY()
	FString Error;
};
