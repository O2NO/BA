#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LuxLightmeterActor.generated.h"

class UStaticMeshComponent;
class UTextRenderComponent;
class UArrowComponent;
class USceneComponent;

UCLASS(Blueprintable, ClassGroup = "LuxMeter", meta = (DisplayName = "Lux Lightmeter"))
class ALuxLightmeterActor : public AActor
{
	GENERATED_BODY()

public:
	ALuxLightmeterActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LuxMeter")
	float LastIlluminanceLux;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<UStaticMeshComponent> SensorMesh;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> ArrowComponent;
#endif

	UPROPERTY(VisibleAnywhere, Category = "LuxMeter")
	TObjectPtr<UTextRenderComponent> ResultText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LuxMeter")
	float LastFNumber;

	void SetIlluminance(float Lux);
	void SetReadout(float Lux, float FNumber);

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
};
