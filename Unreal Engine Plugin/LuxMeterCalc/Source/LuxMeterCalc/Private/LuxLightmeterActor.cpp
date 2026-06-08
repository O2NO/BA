#include "LuxLightmeterActor.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/ArrowComponent.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Package.h"

namespace
{
	UStaticMesh* FindCustomMeshByName(FName AssetName)
	{
		const FAssetRegistryModule& Reg =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		FARFilter Filter;
		Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(TEXT("/LuxMeterCalc")));
		Filter.bRecursivePaths = true;
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		Reg.Get().GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName == AssetName)
			{
				return Cast<UStaticMesh>(Asset.GetAsset());
			}
		}
		return nullptr;
	}
}

ALuxLightmeterActor::ALuxLightmeterActor()
	: LastIlluminanceLux(0.f)
	, LastFNumber(0.f)
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	// 3D sensor body — actual mesh assignment is deferred to OnConstruction so we
	// can pick up the user's imported /LuxMeterCalc/Models/LightMeter when present,
	// with a graceful fallback to the engine basic cube.
	SensorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SensorMesh"));
	if (SensorMesh)
	{
		SensorMesh->SetupAttachment(SceneRoot);
		SensorMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SensorMesh->SetCastShadow(false);
	}

#if WITH_EDITORONLY_DATA
	ArrowComponent = CreateDefaultSubobject<UArrowComponent>(TEXT("DirectionArrow"));
	if (ArrowComponent)
	{
		ArrowComponent->SetupAttachment(SceneRoot);
		ArrowComponent->ArrowColor = FColor(80, 200, 255);
		ArrowComponent->ArrowSize = 0.6f;
		ArrowComponent->bIsScreenSizeScaled = true;
	}
#endif

	ResultText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ResultText"));
	if (ResultText)
	{
		ResultText->SetupAttachment(SceneRoot);
		// Sit on the model: 3cm in front of the actor origin (+X clears the body),
		// 5cm below origin, sized so a typical reading fills ~7cm of width.
		ResultText->SetRelativeLocation(FVector(2.f, 0.f, -8.f));
		ResultText->SetHorizontalAlignment(EHTA_Center);
		ResultText->SetVerticalAlignment(EVRTA_TextCenter);
		ResultText->SetTextRenderColor(FColor::White);
		ResultText->SetWorldSize(1.8f);
		ResultText->SetText(FText::FromString(TEXT("--- lx")));
	}
}

void ALuxLightmeterActor::SetIlluminance(float Lux)
{
	SetReadout(Lux, LastFNumber);
}

void ALuxLightmeterActor::SetReadout(float Lux, float FNumber)
{
	LastIlluminanceLux = Lux;
	LastFNumber = FNumber;
	if (!ResultText)
	{
		return;
	}

	FString Text = FString::Printf(TEXT("%.1f lx"), Lux);
	if (FNumber > 0.f)
	{
		Text += FString::Printf(TEXT("\nf/%.1f"), FNumber);
	}
	ResultText->SetText(FText::FromString(Text));
}

void ALuxLightmeterActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (SensorMesh)
	{
		UStaticMesh* Custom = FindCustomMeshByName(FName(TEXT("LightMeter")));
		if (Custom)
		{
			if (SensorMesh->GetStaticMesh() != Custom)
			{
				SensorMesh->SetStaticMesh(Custom);
				SensorMesh->SetRelativeRotation(FRotator::ZeroRotator);
				SensorMesh->SetRelativeScale3D(FVector::OneVector);
			}
		}
		else if (!SensorMesh->GetStaticMesh())
		{
			if (UStaticMesh* Fallback = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
			{
				SensorMesh->SetStaticMesh(Fallback);
				SensorMesh->SetRelativeRotation(FRotator::ZeroRotator);
				SensorMesh->SetRelativeScale3D(FVector(0.04f, 0.18f, 0.18f));
			}
		}
	}
}
