"""
Pydantic models that mirror the UE USTRUCTs in LuxMeasurementTypes.h.

UE's FJsonObjectConverter lowercases the first character of every property name,
which produces awkward keys when an acronym leads (e.g. IESFilePath -> iESFilePath).
We accept those exact aliases on input and emit them on output (by_alias=True),
while keeping idiomatic snake_case Python identifiers internally.
"""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field


class Vector3(BaseModel):
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0


class LightPayload(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    location: Vector3
    forward: Vector3
    up: Vector3
    ies_file_path: str = Field(alias="iESFilePath", default="")
    intensity_lumens: float = Field(alias="intensityLumens", default=0.0)
    display_name: str = Field(alias="displayName", default="")


class MeterPayload(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    location: Vector3
    forward: Vector3


class ReflectorPayload(BaseModel):
    """A flat rectangular Lambertian reflector. +X = surface normal."""

    model_config = ConfigDict(populate_by_name=True)

    location: Vector3
    forward: Vector3                                       # surface normal
    up: Vector3                                            # height axis
    width_cm: float = Field(alias="widthCm", default=50.0)
    height_cm: float = Field(alias="heightCm", default=50.0)
    albedo: float = 0.5
    display_name: str = Field(alias="displayName", default="")


class MeasureRequest(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    lights: list[LightPayload] = Field(default_factory=list)
    meter: MeterPayload
    reflectors: list[ReflectorPayload] = Field(default_factory=list)
    units: str = "cm"


class LightDebugDTO(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    display_name: str = Field(alias="displayName", default="")
    distance_meters: float = Field(alias="distanceMeters", default=0.0)
    gamma_deg: float = Field(alias="gammaDeg", default=0.0)
    c_deg: float = Field(alias="cDeg", default=0.0)
    candela: float = 0.0
    cosine_to_meter: float = Field(alias="cosineToMeter", default=0.0)
    contribution_lux: float = Field(alias="contributionLux", default=0.0)


class MeasureResponse(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    illuminance_lux: float = Field(alias="illuminanceLux", default=0.0)
    per_light_lux: list[float] = Field(alias="perLightLux", default_factory=list)
    debug: list[LightDebugDTO] = Field(default_factory=list)
    error: str = ""


class MeasureBatchRequest(BaseModel):
    """One static fixture set evaluated against many meter poses."""

    model_config = ConfigDict(populate_by_name=True)

    lights: list[LightPayload] = Field(default_factory=list)
    meters: list[MeterPayload] = Field(default_factory=list)
    reflectors: list[ReflectorPayload] = Field(default_factory=list)
    units: str = "cm"


class MeasureBatchResponse(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    illuminance_lux: list[float] = Field(alias="illuminanceLux", default_factory=list)
    error: str = ""
