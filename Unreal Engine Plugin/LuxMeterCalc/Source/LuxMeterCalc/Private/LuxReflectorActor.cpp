#include "LuxReflectorActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ALuxReflectorActor::ALuxReflectorActor()
	: Width(50.f)
	, Height(50.f)
	, Albedo(0.5f)
	, LastReflectedLux(0.f)
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	SurfaceMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SurfaceMesh"));
	if (SurfaceMesh)
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
			TEXT("/Engine/BasicShapes/Cube"));
		if (CubeMesh.Succeeded())
		{
			SurfaceMesh->SetStaticMesh(CubeMesh.Object);
		}
		SurfaceMesh->SetupAttachment(SceneRoot);
		// Thin in +X (the active reflective face), wide in Y/Z so the plate shape
		// reads at a glance. ApplyShape sets the exact scale to match Width/Height.
		SurfaceMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SurfaceMesh->SetCastShadow(false);
	}

#if WITH_EDITORONLY_DATA
	ArrowComponent = CreateDefaultSubobject<UArrowComponent>(TEXT("DirectionArrow"));
	if (ArrowComponent)
	{
		ArrowComponent->SetupAttachment(SceneRoot);
		ArrowComponent->ArrowColor = FColor(220, 220, 220);
		ArrowComponent->ArrowSize = 0.6f;
		ArrowComponent->bIsScreenSizeScaled = true;
	}
#endif

	ApplyShape();
}

void ALuxReflectorActor::ApplyShape()
{
	if (!SurfaceMesh) return;

	// Engine basic cube is 100 cm on each side. Width controls +Y extent in cm,
	// Height controls +Z. Thickness in X is fixed at 2 cm so the plate has visual
	// substance without dominating the photometric math (which uses Forward as
	// the surface normal of an infinitely-thin plane).
	const float ThicknessCm = 2.f;
	SurfaceMesh->SetRelativeScale3D(FVector(ThicknessCm / 100.f,
											FMath::Max(Width,  1.f) / 100.f,
											FMath::Max(Height, 1.f) / 100.f));
}

void ALuxReflectorActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyShape();
}

#if WITH_EDITOR
void ALuxReflectorActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ApplyShape();
}
#endif
