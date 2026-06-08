#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LuxReflectorActor.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UArrowComponent;

/**
 * Lambertian (matte) reflector. Flat rectangle whose +X axis is the surface
 * normal. Width = +Y extent, Height = +Z extent. Sent to the Python calculator
 * with its pose + albedo so the Lambertian bounce contributes to the meter.
 */
UCLASS(Blueprintable, ClassGroup = "LuxMeter", meta = (DisplayName = "Lux Reflector"))
class ALuxReflectorActor : public AActor
{
	GENERATED_BODY()

public:
	ALuxReflectorActor();

	// Reflector dimensions in cm. The visual mesh's relative scale tracks these
	// values so the in-viewport plate matches what the calculator integrates.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|Reflector",
		meta = (ClampMin = "1.0", ClampMax = "500.0"))
	float Width;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|Reflector",
		meta = (ClampMin = "1.0", ClampMax = "500.0"))
	float Height;

	// Lambertian reflectance, 0..1. 0 = matte black, 1 = perfect diffuse white.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LuxMeter|Reflector",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Albedo;

	// Last reflected lux at the lightmeter — set by the panel after a measurement.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LuxMeter|Reflector")
	float LastReflectedLux;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<UStaticMeshComponent> SurfaceMesh;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> ArrowComponent;
#endif

	void ApplyShape();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
};
