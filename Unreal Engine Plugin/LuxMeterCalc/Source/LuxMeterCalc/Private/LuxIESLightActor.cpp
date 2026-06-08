#include "LuxIESLightActor.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureLightProfile.h"
#include "EditorFramework/AssetImportData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
	// Find any UStaticMesh asset by AssetName anywhere under /LuxMeterCalc/.
	// Robust to flat (FBX) and nested (glTF) importer layouts.
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

ALuxIESLightActor::ALuxIESLightActor()
	: ViewportIntensityLumens(1500.f)
{
	PrimaryActorTick.bCanEverTick = false;

	SpotLight = CreateDefaultSubobject<USpotLightComponent>(TEXT("SpotLight"));
	SetRootComponent(SpotLight);
	SpotLight->SetIntensityUnits(ELightUnits::Lumens);
	SpotLight->SetIntensity(ViewportIntensityLumens);
	SpotLight->SetOuterConeAngle(60.f);
	SpotLight->SetInnerConeAngle(0.f);
	SpotLight->SetAttenuationRadius(2000.f);

	// 3D fixture body — actual mesh assignment is deferred to OnConstruction so we
	// can pick up the user's imported /LuxMeterCalc/Models/FixtureAsset whenever it
	// becomes available, with a graceful fallback to the engine basic cone otherwise.
	FixtureMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FixtureMesh"));
	if (FixtureMesh)
	{
		FixtureMesh->SetupAttachment(SpotLight);
		FixtureMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FixtureMesh->SetCastShadow(false);
	}

#if WITH_EDITORONLY_DATA
	ArrowComponent = CreateDefaultSubobject<UArrowComponent>(TEXT("DirectionArrow"));
	if (ArrowComponent)
	{
		ArrowComponent->SetupAttachment(SpotLight);
		ArrowComponent->ArrowColor = FColor(255, 220, 80);
		ArrowComponent->ArrowSize = 0.5f;
		ArrowComponent->bIsScreenSizeScaled = true;
	}
#endif
}

void ALuxIESLightActor::ApplyIESToLight()
{
	if (!SpotLight)
	{
		return;
	}
	SpotLight->SetIESTexture(IESProfile);
	SpotLight->SetIntensity(ViewportIntensityLumens);
}

void ALuxIESLightActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (FixtureMesh)
	{
		UStaticMesh* Custom = FindCustomMeshByName(FName(TEXT("FixtureAsset")));
		if (Custom)
		{
			// User-authored mesh: assume modeled in UE conventions, no extra fixup.
			if (FixtureMesh->GetStaticMesh() != Custom)
			{
				FixtureMesh->SetStaticMesh(Custom);
				FixtureMesh->SetRelativeRotation(FRotator::ZeroRotator);
				FixtureMesh->SetRelativeScale3D(FVector::OneVector);
			}
		}
		else if (!FixtureMesh->GetStaticMesh())
		{
			// Fallback: engine basic cone, rotated so apex points +X.
			if (UStaticMesh* Fallback = LoadObject<UStaticMesh>(
				nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")))
			{
				FixtureMesh->SetStaticMesh(Fallback);
				FixtureMesh->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
				FixtureMesh->SetRelativeScale3D(FVector(0.2f, 0.2f, 0.2f));
			}
		}
	}

	ApplyIESToLight();
}

#if WITH_EDITOR
void ALuxIESLightActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName ChangedName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: NAME_None;

	if (ChangedName == GET_MEMBER_NAME_CHECKED(ALuxIESLightActor, IESProfile))
	{
		// Keep SourceIESPath (the path the calculator parses) in sync with the asset.
		SourceIESPath.Empty();
		if (IESProfile && IESProfile->AssetImportData)
		{
			TArray<FString> Filenames;
			IESProfile->AssetImportData->ExtractFilenames(Filenames);
			if (Filenames.Num() > 0)
			{
				SourceIESPath = FPaths::ConvertRelativePathToFull(Filenames[0]);
			}
		}
		// Rename the actor label so the Outliner mirrors the assigned profile.
		if (IESProfile)
		{
			SetActorLabel(FString::Printf(TEXT("LuxIES_%s"), *IESProfile->GetName()));
		}
	}

	ApplyIESToLight();
}
#endif
