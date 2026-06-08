#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LuxIESLightActor.generated.h"

class USpotLightComponent;
class UTextureLightProfile;
class UArrowComponent;
class UStaticMeshComponent;

UCLASS(Blueprintable, ClassGroup = "LuxMeter", meta = (DisplayName = "Lux IES Light"))
class ALuxIESLightActor : public AActor
{
	GENERATED_BODY()

public:
	ALuxIESLightActor();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|IES")
	TObjectPtr<UTextureLightProfile> IESProfile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|IES", meta = (FilePathFilter = "ies"))
	FString SourceIESPath;

	// Visual-only previs intensity. Drives the spotlight component's brightness in the
	// editor viewport. NOT sent to the calculator — illuminance is derived from the IES
	// file's photometric data (candela values) regardless of what's set here.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|IES", meta = (ClampMin = "0.0"))
	float ViewportIntensityLumens;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<USpotLightComponent> SpotLight;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<UStaticMeshComponent> FixtureMesh;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> ArrowComponent;
#endif

	void ApplyIESToLight();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
};
