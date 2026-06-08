#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LuxMeterCalcSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "LuxMeterCalc"))
class ULuxMeterCalcSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULuxMeterCalcSettings();

	UPROPERTY(EditAnywhere, config, Category = "Calculator")
	FString Host;

	UPROPERTY(EditAnywhere, config, Category = "Calculator", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 Port;

	UPROPERTY(EditAnywhere, config, Category = "Calculator", meta = (ClampMin = "0.1", ClampMax = "120.0"))
	float RequestTimeoutSeconds;

	FString GetBaseUrl() const;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
